"""台灣華語語音辨識服務（MediaTek Breeze ASR 25，跑在 Win11 電腦的顯卡上）。

POST /asr?rate=16000   body = 16-bit little-endian 單聲道 PCM
回傳 {"text": "..."}
"""
from __future__ import annotations

import os

import numpy as np
import torch
import uvicorn
from fastapi import FastAPI, Header, HTTPException, Request
from transformers import pipeline

MODEL = os.getenv("ASR_MODEL", "MediaTek-Research/Breeze-ASR-25")
LANGUAGE = os.getenv("ASR_LANGUAGE", "")  # 留空 = 自動（Breeze 擅長中英夾雜）
TOKEN = os.getenv("VOICE_TOKEN", "")

device = "cuda:0" if torch.cuda.is_available() else "cpu"
asr = pipeline(
    "automatic-speech-recognition",
    model=MODEL,
    torch_dtype=torch.float16 if device != "cpu" else torch.float32,
    device=device,
)
app = FastAPI(title="Breeze ASR")


@app.get("/health")
def health():
    return {"ok": True, "model": MODEL, "device": device}


@app.post("/asr")
async def transcribe(request: Request, rate: int = 16000, x_voice_token: str = Header(default="")):
    if TOKEN and x_voice_token != TOKEN:
        raise HTTPException(401, "token 錯誤")
    pcm = await request.body()
    audio = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2").astype(np.float32) / 32768.0
    if len(audio) < rate * 0.3:
        return {"text": ""}
    kwargs = {"task": "transcribe"}
    if LANGUAGE:
        kwargs["language"] = LANGUAGE
    out = asr({"raw": audio, "sampling_rate": rate}, generate_kwargs=kwargs)
    return {"text": (out.get("text") or "").strip()}


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8081)
