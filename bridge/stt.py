"""語音轉文字（本地 faster-whisper）。

Stick 傳來 16-bit 單聲道 PCM，這裡轉成文字。模型第一次用到才載入。
"""
from __future__ import annotations

import asyncio
import logging
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Any

from .config import section

log = logging.getLogger("bridge.stt")

# 模型不是 thread-safe，所有辨識排隊在同一條執行緒
_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="stt")
_model: Any = None
_model_lock = threading.Lock()
_converter: Any = None


class SttUnavailable(RuntimeError):
    pass


def _load_model() -> Any:
    global _model, _converter
    with _model_lock:
        if _model is not None:
            return _model
        try:
            from faster_whisper import WhisperModel
        except ImportError as e:
            raise SttUnavailable("Pi 上沒裝 faster-whisper（pip install faster-whisper）") from e
        cfg = section("stt")
        log.info("loading whisper model %s", cfg.get("model", "small"))
        _model = WhisperModel(
            cfg.get("model", "small"),
            device="cpu",
            compute_type=cfg.get("compute_type", "int8"),
            download_root=cfg.get("download_root") or None,
        )
        if cfg.get("traditional", True):
            try:
                import opencc
                _converter = opencc.OpenCC("s2twp")
            except ImportError:
                log.warning("沒裝 opencc，辨識結果可能是簡體字")
        return _model


def _to_float_audio(pcm: bytes, rate: int):
    import numpy as np

    audio = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2").astype(np.float32) / 32768.0
    if rate != 16000 and len(audio):
        # Whisper 要 16 kHz；簡單線性重取樣就夠用
        n = int(len(audio) * 16000 / rate)
        audio = np.interp(np.linspace(0, len(audio) - 1, n), np.arange(len(audio)), audio)
        audio = audio.astype(np.float32)
    return audio


def _transcribe_sync(pcm: bytes, rate: int) -> str:
    model = _load_model()
    cfg = section("stt")
    audio = _to_float_audio(pcm, rate)
    if len(audio) < 16000 * 0.3:  # 不到 0.3 秒，當作沒說話
        return ""
    segments, _info = model.transcribe(
        audio,
        language=cfg.get("language", "zh") or None,
        beam_size=int(cfg.get("beam_size", 1)),
        initial_prompt=cfg.get("initial_prompt") or None,
        vad_filter=True,
        condition_on_previous_text=False,  # 短句不需要，關掉比較不會亂接字
    )
    text = "".join(s.text for s in segments).strip()
    if _converter is not None:
        text = _converter.convert(text)
    return text


async def transcribe(pcm: bytes, rate: int = 16000) -> str:
    """把 16-bit little-endian 單聲道 PCM 轉成文字；聽不到內容回傳空字串。"""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(_executor, _transcribe_sync, pcm, rate)


def max_bytes(rate: int = 16000) -> int:
    """一段語音最多收多少 byte（超過就截斷）。"""
    return int(section("stt").get("max_seconds", 30)) * rate * 2
