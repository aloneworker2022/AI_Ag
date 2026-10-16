"""python -m bridge：用 config.yaml 的 host / port 啟動。"""
import logging

import uvicorn

from .config import section

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
cfg = section("server")
uvicorn.run("bridge.main:app", host=cfg.get("host", "0.0.0.0"), port=int(cfg.get("port", 3799)))
