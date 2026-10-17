"""把 m5stick/src 的韌體合併成單一 Arduino IDE 草稿碼：m5stick/arduino/AIStick/AIStick.ino

用法（在專案根目錄）：python m5stick/tools/make_ino.py
改了 src/ 之後重跑一次，讓 .ino 保持同步。
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
OUT = ROOT / "arduino" / "AIStick" / "AIStick.ino"

# 依相依順序排列
ORDER = ["settings", "ui", "recorder", "wifi_manager", "link", "setup_portal"]

HEADER = """\
// =====================================================================
//  AIStick — M5StickS3 × Hermes Agent（Arduino IDE 單檔版）
//
//  按住 A 說話 → Pi 上的 bridge 用 Whisper 轉文字 → Hermes Agent → 回覆顯示在螢幕
//
//  這個檔案是由 m5stick/tools/make_ino.py 從 m5stick/src/ 自動產生的。
//
//  Arduino IDE 設定（工具選單）：
//    開發板：ESP32S3 Dev Module（開發板管理員安裝 "esp32 by Espressif Systems"）
//    USB CDC On Boot：Enabled
//    Flash Size：8MB (64Mb)
//    Partition Scheme：8M with spiffs (3MB APP/1.5MB SPIFFS)
//    PSRAM：OPI PSRAM
//  需要的程式庫（程式庫管理員）：
//    M5Unified（會一起裝 M5GFX）、WebSockets（by Markus Sattler）、ArduinoJson（v7）
// =====================================================================

// ---------------------------------------------------------------------
//  出廠預設值：可以先改好再燒錄，也可以燒錄後用「設定模式」網頁再填
//  （設定網頁改過的值會存在 Stick 裡，重新燒錄不會被蓋掉）
// ---------------------------------------------------------------------
"""


def strip_secrets_header(text: str) -> str:
    text = text.replace("#pragma once\n", "")
    # 拿掉範本開頭的說明註解（HEADER 已經講了）
    lines = text.splitlines()
    while lines and (lines[0].startswith("//") or not lines[0].strip()):
        lines.pop(0)
    return "\n".join(lines).strip() + "\n"


def clean(text: str) -> str:
    text = text.replace("#pragma once\n", "")
    # 本地 include 與 secrets 條件 include 都拿掉（全部在同一檔）
    text = re.sub(r'#if __has_include\("secrets\.h"\)\n#include "secrets\.h"\n#else\n'
                  r'#include "secrets\.example\.h"\n#endif\n', "", text)
    text = re.sub(r'(?m)^#include "[^"]+"\n', "", text)
    return text.strip() + "\n"


def main() -> None:
    parts = [HEADER, strip_secrets_header((ROOT / "include" / "secrets.example.h").read_text("utf-8"))]
    # 系統 include 收集到最上面
    includes: list[str] = []
    bodies: list[str] = []
    for name in ORDER + ["main"]:
        for ext in ([".h", ".cpp"] if name != "main" else [".cpp"]):
            p = SRC / f"{name}{ext}"
            body = clean(p.read_text("utf-8"))
            for inc in re.findall(r"(?m)^#include <[^>]+>\n", body):
                if inc not in includes:
                    includes.append(inc)
            body = re.sub(r"(?m)^#include <[^>]+>\n", "", body).strip()
            bodies.append(f"// ===== {p.name} =====\n{body}\n")
    parts.append("\n" + "".join(includes) + "\n")
    parts.append("\n".join(bodies))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("".join(parts), encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT.parent)} ({sum(1 for _ in OUT.open(encoding='utf-8'))} lines)")


if __name__ == "__main__":
    main()
