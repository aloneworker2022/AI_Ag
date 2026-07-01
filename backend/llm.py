"""Ollama chat client。純本地，透過 httpx 呼叫本機 Ollama。"""
from __future__ import annotations

import json
from typing import Any

import httpx

from .config import get_config


def _ollama_cfg() -> dict[str, Any]:
    return get_config()["ollama"]


async def chat(messages: list[dict[str, str]], model: str | None = None,
               temperature: float = 0.8) -> str:
    """送一串 messages 給 Ollama，回傳純文字回覆。

    messages 格式同 OpenAI：[{"role": "system|user|assistant", "content": "..."}]
    """
    cfg = _ollama_cfg()
    url = f"{cfg['base_url'].rstrip('/')}/api/chat"
    payload = {
        "model": model or cfg["model"],
        "messages": messages,
        "stream": False,
        "options": {"temperature": temperature},
    }
    async with httpx.AsyncClient(timeout=120) as client:
        resp = await client.post(url, json=payload)
        resp.raise_for_status()
        data = resp.json()
    return data.get("message", {}).get("content", "").strip()


async def chat_json(messages: list[dict[str, str]], model: str | None = None) -> Any:
    """要求模型回傳 JSON 並解析。失敗時回傳 None（呼叫端自行處理）。"""
    cfg = _ollama_cfg()
    url = f"{cfg['base_url'].rstrip('/')}/api/chat"
    payload = {
        "model": model or cfg.get("extract_model") or cfg["model"],
        "messages": messages,
        "stream": False,
        "format": "json",          # Ollama 原生支援強制 JSON 輸出
        "options": {"temperature": 0.2},
    }
    try:
        async with httpx.AsyncClient(timeout=120) as client:
            resp = await client.post(url, json=payload)
            resp.raise_for_status()
            content = resp.json().get("message", {}).get("content", "")
        return json.loads(content)
    except (httpx.HTTPError, json.JSONDecodeError, KeyError):
        return None


async def check_connection() -> tuple[bool, list[str]]:
    """測試 Ollama 連線，回傳 (是否可連, 該主機已安裝的模型清單)。"""
    cfg = _ollama_cfg()
    url = f"{cfg['base_url'].rstrip('/')}/api/tags"
    try:
        async with httpx.AsyncClient(timeout=5) as client:
            resp = await client.get(url)
            resp.raise_for_status()
            data = resp.json()
        return True, [m["name"] for m in data.get("models", [])]
    except (httpx.HTTPError, KeyError, json.JSONDecodeError):
        return False, []
