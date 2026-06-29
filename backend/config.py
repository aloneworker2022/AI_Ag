"""讀取 config.yaml 與專案路徑。所有模組共用。"""
from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Any

import yaml

# 專案根目錄（config.py 在 backend/ 底下，往上一層就是根）
ROOT = Path(__file__).resolve().parent.parent

MEMORY_DIR = ROOT / "memory"
SKILLS_DIR = ROOT / "skills"
DREAMS_DIR = ROOT / "dreams"
DREAM_IMAGES_DIR = DREAMS_DIR / "images"
FRONTEND_DIR = ROOT / "frontend"
HISTORY_FILE = MEMORY_DIR / "history.json"
DREAM_INDEX_FILE = DREAMS_DIR / "index.json"
CONFIG_FILE = ROOT / "config.yaml"


@lru_cache(maxsize=1)
def get_config() -> dict[str, Any]:
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def ensure_dirs() -> None:
    """啟動時確保必要的資料夾都存在。"""
    for d in (
        MEMORY_DIR / "characters",
        MEMORY_DIR / "scenes",
        MEMORY_DIR / "events",
        MEMORY_DIR / "facts",
        SKILLS_DIR / "persona",
        SKILLS_DIR / "tools",
        DREAM_IMAGES_DIR,
    ):
        d.mkdir(parents=True, exist_ok=True)
