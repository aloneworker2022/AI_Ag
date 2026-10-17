"""M5StickS3 ↔ Hermes Agent 橋接程式（跑在 Raspberry Pi 5）。

Stick 透過 WebSocket 連到 ws://<pi>:3799/ws/device：
  說話（PCM 串流）→ 這裡用 Whisper 轉文字 → 送給 Hermes Agent → 回覆文字傳回 Stick。
協定見 m5stick/README.md。

啟動：python -m bridge   （或 uvicorn bridge.main:app --host 0.0.0.0 --port 3799）
"""
from __future__ import annotations

import asyncio
import hmac
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from . import hermes, stt, voice
from .config import STATE_FILE, section

log = logging.getLogger("bridge")

PROTOCOL_VERSION = 2
HELLO_TIMEOUT_S = 10

app = FastAPI(title="M5StickS3 ↔ Hermes bridge")


# ---------------------------------------------------------------------------
# Hermes session：每支 Stick 一個，存在 state.json，重開 Pi 也接得上
# ---------------------------------------------------------------------------
def _load_state() -> dict[str, Any]:
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def _save_state(state: dict[str, Any]) -> None:
    STATE_FILE.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")


def session_ids(device: str) -> tuple[str, str]:
    """(session_id, session_key)。session_id 換新 = 開新對話；session_key 固定 = 長期記憶同一人。"""
    state = _load_state()
    n = int(state.get("sessions", {}).get(device, 1))
    return f"m5stick-{device}-{n}", f"m5stick:{device}"


def new_session(device: str) -> str:
    state = _load_state()
    sessions = state.setdefault("sessions", {})
    sessions[device] = int(sessions.get(device, 1)) + 1
    _save_state(state)
    return session_ids(device)[0]


# ---------------------------------------------------------------------------
# 裝置：以名稱為單位保留狀態，斷線重連後還是同一個 Device，
# 正在跑的這一輪不會被取消，沒送到的訊息重連後補送。
# ---------------------------------------------------------------------------
OUTBOX_MAX = 20


@dataclass
class Device:
    name: str
    ws: WebSocket | None   # None = 目前離線
    fw: str = ""
    connected_at: float = field(default_factory=time.time)
    busy: bool = False
    _send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    # 正在收的語音串流
    audio_id: Any = None
    audio_rate: int = 16000
    audio_buf: bytearray = field(default_factory=bytearray)
    # 等使用者在 Stick 上按「允許 / 拒絕」
    approval: asyncio.Future | None = None
    # 斷線時沒送到、屬於某一輪（有 id）的訊息，重連後補送
    outbox: list[dict[str, Any]] = field(default_factory=list)
    # 背景跑的辨識 / Hermes 工作，不跟著連線結束
    tasks: set[asyncio.Task] = field(default_factory=set)
    # 目前這一輪（插話時取消）與正在念的語音
    turn_task: asyncio.Task | None = None
    speaker: Any = None
    # Stick 選單裡的「不說話」：False = 只回文字，不合成語音
    voice_on: bool = True

    @property
    def online(self) -> bool:
        return self.ws is not None

    async def send(self, payload: dict[str, Any], queue: bool = True) -> None:
        """送 JSON。離線時，有 id 的訊息（queue=True）先存起來，重連後補送。"""
        async with self._send_lock:
            ws = self.ws
            if ws is not None:
                try:
                    await ws.send_text(json.dumps(payload, ensure_ascii=False))
                    return
                except Exception:  # 連線剛好斷了
                    log.info("%s: send failed, queueing %s", self.name, payload.get("type"))
            if queue and payload.get("id") is not None:
                self.outbox.append(payload)
                del self.outbox[:-OUTBOX_MAX]

    async def send_bytes(self, data: bytes) -> bool:
        """送二進位（語音）。離線就丟掉，回傳 False。"""
        async with self._send_lock:
            if self.ws is None:
                return False
            try:
                await self.ws.send_bytes(data)
                return True
            except Exception:
                return False

    def stop_voice(self) -> None:
        if self.speaker is not None:
            self.speaker.cancel()
            self.speaker = None

    def interrupt(self) -> None:
        """使用者插話：停止念、取消還在跑的上一輪。"""
        self.stop_voice()
        if self.turn_task is not None and not self.turn_task.done():
            self.turn_task.cancel()

    async def flush(self) -> None:
        pending, self.outbox = self.outbox, []
        for payload in pending:
            await self.send(payload)


devices: dict[str, Device] = {}


def _token_ok(given: Any) -> bool:
    expected = str(section("device").get("token") or "")
    if not expected:
        return True
    return isinstance(given, str) and hmac.compare_digest(given, expected)


# ---------------------------------------------------------------------------
# 一輪對話：取得使用者說的話 → Hermes → 回覆
# ---------------------------------------------------------------------------
async def _respond(dev: Device, msg_id: Any, get_text: Callable[[], Awaitable[str]]) -> None:
    for _ in range(40):  # 剛插話時上一輪正在取消，最多等 2 秒讓它收尾
        if not dev.busy:
            break
        await asyncio.sleep(0.05)
    if dev.busy:
        await dev.send({"type": "error", "id": msg_id, "message": "上一句還在處理，等一下"})
        return
    dev.busy = True
    try:
        text = (await get_text()).strip()
        if not text:
            await dev.send({"type": "error", "id": msg_id, "message": "沒聽清楚，再說一次"})
            return

        if text in ("/new", "開新對話", "新對話"):
            new_session(dev.name)
            await dev.send({"type": "reply", "id": msg_id, "text": "好，開始新的對話。"})
            return

        await dev.send({"type": "thinking", "id": msg_id})

        async def on_tool(label: str) -> None:
            await dev.send({"type": "tool", "id": msg_id, "label": label[:80]})

        async def on_status(text: str) -> None:
            await dev.send({"type": "status", "id": msg_id, "text": text[:80]})

        async def on_approval(event: dict[str, Any]) -> str:
            fut = asyncio.get_running_loop().create_future()
            dev.approval = fut
            await dev.send({
                "type": "approval", "id": msg_id,
                "command": str(event.get("command") or "")[:300],
                "description": str(event.get("description") or "")[:200],
            })
            try:
                return await fut
            finally:
                if dev.approval is fut:
                    dev.approval = None

        # 語音模式：回覆一邊產生，一邊切句子合成語音念出來；文字也邊產生邊顯示
        speaker = None
        if voice.enabled() and dev.online and dev.voice_on:
            dev.stop_voice()
            speaker = voice.Speaker(
                msg_id,
                lambda payload: dev.send(payload, queue=False),
                dev.send_bytes)
            dev.speaker = speaker
        splitter = voice.SentenceSplitter()
        streamed = ""
        last_partial = 0.0
        loop = asyncio.get_running_loop()

        async def on_delta(delta: str) -> None:
            nonlocal streamed, last_partial
            streamed += delta
            if speaker is not None:
                for sentence in splitter.feed(delta):
                    speaker.say(sentence)
            if loop.time() - last_partial > 0.4:
                last_partial = loop.time()
                await dev.send({"type": "reply_partial", "id": msg_id,
                                "text": hermes.plain_text(streamed)}, queue=False)

        session_id, session_key = session_ids(dev.name)
        try:
            reply = await hermes.run_turn(
                text, session_id=session_id, session_key=session_key,
                extra_system=voice.style_prompt() if speaker is not None else "",
                cb=hermes.TurnCallbacks(on_tool=on_tool, on_status=on_status,
                                        on_approval=on_approval, on_delta=on_delta))
        except BaseException:
            if speaker is not None:
                speaker.cancel()
            raise
        if speaker is not None:
            for sentence in splitter.flush():
                speaker.say(sentence)
            if not speaker.said_anything and reply:  # 沒有串流到文字（例如舊版 Hermes）
                for sentence in voice.SentenceSplitter().feed(reply + "\n"):
                    speaker.say(sentence)
            speaker.close()
        await dev.send({"type": "reply", "id": msg_id, "text": reply or "（Hermes 沒有回覆文字）"})
    except (hermes.HermesError, stt.SttUnavailable) as e:
        await dev.send({"type": "error", "id": msg_id, "message": str(e)})
    except Exception as e:
        log.exception("respond failed")
        await dev.send({"type": "error", "id": msg_id, "message": f"處理失敗：{e}"})
    finally:
        dev.busy = False


async def _handle_chat(dev: Device, msg: dict[str, Any]) -> None:
    text = str(msg.get("text") or "")

    async def get_text() -> str:
        return text

    await _respond(dev, msg.get("id"), get_text)


def _save_wav(pcm: bytes, rate: int) -> None:
    """診斷用：把最後一段收到的語音存成 bridge/last_input.wav，可以拿去聽看看收音品質。"""
    import wave
    from .config import BRIDGE_DIR
    with wave.open(str(BRIDGE_DIR / "last_input.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


async def _handle_voice(dev: Device, msg_id: Any, pcm: bytes, rate: int) -> None:
    log.info("%s: 收到 %.1f 秒語音", dev.name, len(pcm) / (rate * 2))
    if section("stt").get("save_last_wav"):
        try:
            _save_wav(pcm, rate)
        except OSError as e:
            log.warning("save wav failed: %s", e)

    async def get_text() -> str:
        await dev.send({"type": "transcribing", "id": msg_id})
        text = await stt.transcribe(pcm, rate)
        log.info("%s said: %s", dev.name, text)
        if text:
            await dev.send({"type": "transcript", "id": msg_id, "text": text})
        return text

    await _respond(dev, msg_id, get_text)


def _spawn(tasks: set[asyncio.Task], coro) -> asyncio.Task:
    # 丟背景跑，等 Hermes / 辨識時仍能收 ping、按鍵回覆
    t = asyncio.create_task(coro)
    tasks.add(t)
    t.add_done_callback(tasks.discard)
    return t


async def _on_text(dev: Device, msg: dict[str, Any]) -> None:
    tasks = dev.tasks
    mtype = msg.get("type")
    if mtype == "ping":
        await dev.send({"type": "pong", "t": msg.get("t")})
    elif mtype == "chat":
        dev.interrupt()
        dev.turn_task = _spawn(tasks, _handle_chat(dev, msg))
    elif mtype == "audio_start":
        if msg.get("format", "pcm_s16le") != "pcm_s16le":
            await dev.send({"type": "error", "id": msg.get("id"), "message": "只支援 pcm_s16le"})
            return
        dev.interrupt()  # 使用者開始說話：停止念、取消上一輪
        dev.audio_id = msg.get("id")
        dev.audio_rate = int(msg.get("rate") or 16000)
        dev.audio_buf = bytearray()
    elif mtype == "audio_end":
        if dev.audio_id is None or msg.get("id") != dev.audio_id:
            return
        pcm, rate, msg_id = bytes(dev.audio_buf), dev.audio_rate, dev.audio_id
        dev.audio_id, dev.audio_buf = None, bytearray()
        dev.turn_task = _spawn(tasks, _handle_voice(dev, msg_id, pcm, rate))
    elif mtype == "audio_cancel":
        dev.audio_id, dev.audio_buf = None, bytearray()
    elif mtype == "tts_stop":
        dev.stop_voice()
    elif mtype == "voice_mode":
        dev.voice_on = bool(msg.get("enabled", True))
        if not dev.voice_on:
            dev.stop_voice()
        log.info("%s voice %s", dev.name, "on" if dev.voice_on else "off")
    elif mtype == "approval_reply":
        if dev.approval and not dev.approval.done():
            dev.approval.set_result("once" if msg.get("choice") == "once" else "deny")
    elif mtype == "new_session":
        sid = new_session(dev.name)
        await dev.send({"type": "notify", "title": "新對話", "text": "已開始新的對話。"})
        log.info("%s new session %s", dev.name, sid)
    else:
        await dev.send({"type": "error", "id": msg.get("id"), "message": f"不支援的 type：{mtype}"})


def _on_binary(dev: Device, data: bytes) -> None:
    if dev.audio_id is None:
        return
    room = stt.max_bytes(dev.audio_rate) - len(dev.audio_buf)
    if room > 0:
        dev.audio_buf += data[:room]


@app.websocket("/ws/device")
async def device_ws(ws: WebSocket) -> None:
    await ws.accept()
    try:
        hello = json.loads(await asyncio.wait_for(ws.receive_text(), HELLO_TIMEOUT_S))
    except (asyncio.TimeoutError, json.JSONDecodeError, WebSocketDisconnect, KeyError):
        await ws.close(code=4400)
        return
    if not isinstance(hello, dict) or hello.get("type") != "hello":
        await ws.close(code=4400)
        return
    if not _token_ok(hello.get("token")):
        await ws.send_text(json.dumps({"type": "error", "message": "token 錯誤"}, ensure_ascii=False))
        await ws.close(code=4401)
        return

    name = str(hello.get("device") or "stick")[:32]
    dev = devices.get(name)
    if dev is None:
        dev = Device(name=name, ws=ws)
        devices[name] = dev
    else:
        old_ws, dev.ws = dev.ws, ws
        if old_ws is not None:
            try:
                await old_ws.close(code=4000)
            except Exception:
                pass
    dev.fw = str(hello.get("fw") or "")
    dev.voice_on = bool(hello.get("voice", True))
    dev.connected_at = time.time()
    dev.audio_id, dev.audio_buf = None, bytearray()  # 斷在說話中途的那段丟掉
    log.info("device connected: %s fw=%s (補送 %d 則)", name, dev.fw, len(dev.outbox))
    await dev.send({"type": "welcome", "proto": PROTOCOL_VERSION, "agent": "Hermes"})
    await dev.flush()

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
                await _on_text(dev, msg)
    except WebSocketDisconnect:
        pass
    finally:
        # 正在跑的這一輪繼續跑完，回覆等 Stick 重連後補送；
        # 等核准的指令則交給 approval_timeout 處理。
        if dev.ws is ws:
            dev.ws = None
            dev.stop_voice()  # 離線了就不用再念
        log.info("device disconnected: %s (進行中 %d 項)", name, len(dev.tasks))


# ---------------------------------------------------------------------------
# HTTP：狀態、推訊息到 Stick、開新對話
# ---------------------------------------------------------------------------
@app.get("/health")
async def health():
    return {"ok": True, "devices": sum(d.online for d in devices.values())}


@app.get("/devices")
async def list_devices():
    return [{"name": d.name, "fw": d.fw, "online": d.online, "connected_at": d.connected_at,
             "busy": d.busy, "session": session_ids(d.name)[0]} for d in devices.values()]


class NotifyIn(BaseModel):
    text: str
    title: str = ""


@app.post("/devices/{name}/notify")
async def notify_device(name: str, body: NotifyIn):
    dev = devices.get(name)
    if not dev or not dev.online:
        raise HTTPException(404, "裝置不在線")
    await dev.send({"type": "notify", "title": body.title, "text": body.text})
    return {"ok": True}


@app.post("/devices/{name}/new-session")
async def new_session_http(name: str):
    return {"session": new_session(name)}
