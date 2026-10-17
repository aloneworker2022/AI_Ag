"""語音回覆：Hermes 一邊產生文字，這裡一邊切句子、合成語音，再照播放速度串流給 Stick。

合成引擎（voice.engine）：
- piper（預設）：Piper，直接在 Pi 上跑，離線、免費，電腦不用裝任何東西
- http：任何 OpenAI 相容的 TTS 服務（POST {"input": "..."} 回傳 WAV），之後想換聲音用

輸出一律轉成 Stick 用的 16 kHz 16-bit 單聲道 PCM。
"""
from __future__ import annotations

import asyncio
import io
import logging
import re
import threading
import wave
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Awaitable, Callable

import httpx

from .config import BRIDGE_DIR, section

log = logging.getLogger("bridge.voice")

OUT_RATE = 16000
CHUNK_BYTES = 4096   # 每段 128 ms
LEAD_S = 0.8         # 最多比播放進度超前幾秒，避免塞爆 Stick 緩衝


def _cfg() -> dict[str, Any]:
    return section("voice")


def enabled() -> bool:
    return bool(_cfg().get("enabled", False))


def style_prompt() -> str:
    """語音模式額外的口語化指示（疊在 hermes.system_prompt 後面）。"""
    return str(_cfg().get("style_prompt") or "")


# ---------------------------------------------------------------------------
# 文字整理與切句
# ---------------------------------------------------------------------------
_URL = re.compile(r"https?://\S+")
_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")
_ENDS = "。！？!?；;\n"
_SOFT = "，,、：:"


def clean(text: str) -> str:
    text = _URL.sub("", text)
    text = _EMOJI.sub("", text)
    text = re.sub(r"[*#`>_~|\[\]()]", "", text)
    return re.sub(r"\s+", " ", text).strip()


def _speakable(text: str) -> bool:
    return bool(re.search(r"[\w一-鿿]", text))


class SentenceSplitter:
    """串流文字 → 一句一句。第一句盡量短（早點開口），之後湊長一點（語氣比較連貫）。"""

    def __init__(self, first_min: int = 6, min_len: int = 14, soft_max: int = 40) -> None:
        self.buf = ""
        self.count = 0
        self.first_min = first_min
        self.min_len = min_len
        self.soft_max = soft_max

    def feed(self, text: str) -> list[str]:
        out: list[str] = []
        for ch in text:
            self.buf += ch
            need = self.first_min if self.count == 0 else self.min_len
            if ch in _ENDS and len(self.buf.strip()) >= need:
                out.append(self._take())
            elif ch in _SOFT and len(self.buf) >= self.soft_max:
                out.append(self._take())  # 太長的句子在逗號切開
        return [s for s in out if s]

    def flush(self) -> list[str]:
        return [self._take()] if self.buf.strip() else []

    def _take(self) -> str:
        s, self.buf = clean(self.buf), ""
        if s and _speakable(s):
            self.count += 1
            return s
        return ""


# ---------------------------------------------------------------------------
# 合成
# ---------------------------------------------------------------------------
class VoiceError(RuntimeError):
    pass


def _resample_to_out(audio, rate: int) -> bytes:
    """float/int 陣列（單聲道）→ 16 kHz int16 bytes。"""
    import numpy as np

    audio = np.asarray(audio, dtype=np.float32)
    if rate != OUT_RATE and len(audio):
        n = int(len(audio) * OUT_RATE / rate)
        audio = np.interp(np.linspace(0, len(audio) - 1, n), np.arange(len(audio)), audio)
    return np.clip(audio, -32768, 32767).astype("<i2").tobytes()


# ---- Piper（在 Pi 上跑）----
VOICES_DIR = BRIDGE_DIR / "voices"
_piper_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="piper")
_piper_lock = threading.Lock()
_piper: Any = None


def _load_piper() -> Any:
    """第一次用才載入；聲音檔不在就自動下載到 bridge/voices/（要網路一次）。"""
    global _piper
    with _piper_lock:
        if _piper is not None:
            return _piper
        try:
            from piper import PiperVoice
            from piper.config import PhonemeType
            from piper.download_voices import download_voice
        except ImportError as e:
            raise VoiceError("Pi 上沒裝 piper-tts（pip install piper-tts）") from e
        name = str(_cfg().get("piper_voice") or "zh_CN-huayan-medium")
        VOICES_DIR.mkdir(parents=True, exist_ok=True)
        model = VOICES_DIR / f"{name}.onnx"
        if not model.exists():
            log.info("downloading piper voice %s", name)
            download_voice(name, VOICES_DIR)
        voice = PiperVoice.load(model, download_dir=VOICES_DIR)
        if voice.config.phoneme_type == PhonemeType.PINYIN and not (VOICES_DIR / "g2pW").exists():
            from piper.phonemize_chinese import download_model
            download_model(VOICES_DIR / "g2pW")  # 拼音型中文聲音要的多音字模型
        _piper = voice
        return voice


def _piper_synth_sync(text: str) -> bytes:
    import numpy as np
    from piper.config import SynthesisConfig

    voice = _load_piper()
    cfg = _cfg()
    syn = SynthesisConfig(length_scale=float(cfg.get("length_scale", 1.0)),
                          volume=float(cfg.get("volume", 1.0)))
    chunks = list(voice.synthesize(text, syn))
    if not chunks:
        return b""
    audio = np.concatenate([c.audio_int16_array for c in chunks]).astype(np.float32)
    return _resample_to_out(audio, chunks[0].sample_rate)


async def _piper_synth(text: str) -> bytes:
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(_piper_executor, _piper_synth_sync, text)


# ---- OpenAI 相容的 HTTP TTS 服務 ----
def _wav_to_pcm16k(data: bytes) -> bytes:
    import numpy as np

    with wave.open(io.BytesIO(data), "rb") as w:
        rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        frames = w.readframes(w.getnframes())
    if width == 2:
        audio = np.frombuffer(frames, dtype="<i2").astype(np.float32)
    elif width == 4:  # 有些服務存成 32-bit float
        audio = np.frombuffer(frames, dtype="<f4").astype(np.float32) * 32767.0
    else:
        raise VoiceError(f"不支援的 WAV 格式（{width * 8}-bit）")
    if ch > 1:
        audio = audio.reshape(-1, ch).mean(axis=1)
    return _resample_to_out(audio, rate)


async def _http_synth(client: httpx.AsyncClient, text: str) -> bytes:
    cfg = _cfg()
    url = cfg.get("tts_url") or "http://127.0.0.1:8080/audio/speech"
    try:
        r = await client.post(url, json={"model": "tts", "input": text},
                              timeout=float(cfg.get("timeout", 30)))
    except httpx.ConnectError as e:
        raise VoiceError(f"連不到 TTS 服務（{url}）") from e
    except httpx.TimeoutException as e:
        raise VoiceError("TTS 服務太久沒回應") from e
    if r.status_code != 200:
        raise VoiceError(f"TTS 服務回應 {r.status_code}")
    return _wav_to_pcm16k(r.content)


async def synthesize(client: httpx.AsyncClient, text: str) -> bytes:
    if str(_cfg().get("engine", "piper")) == "http":
        return await _http_synth(client, text)
    return await _piper_synth(text)


# ---------------------------------------------------------------------------
# 一輪的語音輸出：收句子 → 合成（先合成下一句）→ 照播放速度送給 Stick
# ---------------------------------------------------------------------------
SendJson = Callable[[dict[str, Any]], Awaitable[None]]
SendBytes = Callable[[bytes], Awaitable[bool]]


class Speaker:
    def __init__(self, msg_id: Any, send_json: SendJson, send_bytes: SendBytes) -> None:
        self.msg_id = msg_id
        self._send_json = send_json
        self._send_bytes = send_bytes
        self._sentences: asyncio.Queue = asyncio.Queue()
        self.said_anything = False
        self._task = asyncio.create_task(self._run())

    def say(self, sentence: str) -> None:
        if sentence:
            self.said_anything = True
            self._sentences.put_nowait(sentence)

    def close(self) -> None:
        self._sentences.put_nowait(None)

    def cancel(self) -> None:
        self._task.cancel()

    def done(self) -> bool:
        return self._task.done()

    async def _run(self) -> None:
        loop = asyncio.get_running_loop()
        started = False
        play_end = 0.0
        audio_q: asyncio.Queue = asyncio.Queue(maxsize=2)

        async def synth_worker(client: httpx.AsyncClient) -> None:
            # 合成跟播放分開跑：念這一句時，下一句已經在合成
            try:
                while True:
                    s = await self._sentences.get()
                    if s is None:
                        break
                    await audio_q.put(await synthesize(client, s))
                await audio_q.put(None)
            except Exception as e:
                await audio_q.put(e)

        async with httpx.AsyncClient() as client:
            worker = asyncio.create_task(synth_worker(client))
            try:
                while True:
                    pcm = await audio_q.get()
                    if pcm is None:
                        break
                    if isinstance(pcm, Exception):
                        raise pcm
                    if not started:
                        await self._send_json({"type": "audio_reply_start", "id": self.msg_id,
                                               "rate": OUT_RATE, "format": "pcm_s16le"})
                        started = True
                    for i in range(0, len(pcm), CHUNK_BYTES):
                        part = pcm[i:i + CHUNK_BYTES]
                        now = loop.time()
                        if play_end - now > LEAD_S:
                            await asyncio.sleep(play_end - now - LEAD_S)
                            now = loop.time()
                        if not await self._send_bytes(part):
                            return  # Stick 離線了
                        play_end = max(play_end, now) + len(part) / (OUT_RATE * 2)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("voice failed: %s", e)
                await self._send_json({"type": "status", "id": self.msg_id,
                                       "text": f"語音失敗：{e}"[:80]})
            finally:
                worker.cancel()
                if started:
                    try:
                        await asyncio.shield(self._send_json({"type": "audio_reply_end",
                                                              "id": self.msg_id}))
                    except Exception:
                        pass
