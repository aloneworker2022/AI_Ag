"""Skill 系統：載入 skills/ 底下的 md，依關鍵字觸發。

兩種型別（frontmatter `type`）：
  - persona：命中時把 body 注入 system prompt（改變說話風格/情境）
  - tool   ：命中時執行 skills_impl 對應的 handler，結果注入 context

觸發方式：關鍵字比對使用者輸入（本地模型友善，不需 function-calling）。
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

import frontmatter

from .config import SKILLS_DIR
from . import skills_impl


def load_skills() -> list[dict[str, Any]]:
    skills: list[dict[str, Any]] = []
    for md in sorted(SKILLS_DIR.rglob("*.md")):
        post = frontmatter.load(md)
        skills.append({
            "name": post.get("name", md.stem),
            "type": post.get("type", "persona"),
            "triggers": [str(t) for t in post.get("triggers", [])],
            "handler": post.get("fn"),
            "description": post.get("description", ""),
            "body": post.content.strip(),
            "path": str(md.relative_to(SKILLS_DIR)),
        })
    return skills


def match(message: str) -> list[dict[str, Any]]:
    """回傳命中的 skill（任一 trigger 是訊息的子字串就算命中）。"""
    msg = message.lower()
    hits = []
    for skill in load_skills():
        if any(t.lower() in msg for t in skill["triggers"]):
            hits.append(skill)
    return hits


def apply(message: str) -> tuple[list[str], list[str], list[str]]:
    """處理一則訊息的所有命中 skill。

    回傳 (persona_injections, tool_outputs, triggered_names)
    """
    persona_injections: list[str] = []
    tool_outputs: list[str] = []
    names: list[str] = []
    for skill in match(message):
        names.append(skill["name"])
        if skill["type"] == "tool" and skill.get("handler"):
            out = skills_impl.run(skill["handler"], message)
            if out:
                tool_outputs.append(out)
        else:  # persona
            if skill["body"]:
                persona_injections.append(skill["body"])
    return persona_injections, tool_outputs, names
