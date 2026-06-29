"""做夢系統。

睡覺時段內，自動把記憶裡「還沒畫過的對象」（人物/場景/事件）丟給 ComfyUI 畫出來，
並在 dreams/index.json 建立「對象 → 圖片」的關聯。
之後聊天提到該對象，就能把夢到的圖貼到畫面上。
"""
from __future__ import annotations

import json
from datetime import datetime, time
from pathlib import Path
from typing import Any

import frontmatter

from . import comfyui
from .config import (DREAM_IMAGES_DIR, DREAM_INDEX_FILE, MEMORY_DIR, get_config)

# 做夢時的全域狀態（給 /status 用）
STATE: dict[str, Any] = {"dreaming": False, "current_subject": None}


# ---------------------------------------------------------------------------
# index.json：對象路徑 -> 圖片資訊
# ---------------------------------------------------------------------------
def load_index() -> dict[str, Any]:
    if DREAM_INDEX_FILE.exists():
        try:
            return json.loads(DREAM_INDEX_FILE.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}
    return {}


def save_index(idx: dict[str, Any]) -> None:
    DREAM_INDEX_FILE.parent.mkdir(parents=True, exist_ok=True)
    DREAM_INDEX_FILE.write_text(json.dumps(idx, ensure_ascii=False, indent=2),
                                encoding="utf-8")


def image_for(mem_path: str) -> str | None:
    """給一個記憶相對路徑，回傳其夢境圖的 URL（給前端用），無則 None。"""
    entry = load_index().get(mem_path)
    if entry:
        return f"/images/{Path(entry['image']).name}"
    return None


def gallery() -> list[dict[str, Any]]:
    idx = load_index()
    out = []
    for mem_path, entry in idx.items():
        out.append({
            "subject": entry.get("name", mem_path),
            "memory_path": mem_path,
            "image": f"/images/{Path(entry['image']).name}",
            "prompt": entry.get("prompt", ""),
            "dreamed_at": entry.get("dreamed_at", ""),
        })
    out.sort(key=lambda x: x["dreamed_at"], reverse=True)
    return out


# ---------------------------------------------------------------------------
# 睡覺時段判斷
# ---------------------------------------------------------------------------
def _parse(t: str) -> time:
    h, m = t.split(":")
    return time(int(h), int(m))


def is_sleep_time(now: datetime | None = None) -> bool:
    cfg = get_config()["dream"]
    now = now or datetime.now()
    start = _parse(cfg["sleep_start"])
    end = _parse(cfg["sleep_end"])
    cur = now.time()
    if start <= end:
        return start <= cur < end
    # 跨午夜，例如 23:00 -> 07:00
    return cur >= start or cur < end


# ---------------------------------------------------------------------------
# 挑出還沒畫過的對象
# ---------------------------------------------------------------------------
def _pending_subjects() -> list[dict[str, str]]:
    idx = load_index()
    pending = []
    for cat in ("characters", "scenes", "events"):
        for md in sorted((MEMORY_DIR / cat).glob("*.md")):
            rel = f"{cat}/{md.name}"
            if rel in idx:
                continue
            post = frontmatter.load(md)
            appearance = str(post.get("appearance", "")).strip()
            desc = appearance or post.content.strip()[:200]
            if not desc:
                continue
            pending.append({
                "path": rel,
                "name": str(post.get("name", md.stem)),
                "prompt": desc,
            })
    return pending


def _build_prompt(subject: dict[str, str]) -> str:
    """組 ComfyUI 正向 prompt。加一點夢境風格的字。"""
    base = subject["prompt"]
    return f"{base}, dreamlike, soft lighting, detailed, anime style, masterpiece"


# ---------------------------------------------------------------------------
# 主流程：跑一輪夢
# ---------------------------------------------------------------------------
async def run_dream_cycle(force: bool = False) -> dict[str, Any]:
    """檢查一次：若該睡覺（或 force），畫掉一批還沒畫的對象。"""
    if not force and not is_sleep_time():
        return {"ran": False, "reason": "not sleep time"}

    pending = _pending_subjects()
    if not pending:
        return {"ran": False, "reason": "nothing to dream"}

    if not await comfyui.is_alive():
        return {"ran": False, "reason": "comfyui not reachable"}

    max_n = get_config()["dream"]["max_dreams_per_night"]
    idx = load_index()
    dreamed = []
    STATE["dreaming"] = True
    try:
        for subject in pending[:max_n]:
            STATE["current_subject"] = subject["name"]
            prompt = _build_prompt(subject)
            img = await comfyui.generate(prompt, filename_hint=subject["path"].split("/")[0])
            if img:
                idx[subject["path"]] = {
                    "name": subject["name"],
                    "image": str(img),
                    "prompt": prompt,
                    "dreamed_at": datetime.now().isoformat(timespec="seconds"),
                }
                save_index(idx)
                dreamed.append(subject["name"])
    finally:
        STATE["dreaming"] = False
        STATE["current_subject"] = None

    return {"ran": True, "dreamed": dreamed}


def status() -> dict[str, Any]:
    return {
        "sleeping": is_sleep_time(),
        "dreaming": STATE["dreaming"],
        "current_subject": STATE["current_subject"],
    }
