"""文字轉語音：把 Hermes 的回覆念出來，傳給 Stick 播放。

預設用 edge-tts（微軟 Edge 的線上語音，免費、不用金鑰，需要網路），
再用 ffmpeg 把 MP3 轉成 Stick 能直接播的 16-bit 單聲道 PCM。
長回覆會切成句子一句一句合成，第一句好了就先開始播。
"""
from __future__ import annotations

import asyncio
import logging
import re
from typing import Any, AsyncIterator

from .config import section

log = logging.getLogger("bridge.tts")


class TtsError(RuntimeError):
    pass


def _cfg() -> dict[str, Any]:
    return section("tts")


def enabled() -> bool:
    return bool(_cfg().get("enabled", True))


def sample_rate() -> int:
    return int(_cfg().get("sample_rate", 16000))


_URL = re.compile(r"https?://\S+")
# 表情符號、各種圖形符號：念出來很怪，直接拿掉
_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")
_SENTENCE = re.compile(r"[^。！？!?；;\n]+[。！？!?；;]*")


def clean(text: str) -> str:
    text = _URL.sub("", text)
    text = _EMOJI.sub("", text)
    text = re.sub(r"[*#`>_~|]", "", text)
    return re.sub(r"\s+", " ", text).strip()


def split_sentences(text: str, min_len: int = 12, max_len: int = 120) -> list[str]:
    """切成適合一次合成的段落：太短的併到下一句，太長的硬切。"""
    parts: list[str] = []
    buf = ""
    for m in _SENTENCE.finditer(text):
        s = m.group().strip()
        if not s:
            continue
        buf += s
        if len(buf) >= min_len:
            parts.append(buf)
            buf = ""
    if buf:
        if parts and len(buf) < min_len:
            parts[-1] += buf
        else:
            parts.append(buf)
    out: list[str] = []
    for p in parts:
        while len(p) > max_len:
            out.append(p[:max_len])
            p = p[max_len:]
        if p:
            out.append(p)
    return out


async def _edge_mp3(text: str) -> bytes:
    try:
        import edge_tts
    except ImportError as e:
        raise TtsError("Pi 上沒裝 edge-tts（pip install edge-tts）") from e
    cfg = _cfg()
    comm = edge_tts.Communicate(
        text,
        cfg.get("voice", "zh-TW-HsiaoChenNeural"),
        rate=str(cfg.get("rate", "+0%")),
        volume=str(cfg.get("volume", "+0%")),
        pitch=str(cfg.get("pitch", "+0Hz")),
    )
    mp3 = bytearray()
    async for chunk in comm.stream():
        if chunk.get("type") == "audio" and chunk.get("data"):
            mp3 += chunk["data"]
    return bytes(mp3)


async def _to_pcm(mp3: bytes, rate: int) -> bytes:
    try:
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", "pipe:0",
            "-f", "s16le", "-ac", "1", "-ar", str(rate), "pipe:1",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE)
    except FileNotFoundError as e:
        raise TtsError("Pi 上沒裝 ffmpeg（sudo apt install ffmpeg）") from e
    pcm, err = await proc.communicate(mp3)
    if proc.returncode != 0:
        raise TtsError(f"ffmpeg 轉檔失敗：{err.decode(errors='ignore')[:200]}")
    return pcm


async def synthesize(text: str, rate: int) -> bytes:
    """一段文字 → 16-bit little-endian 單聲道 PCM。"""
    mp3 = await _edge_mp3(text)
    if not mp3:
        return b""
    return await _to_pcm(mp3, rate)


async def stream(text: str) -> AsyncIterator[bytes]:
    """邊合成邊吐 PCM：背景先合成下一句，播放這一句時不用乾等。"""
    text = clean(text)[: int(_cfg().get("max_chars", 400))]
    sentences = split_sentences(text)
    if not sentences:
        return
    rate = sample_rate()
    queue: asyncio.Queue = asyncio.Queue(maxsize=2)

    async def producer() -> None:
        try:
            for s in sentences:
                await queue.put(await synthesize(s, rate))
            await queue.put(None)
        except Exception as e:  # 交給消費端丟出
            await queue.put(e)

    task = asyncio.create_task(producer())
    try:
        while True:
            item = await queue.get()
            if item is None:
                break
            if isinstance(item, Exception):
                raise item
            if item:
                yield item
    finally:
        task.cancel()
