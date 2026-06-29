"""純 Markdown 記憶系統。

設計原則：透明 + 可改。每筆記憶都是 memory/ 底下的 .md 檔，
使用者可以在網頁面板或直接用編輯器打開來看、改、刪。

四類：characters / scenes / events / facts
每個 md 用 YAML frontmatter（結構欄位）+ 自由內文。
"""
from __future__ import annotations

import json
import re
from datetime import datetime
from pathlib import Path
from typing import Any

import frontmatter

from .config import HISTORY_FILE, MEMORY_DIR, get_config

CATEGORIES = ["characters", "scenes", "events", "facts"]


# ---------------------------------------------------------------------------
# 基本檔案操作
# ---------------------------------------------------------------------------
def _safe_path(rel_path: str) -> Path:
    """把相對路徑解析到 MEMORY_DIR 底下，防止路徑跳脫。"""
    p = (MEMORY_DIR / rel_path).resolve()
    if not str(p).startswith(str(MEMORY_DIR.resolve())):
        raise ValueError("非法路徑")
    return p


def _slugify(name: str) -> str:
    s = re.sub(r"\s+", "_", name.strip())
    s = re.sub(r"[^\w一-鿿_-]", "", s)
    return s or "untitled"


def list_memory() -> list[dict[str, Any]]:
    """列出所有記憶檔（給網頁面板用）。"""
    items: list[dict[str, Any]] = []
    for cat in CATEGORIES:
        cat_dir = MEMORY_DIR / cat
        if not cat_dir.exists():
            continue
        for md in sorted(cat_dir.glob("*.md")):
            post = frontmatter.load(md)
            items.append({
                "category": cat,
                "path": f"{cat}/{md.name}",
                "name": post.get("name", md.stem),
                "tags": post.get("tags", []),
                "modified": md.stat().st_mtime,
            })
    return items


def read(rel_path: str) -> str:
    return _safe_path(rel_path).read_text(encoding="utf-8")


def write(rel_path: str, content: str) -> None:
    p = _safe_path(rel_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding="utf-8")


def delete(rel_path: str) -> None:
    p = _safe_path(rel_path)
    if p.exists():
        p.unlink()


# ---------------------------------------------------------------------------
# 高階：人物 / 事件 / 事實的建立與更新
# ---------------------------------------------------------------------------
def get_companion() -> frontmatter.Post:
    """主要 AI 伴侶。第一次跑時若不存在會用設定檔的名字建一個預設的。"""
    name = get_config()["companion"]["name"]
    path = MEMORY_DIR / "characters" / "companion.md"
    if not path.exists():
        post = frontmatter.Post(
            "個性溫柔體貼，喜歡和你聊天。說話自然、有溫度。",
            name=name,
            appearance="1girl, long black hair, gentle eyes, soft smile, casual clothes, masterpiece",
            tags=["companion"],
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(frontmatter.dumps(post), encoding="utf-8")
    return frontmatter.load(path)


def upsert_character(name: str, *, appearance: str | None = None,
                     note: str | None = None) -> str:
    """新增或更新一個人物（非伴侶本身）。回傳相對路徑。"""
    slug = _slugify(name)
    rel = f"characters/{slug}.md"
    path = MEMORY_DIR / "characters"
    path.mkdir(parents=True, exist_ok=True)
    fp = MEMORY_DIR / rel
    if fp.exists():
        post = frontmatter.load(fp)
    else:
        post = frontmatter.Post("", name=name, appearance=appearance or "", tags=[])
    if appearance and not post.get("appearance"):
        post["appearance"] = appearance
    if note:
        post.content = (post.content + f"\n- {note}").strip()
    fp.write_text(frontmatter.dumps(post), encoding="utf-8")
    return rel


def upsert_scene(name: str, *, description: str | None = None) -> str:
    slug = _slugify(name)
    rel = f"scenes/{slug}.md"
    (MEMORY_DIR / "scenes").mkdir(parents=True, exist_ok=True)
    fp = MEMORY_DIR / rel
    if fp.exists():
        post = frontmatter.load(fp)
    else:
        post = frontmatter.Post("", name=name, appearance=description or "", tags=[])
    if description:
        if not post.get("appearance"):
            post["appearance"] = description
        post.content = (post.content + f"\n- {description}").strip()
    fp.write_text(frontmatter.dumps(post), encoding="utf-8")
    return rel


def append_event(summary: str) -> str:
    """事件記在當天一個 md，逐條附時間戳。"""
    day = datetime.now().strftime("%Y-%m-%d")
    rel = f"events/{day}.md"
    (MEMORY_DIR / "events").mkdir(parents=True, exist_ok=True)
    fp = MEMORY_DIR / rel
    ts = datetime.now().strftime("%H:%M")
    if fp.exists():
        post = frontmatter.load(fp)
        post.content = f"{post.content}\n- [{ts}] {summary}".strip()
    else:
        post = frontmatter.Post(f"- [{ts}] {summary}", name=f"{day} 的事", tags=["events"])
    fp.write_text(frontmatter.dumps(post), encoding="utf-8")
    return rel


def append_fact(fact: str) -> str:
    """關於使用者的事實/偏好，集中在 facts/user.md。"""
    rel = "facts/user.md"
    (MEMORY_DIR / "facts").mkdir(parents=True, exist_ok=True)
    fp = MEMORY_DIR / rel
    ts = datetime.now().strftime("%Y-%m-%d")
    if fp.exists():
        post = frontmatter.load(fp)
        post.content = f"{post.content}\n- {fact} _(記於 {ts})_".strip()
    else:
        post = frontmatter.Post(f"- {fact} _(記於 {ts})_", name="關於使用者", tags=["facts"])
    fp.write_text(frontmatter.dumps(post), encoding="utf-8")
    return rel


# ---------------------------------------------------------------------------
# 檢索：簡單關鍵字比對（本地、無向量庫，保持簡單）
# ---------------------------------------------------------------------------
def search(text: str, limit: int = 6) -> list[dict[str, Any]]:
    """從訊息文字找出可能相關的記憶檔。

    比對 name 是否出現在文字裡，或內文是否含文字中的詞。
    回傳 [{path, name, category, content}]，依相關度粗排。
    """
    results: list[tuple[int, dict[str, Any]]] = []
    for item in list_memory():
        fp = MEMORY_DIR / item["path"]
        post = frontmatter.load(fp)
        name = str(item["name"])
        score = 0
        if name and name in text:
            score += 5
        # 內文與訊息共同詞
        body = post.content.lower()
        for token in set(re.findall(r"[\w一-鿿]{2,}", text.lower())):
            if token in body:
                score += 1
        if score > 0:
            results.append((score, {
                "path": item["path"],
                "name": name,
                "category": item["category"],
                "content": post.content.strip(),
            }))
    results.sort(key=lambda x: x[0], reverse=True)
    return [r[1] for r in results[:limit]]


# ---------------------------------------------------------------------------
# 短期記憶：history.json
# ---------------------------------------------------------------------------
def load_history() -> list[dict[str, str]]:
    if HISTORY_FILE.exists():
        try:
            return json.loads(HISTORY_FILE.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return []
    return []


def append_history(role: str, content: str) -> None:
    history = load_history()
    history.append({"role": role, "content": content,
                    "ts": datetime.now().isoformat(timespec="seconds")})
    # 只留設定的輪數（user+assistant 各算一則，所以 *2）
    keep = get_config()["memory"]["history_turns"] * 2
    history = history[-keep:]
    HISTORY_FILE.parent.mkdir(parents=True, exist_ok=True)
    HISTORY_FILE.write_text(json.dumps(history, ensure_ascii=False, indent=2),
                            encoding="utf-8")


def recent_messages() -> list[dict[str, str]]:
    """給 LLM 用的近期對話（去掉 ts 欄位）。"""
    return [{"role": h["role"], "content": h["content"]} for h in load_history()]
