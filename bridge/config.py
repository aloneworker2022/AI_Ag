"""讀取 bridge/config.yaml（沒有就用 config.example.yaml）。"""
from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Any

import yaml

BRIDGE_DIR = Path(__file__).resolve().parent
CONFIG_FILE = BRIDGE_DIR / "config.yaml"
EXAMPLE_FILE = BRIDGE_DIR / "config.example.yaml"
STATE_FILE = BRIDGE_DIR / "state.json"


@lru_cache(maxsize=1)
def get_config() -> dict[str, Any]:
    path = CONFIG_FILE if CONFIG_FILE.exists() else EXAMPLE_FILE
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def section(name: str) -> dict[str, Any]:
    return get_config().get(name) or {}
