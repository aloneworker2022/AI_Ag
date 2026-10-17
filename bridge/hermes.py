"""Hermes Agent API server 的客戶端（OpenAI 相容的 /v1/chat/completions，串流）。

- 對話歷史由 Hermes 保存：每次只送最新一句，用 X-Hermes-Session-Id 接續同一個 session。
- X-Hermes-Session-Key 固定不變，讓 Hermes 的長期記憶認得「這是同一支 Stick」。
- 串流中的 hermes.tool.progress（用了什麼工具）、hermes.status、approval.request
  （指令需要核准）會透過 callback 交給呼叫端轉給 Stick。
"""
from __future__ import annotations

import asyncio
import json
import logging
import re
from dataclasses import dataclass
from typing import Any, Awaitable, Callable

import httpx

from .config import section

log = logging.getLogger("bridge.hermes")


class HermesError(RuntimeError):
    pass


@dataclass
class TurnCallbacks:
    on_tool: Callable[[str], Awaitable[None]]
    on_status: Callable[[str], Awaitable[None]]
    # 收到核准請求：回傳 "once" 或 "deny"
    on_approval: Callable[[dict[str, Any]], Awaitable[str]]
    # 回覆文字一邊產生一邊交出來（語音模式用來一句一句先念）
    on_delta: Callable[[str], Awaitable[None]] | None = None


def _cfg() -> dict[str, Any]:
    return section("hermes")


def _headers(session_id: str, session_key: str) -> dict[str, str]:
    h = {"Content-Type": "application/json", "Accept": "text/event-stream",
         "X-Hermes-Session-Id": session_id, "X-Hermes-Session-Key": session_key}
    key = str(_cfg().get("api_key") or "")
    if key:
        h["Authorization"] = f"Bearer {key}"
    return h


def _error_message(status: int, body: bytes) -> str:
    try:
        data = json.loads(body)
        err = data.get("error") if isinstance(data, dict) else None
        if isinstance(err, dict) and err.get("message"):
            return f"Hermes 回應 {status}：{err['message']}"
    except (json.JSONDecodeError, UnicodeDecodeError):
        pass
    if status == 401:
        return "Hermes 金鑰錯誤（hermes.api_key 要跟 API_SERVER_KEY 一樣）"
    return f"Hermes 回應 {status}"


_MD_PATTERNS = [
    (re.compile(r"```[a-zA-Z0-9_-]*\n?"), ""),   # 程式碼區塊標記
    (re.compile(r"\*\*(.+?)\*\*"), r"\1"),      # 粗體
    (re.compile(r"(?m)^#{1,6}\s*"), ""),         # 標題
    (re.compile(r"`([^`]+)`"), r"\1"),           # 行內程式碼
    (re.compile(r"\[([^\]]+)\]\((https?://[^)]+)\)"), r"\1 \2"),  # 連結
]


def plain_text(text: str) -> str:
    """把常見 Markdown 拿掉，小螢幕比較好讀。"""
    for pat, rep in _MD_PATTERNS:
        text = pat.sub(rep, text)
    return text.strip()


async def _approve(client: httpx.AsyncClient, event: dict[str, Any], choice: str,
                   session_id: str, session_key: str) -> None:
    run_id = event.get("run_id")
    if not run_id:
        return
    body: dict[str, Any] = {"choice": choice}
    if event.get("request_id"):
        body["request_id"] = event["request_id"]
    if event.get("execution_generation") is not None:
        body["execution_generation"] = event["execution_generation"]
    url = f"{_cfg()['base_url'].rstrip('/')}/runs/{run_id}/approval"
    headers = _headers(session_id, session_key)
    headers["Accept"] = "application/json"
    try:
        r = await client.post(url, json=body, headers=headers, timeout=15)
        if r.status_code >= 400:
            log.warning("approval %s for %s failed: %s %s", choice, run_id, r.status_code, r.text[:200])
    except httpx.HTTPError as e:
        log.warning("approval request failed: %s", e)


async def run_turn(text: str, *, session_id: str, session_key: str, cb: TurnCallbacks,
                   extra_system: str = "") -> str:
    """送一句話給 Hermes，等它（可能用了一堆工具後）回覆，回傳最終文字。"""
    cfg = _cfg()
    url = f"{cfg['base_url'].rstrip('/')}/chat/completions"
    messages = []
    system = "\n".join(x for x in (cfg.get("system_prompt"), extra_system) if x)
    if system:
        messages.append({"role": "system", "content": system})
    messages.append({"role": "user", "content": text})
    body = {"model": cfg.get("model", "hermes-agent"), "messages": messages, "stream": True}

    timeout = httpx.Timeout(connect=10, read=float(cfg.get("read_timeout", 120)), write=30, pool=10)
    parts: list[str] = []
    finish: str | None = None
    approval_tasks: set[asyncio.Task] = set()

    async def handle_approval(client: httpx.AsyncClient, event: dict[str, Any]) -> None:
        try:
            choice = await asyncio.wait_for(cb.on_approval(event),
                                            float(cfg.get("approval_timeout", 120)))
        except asyncio.TimeoutError:
            choice = "deny"
        await _approve(client, event, choice if choice in ("once", "deny") else "deny",
                       session_id, session_key)

    try:
        async with httpx.AsyncClient(timeout=timeout) as client:
            async with client.stream("POST", url, json=body,
                                     headers=_headers(session_id, session_key)) as r:
                if r.status_code != 200:
                    raise HermesError(_error_message(r.status_code, await r.aread()))
                event_name = ""
                async for line in r.aiter_lines():
                    if not line:
                        event_name = ""  # 空行 = 一個 SSE 事件結束
                        continue
                    if line.startswith(":"):
                        continue  # keepalive
                    if line.startswith("event:"):
                        event_name = line[6:].strip()
                        continue
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    try:
                        payload = json.loads(data)
                    except json.JSONDecodeError:
                        continue

                    if event_name == "hermes.tool.progress":
                        if payload.get("status") == "running":
                            await cb.on_tool(str(payload.get("label") or payload.get("tool") or "工具"))
                    elif event_name == "hermes.status":
                        msg = payload.get("text") or payload.get("message")
                        if msg:
                            await cb.on_status(str(msg))
                    elif event_name == "approval.request":
                        t = asyncio.create_task(handle_approval(client, payload))
                        approval_tasks.add(t)
                        t.add_done_callback(approval_tasks.discard)
                    else:
                        if isinstance(payload.get("error"), dict):
                            raise HermesError(str(payload["error"].get("message") or "Hermes 錯誤"))
                        for choice in payload.get("choices") or []:
                            delta = choice.get("delta") or {}
                            if delta.get("content"):
                                parts.append(delta["content"])
                                if cb.on_delta is not None:
                                    await cb.on_delta(delta["content"])
                            if choice.get("finish_reason"):
                                finish = choice["finish_reason"]
    except httpx.ConnectError as e:
        raise HermesError("連不到 Hermes（hermes gateway 有在跑嗎？）") from e
    except httpx.ReadTimeout as e:
        raise HermesError("Hermes 太久沒有回應") from e
    finally:
        for t in approval_tasks:
            t.cancel()

    reply = plain_text("".join(parts))
    if finish in ("error", "cancelled") and not reply:
        raise HermesError("Hermes 執行失敗" if finish == "error" else "Hermes 中斷了這一輪")
    return reply
