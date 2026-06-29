"""ComfyUI API client。對接既有的本機 ComfyUI 服務。

流程：載入 workflow 範本 → 填入正向 prompt（與可選的 checkpoint）→
POST /prompt → 輪詢 /history/{id} → /view 取圖存檔。
"""
from __future__ import annotations

import copy
import json
import uuid
from pathlib import Path

import httpx

from .config import DREAM_IMAGES_DIR, ROOT, get_config


def _cfg() -> dict:
    return get_config()["comfyui"]


def _base() -> str:
    return _cfg()["base_url"].rstrip("/")


def _load_workflow() -> dict:
    wf_path = ROOT / _cfg()["workflow"]
    return json.loads(wf_path.read_text(encoding="utf-8"))


def _inject_prompt(workflow: dict, positive: str) -> dict:
    """把正向 prompt 填進 workflow。

    慣例（見 comfy_workflow.json）：
      - 正向 CLIPTextEncode 節點的 _meta.title 含 "Positive"
      - checkpoint 節點 class_type == "CheckpointLoaderSimple"
    """
    wf = copy.deepcopy(workflow)
    checkpoint = _cfg().get("checkpoint") or ""
    for node in wf.values():
        meta_title = node.get("_meta", {}).get("title", "")
        if node.get("class_type") == "CLIPTextEncode" and "Positive" in meta_title:
            node["inputs"]["text"] = positive
        if checkpoint and node.get("class_type") == "CheckpointLoaderSimple":
            node["inputs"]["ckpt_name"] = checkpoint
        # 每次換個 seed，避免每張夢都一樣
        if node.get("class_type") == "KSampler":
            node["inputs"]["seed"] = uuid.uuid4().int % (2 ** 32)
    return wf


async def generate(positive_prompt: str, filename_hint: str = "dream") -> Path | None:
    """產生一張圖，存到 dreams/images/，回傳存檔路徑（失敗回 None）。"""
    base = _base()
    workflow = _inject_prompt(_load_workflow(), positive_prompt)
    client_id = str(uuid.uuid4())
    try:
        async with httpx.AsyncClient(timeout=600) as client:
            resp = await client.post(f"{base}/prompt",
                                     json={"prompt": workflow, "client_id": client_id})
            resp.raise_for_status()
            prompt_id = resp.json()["prompt_id"]

            # 輪詢 history 直到該 prompt 完成
            image_info = await _wait_for_image(client, base, prompt_id)
            if not image_info:
                return None

            img_resp = await client.get(f"{base}/view", params=image_info)
            img_resp.raise_for_status()
            DREAM_IMAGES_DIR.mkdir(parents=True, exist_ok=True)
            out = DREAM_IMAGES_DIR / f"{filename_hint}_{uuid.uuid4().hex[:8]}.png"
            out.write_bytes(img_resp.content)
            return out
    except (httpx.HTTPError, KeyError, json.JSONDecodeError):
        return None


async def _wait_for_image(client: httpx.AsyncClient, base: str, prompt_id: str,
                          max_polls: int = 300) -> dict | None:
    import asyncio
    for _ in range(max_polls):
        await asyncio.sleep(2)
        try:
            h = await client.get(f"{base}/history/{prompt_id}")
            h.raise_for_status()
            data = h.json()
        except httpx.HTTPError:
            continue
        if prompt_id not in data:
            continue
        outputs = data[prompt_id].get("outputs", {})
        for node_out in outputs.values():
            for img in node_out.get("images", []):
                return {
                    "filename": img["filename"],
                    "subfolder": img.get("subfolder", ""),
                    "type": img.get("type", "output"),
                }
        return None  # 完成但沒有圖
    return None


async def is_alive() -> bool:
    try:
        async with httpx.AsyncClient(timeout=5) as client:
            r = await client.get(f"{_base()}/system_stats")
            return r.status_code == 200
    except httpx.HTTPError:
        return False
