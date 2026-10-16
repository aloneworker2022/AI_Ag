"""外接裝置（M5StickS3 等）的 WebSocket 通道。

裝置開機後連到 `ws://<pi>:8000/ws/device`，之後所有資料都走這一條長連線。
文字訊息一律是 JSON：`{"type": "...", "id": 1, ...}`。協定細節見 m5stick/README.md。

第 1 階段：用說的 → Pi 語音辨識 → AI 回覆文字顯示在裝置上。
  hello / welcome、audio_start + 二進位 PCM + audio_end / audio_cancel、
  transcribing / transcript、thinking / reply、chat（文字，測試用）、ping / pong、notify、error。
二進位訊息一律屬於「最近一個 *_start」開啟的串流。
後續階段（語音回覆、圖片、感測器、GPIO）只會增加 type，不改既有格式。
"""
from __future__ import annotations

import asyncio
import hmac
import itertools
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

from fastapi import APIRouter, HTTPException, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from .config import get_config
from . import memory, stt

log = logging.getLogger("device")

PROTOCOL_VERSION = 1
HELLO_TIMEOUT_S = 10

router = APIRouter()

ChatHandler = Callable[[str], Awaitable[dict[str, Any]]]
_chat_handler: ChatHandler | None = None


def set_chat_handler(fn: ChatHandler) -> None:
    """由 main.py 注入聊天核心，避免循環 import。"""
    global _chat_handler
    _chat_handler = fn


def _device_cfg() -> dict[str, Any]:
    return get_config().get("device") or {}


@dataclass
class Device:
    name: str
    ws: WebSocket
    fw: str = ""
    connected_at: float = field(default_factory=time.time)
    busy: bool = False
    # 正在收的語音串流
    audio_id: Any = None
    audio_rate: int = 16000
    audio_buf: bytearray = field(default_factory=bytearray)
    audio_truncated: bool = False
    _send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    # Pi 主動發出、等待裝置回覆的請求（給後續 sensor_read / gpio_* 用）
    _pending: dict[int, asyncio.Future] = field(default_factory=dict)

    async def send(self, payload: dict[str, Any]) -> None:
        async with self._send_lock:
            await self.ws.send_text(json.dumps(payload, ensure_ascii=False))


class DeviceHub:
    """目前在線的裝置。同名裝置重連時，舊連線會被取代。"""

    def __init__(self) -> None:
        self.devices: dict[str, Device] = {}
        self._ids = itertools.count(1)

    def get(self, name: str) -> Device:
        dev = self.devices.get(name)
        if not dev:
            raise KeyError(name)
        return dev

    async def request(self, name: str, payload: dict[str, Any],
                      timeout: float = 5.0) -> dict[str, Any]:
        """送指令給裝置並等待回覆（裝置回傳的訊息帶 `re` = 指令 id）。"""
        dev = self.get(name)
        req_id = next(self._ids)
        fut = asyncio.get_running_loop().create_future()
        dev._pending[req_id] = fut
        try:
            await dev.send({**payload, "id": req_id})
            return await asyncio.wait_for(fut, timeout)
        finally:
            dev._pending.pop(req_id, None)


hub = DeviceHub()


def _token_ok(given: Any) -> bool:
    expected = str(_device_cfg().get("token") or "")
    if not expected:
        return True  # 沒設 token = 不驗證（只建議在完全信任的區網這樣用）
    return isinstance(given, str) and hmac.compare_digest(given, expected)


async def _respond(dev: Device, msg_id: Any, get_text: Callable[[], Awaitable[str]]) -> None:
    """共用流程：取得使用者說的話（文字或語音辨識）→ 丟給 AI → 回覆裝置。"""
    if dev.busy:
        await dev.send({"type": "error", "id": msg_id, "message": "上一句還在想，等一下"})
        return
    if _chat_handler is None:
        await dev.send({"type": "error", "id": msg_id, "message": "伺服器尚未就緒"})
        return
    dev.busy = True
    try:
        text = (await get_text()).strip()
        if not text:
            await dev.send({"type": "error", "id": msg_id, "message": "沒聽清楚，再說一次"})
            return
        await dev.send({"type": "thinking", "id": msg_id})
        result = await _chat_handler(text)
        await dev.send({"type": "reply", "id": msg_id, "text": result["reply"]})
    except WebSocketDisconnect:
        raise
    except stt.SttUnavailable as e:
        await dev.send({"type": "error", "id": msg_id, "message": str(e)})
    except Exception as e:  # LLM / 辨識掛掉等狀況，回報給裝置而不是斷線
        log.exception("device respond failed")
        await dev.send({"type": "error", "id": msg_id, "message": f"處理失敗：{e}"})
    finally:
        dev.busy = False


async def _handle_chat(dev: Device, msg: dict[str, Any]) -> None:
    text = str(msg.get("text") or "")

    async def get_text() -> str:
        return text

    await _respond(dev, msg.get("id"), get_text)


async def _handle_voice(dev: Device, msg_id: Any, pcm: bytes, rate: int) -> None:
    async def get_text() -> str:
        await dev.send({"type": "transcribing", "id": msg_id})
        text = await stt.transcribe(pcm, rate)
        log.info("device %s said: %s", dev.name, text)
        if text:
            await dev.send({"type": "transcript", "id": msg_id, "text": text})
        return text

    await _respond(dev, msg_id, get_text)


def _spawn(tasks: set[asyncio.Task], coro) -> None:
    # 丟背景跑，等 LLM / 辨識時仍能收 ping 與下一段資料
    t = asyncio.create_task(coro)
    tasks.add(t)
    t.add_done_callback(tasks.discard)


async def _on_text(dev: Device, msg: dict[str, Any], tasks: set[asyncio.Task]) -> None:
    # 裝置對 Pi 指令的回覆
    re_id = msg.get("re")
    if isinstance(re_id, int) and re_id in dev._pending:
        fut = dev._pending[re_id]
        if not fut.done():
            fut.set_result(msg)
        return

    mtype = msg.get("type")
    if mtype == "ping":
        await dev.send({"type": "pong", "t": msg.get("t")})
    elif mtype == "chat":
        _spawn(tasks, _handle_chat(dev, msg))
    elif mtype == "audio_start":
        if msg.get("format", "pcm_s16le") != "pcm_s16le":
            await dev.send({"type": "error", "id": msg.get("id"), "message": "只支援 pcm_s16le"})
            return
        dev.audio_id = msg.get("id")
        dev.audio_rate = int(msg.get("rate") or 16000)
        dev.audio_buf = bytearray()
        dev.audio_truncated = False
    elif mtype == "audio_end":
        if dev.audio_id is None or msg.get("id") != dev.audio_id:
            return
        pcm, rate, msg_id = bytes(dev.audio_buf), dev.audio_rate, dev.audio_id
        dev.audio_id, dev.audio_buf = None, bytearray()
        _spawn(tasks, _handle_voice(dev, msg_id, pcm, rate))
    elif mtype == "audio_cancel":
        dev.audio_id, dev.audio_buf = None, bytearray()
    else:
        await dev.send({"type": "error", "id": msg.get("id"),
                        "message": f"不支援的 type：{mtype}"})


def _on_binary(dev: Device, data: bytes) -> None:
    if dev.audio_id is None:
        return  # 沒有開啟中的串流，丟掉
    room = stt.max_bytes(dev.audio_rate) - len(dev.audio_buf)
    if room <= 0:
        if not dev.audio_truncated:
            log.warning("device %s audio too long, truncating", dev.name)
            dev.audio_truncated = True
        return
    dev.audio_buf += data[:room]


@router.websocket("/ws/device")
async def device_ws(ws: WebSocket) -> None:
    await ws.accept()

    # 第一則訊息必須是 hello（含 token）
    try:
        raw = await asyncio.wait_for(ws.receive_text(), HELLO_TIMEOUT_S)
        hello = json.loads(raw)
    except (asyncio.TimeoutError, json.JSONDecodeError, WebSocketDisconnect, KeyError):
        await ws.close(code=4400)
        return
    if not isinstance(hello, dict) or hello.get("type") != "hello":
        await ws.close(code=4400)
        return
    if not _token_ok(hello.get("token")):
        await ws.send_text(json.dumps({"type": "error", "message": "token 錯誤"},
                                      ensure_ascii=False))
        await ws.close(code=4401)
        return

    name = str(hello.get("device") or "device")[:32]
    dev = Device(name=name, ws=ws, fw=str(hello.get("fw") or ""))
    old = hub.devices.get(name)
    hub.devices[name] = dev
    if old:
        try:
            await old.ws.close(code=4000)
        except Exception:
            pass
    log.info("device connected: %s fw=%s", name, dev.fw)

    await dev.send({
        "type": "welcome",
        "proto": PROTOCOL_VERSION,
        "companion": memory.get_companion().get("name", "伴侶"),
    })

    tasks: set[asyncio.Task] = set()
    try:
        while True:
            event = await ws.receive()
            if event["type"] == "websocket.disconnect":
                break
            if event.get("bytes") is not None:
                _on_binary(dev, event["bytes"])
                continue
            try:
                msg = json.loads(event.get("text") or "")
            except json.JSONDecodeError:
                await dev.send({"type": "error", "message": "不是 JSON"})
                continue
            if isinstance(msg, dict):
                await _on_text(dev, msg, tasks)
    except WebSocketDisconnect:
        pass
    finally:
        for t in tasks:
            t.cancel()
        for fut in dev._pending.values():
            if not fut.done():
                fut.cancel()
        if hub.devices.get(name) is dev:
            del hub.devices[name]
        log.info("device disconnected: %s", name)


# ---------------------------------------------------------------------------
# HTTP：查看在線裝置、從 Pi 端推訊息到裝置螢幕
# ---------------------------------------------------------------------------
@router.get("/devices")
async def list_devices():
    return [{"name": d.name, "fw": d.fw, "connected_at": d.connected_at, "busy": d.busy}
            for d in hub.devices.values()]


class NotifyIn(BaseModel):
    text: str
    title: str = ""


@router.post("/devices/{name}/notify")
async def notify_device(name: str, body: NotifyIn):
    try:
        dev = hub.get(name)
    except KeyError:
        raise HTTPException(404, "裝置不在線")
    await dev.send({"type": "notify", "title": body.title, "text": body.text})
    return {"ok": True}
