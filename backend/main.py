"""FastAPI app：聊天、記憶面板、skill、做夢、靜態前端。

啟動時：建立資料夾、啟動做夢排程器。
"""
from __future__ import annotations

import json
from contextlib import asynccontextmanager
from typing import Any

import frontmatter
from apscheduler.schedulers.asyncio import AsyncIOScheduler
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import dream, llm, memory, skills
from .config import (DREAM_IMAGES_DIR, FRONTEND_DIR, ensure_dirs, get_config)

scheduler = AsyncIOScheduler()


@asynccontextmanager
async def lifespan(app: FastAPI):
    ensure_dirs()
    memory.get_companion()  # 確保伴侶檔存在
    interval = get_config()["dream"]["check_interval_minutes"]
    scheduler.add_job(dream.run_dream_cycle, "interval", minutes=interval,
                      id="dream_cycle", replace_existing=True)
    scheduler.start()
    yield
    scheduler.shutdown(wait=False)


app = FastAPI(title="本地 AI 伴侶", lifespan=lifespan)


# ---------------------------------------------------------------------------
# 聊天
# ---------------------------------------------------------------------------
class ChatIn(BaseModel):
    message: str


def _build_system_prompt(persona_injections: list[str], tool_outputs: list[str],
                         relevant: list[dict[str, Any]]) -> str:
    companion = memory.get_companion()
    name = companion.get("name", "伴侶")
    parts = [
        f"你是「{name}」，使用者的 AI 伴侶。以第一人稱、自然、有溫度地用繁體中文回覆。",
        f"你的設定：\n{companion.content.strip()}",
    ]
    if relevant:
        mem_lines = "\n".join(f"- ({m['category']}) {m['name']}：{m['content'][:200]}"
                              for m in relevant)
        parts.append(f"你記得的相關記憶：\n{mem_lines}")
    if persona_injections:
        parts.append("目前情境指示：\n" + "\n".join(persona_injections))
    if tool_outputs:
        parts.append("可用的即時資訊（請自然地融入回覆）：\n" + "\n".join(tool_outputs))
    parts.append("回覆請簡潔自然，像真的在對話，不要長篇大論。")
    return "\n\n".join(parts)


async def _extract_memory(user_msg: str, ai_msg: str) -> None:
    """每輪對話後的輕量記憶抽取。把結果附時間戳寫入對應 md。"""
    if not get_config()["memory"]["auto_extract"]:
        return
    instruction = (
        "你是記憶抽取器。從以下對話中找出值得長期記住的資訊，"
        "回傳 JSON，格式："
        '{"facts": ["關於使用者的事實/偏好"], '
        '"characters": [{"name":"人物名","appearance":"外貌英文關鍵字(給畫圖用,可空)","note":"備註"}], '
        '"scenes": [{"name":"地點名","description":"場景英文關鍵字(給畫圖用)"}], '
        '"events": ["重要事件一句話"]}。'
        "沒有就給空陣列。只回 JSON。"
    )
    convo = f"使用者：{user_msg}\n伴侶：{ai_msg}"
    data = await llm.chat_json([
        {"role": "system", "content": instruction},
        {"role": "user", "content": convo},
    ])
    if not isinstance(data, dict):
        return
    for fact in data.get("facts", []) or []:
        if isinstance(fact, str) and fact.strip():
            memory.append_fact(fact.strip())
    for ch in data.get("characters", []) or []:
        if isinstance(ch, dict) and ch.get("name"):
            memory.upsert_character(ch["name"], appearance=ch.get("appearance") or None,
                                    note=ch.get("note") or None)
    for sc in data.get("scenes", []) or []:
        if isinstance(sc, dict) and sc.get("name"):
            memory.upsert_scene(sc["name"], description=sc.get("description") or None)
    for ev in data.get("events", []) or []:
        if isinstance(ev, str) and ev.strip():
            memory.append_event(ev.strip())


@app.post("/chat")
async def chat(body: ChatIn):
    msg = body.message.strip()
    if not msg:
        raise HTTPException(400, "空訊息")

    persona_injections, tool_outputs, triggered = skills.apply(msg)
    relevant = memory.search(msg)
    system = _build_system_prompt(persona_injections, tool_outputs, relevant)

    messages = [{"role": "system", "content": system}]
    messages += memory.recent_messages()
    messages.append({"role": "user", "content": msg})

    reply = await llm.chat(messages)

    memory.append_history("user", msg)
    memory.append_history("assistant", reply)

    # 記憶抽取（同步跑，量小；想更快可改背景）
    await _extract_memory(msg, reply)

    # 立繪：伴侶的夢境圖（若有）
    character_image = dream.image_for("characters/companion.md")
    # 場景圖：訊息命中的、已有夢境圖的場景
    scene_image = None
    for m in relevant:
        img = dream.image_for(m["path"])
        if img:
            if m["category"] == "scenes":
                scene_image = img
            elif m["category"] == "characters" and not character_image:
                character_image = img
    return {
        "reply": reply,
        "character_image": character_image,
        "scene_image": scene_image,
        "triggered_skills": triggered,
    }


# ---------------------------------------------------------------------------
# 狀態 / 做夢
# ---------------------------------------------------------------------------
@app.get("/status")
async def status():
    return dream.status()


@app.post("/sleep")
async def sleep_now():
    """手動觸發做夢（不管現在是不是睡覺時段）。"""
    return await dream.run_dream_cycle(force=True)


@app.get("/dreams")
async def dreams():
    return dream.gallery()


# ---------------------------------------------------------------------------
# 記憶面板
# ---------------------------------------------------------------------------
class MemoryWrite(BaseModel):
    content: str


@app.get("/memory")
async def memory_list():
    return memory.list_memory()


@app.get("/memory/{path:path}")
async def memory_get(path: str):
    try:
        return {"path": path, "content": memory.read(path)}
    except (FileNotFoundError, ValueError):
        raise HTTPException(404, "找不到記憶檔")


@app.put("/memory/{path:path}")
async def memory_put(path: str, body: MemoryWrite):
    try:
        memory.write(path, body.content)
    except ValueError:
        raise HTTPException(400, "非法路徑")
    return {"ok": True}


@app.delete("/memory/{path:path}")
async def memory_delete(path: str):
    try:
        memory.delete(path)
    except ValueError:
        raise HTTPException(400, "非法路徑")
    return {"ok": True}


# ---------------------------------------------------------------------------
# Skills
# ---------------------------------------------------------------------------
@app.get("/skills")
async def skills_list():
    return [{"name": s["name"], "type": s["type"], "triggers": s["triggers"],
             "description": s["description"]} for s in skills.load_skills()]


# ---------------------------------------------------------------------------
# 圖片靜態服務 + 前端
# ---------------------------------------------------------------------------
@app.get("/images/{name}")
async def serve_image(name: str):
    p = DREAM_IMAGES_DIR / name
    if not p.exists() or p.parent.resolve() != DREAM_IMAGES_DIR.resolve():
        raise HTTPException(404, "找不到圖片")
    return FileResponse(p)


# 前端靜態檔（放最後，掛在根路徑）
app.mount("/", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="frontend")
