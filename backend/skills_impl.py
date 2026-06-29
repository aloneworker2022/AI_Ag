"""tool 型 skill 的 Python 實作。

每個函式接收 (message: str) -> str，回傳要注入 context 的文字。
在 skill 的 md frontmatter 用 `handler: 函式名` 對應到這裡。
保持簡單：只放真正單純、本地、無外部 API 的小工具。
"""
from __future__ import annotations

from datetime import datetime

# 名稱 -> 函式 的註冊表
REGISTRY: dict[str, callable] = {}


def register(name: str):
    def deco(fn):
        REGISTRY[name] = fn
        return fn
    return deco


@register("tell_time")
def tell_time(message: str) -> str:
    now = datetime.now()
    weekdays = ["週一", "週二", "週三", "週四", "週五", "週六", "週日"]
    return f"現在時間是 {now:%Y-%m-%d %H:%M}（{weekdays[now.weekday()]}）。"


@register("calc")
def calc(message: str) -> str:
    """非常陽春的算式擷取與計算（只允許數字與基本運算子）。"""
    import re
    expr = "".join(re.findall(r"[0-9+\-*/().\s]", message))
    expr = expr.strip()
    if not expr:
        return "（沒有找到可計算的算式）"
    try:
        # 限制 builtins，避免任意執行
        result = eval(expr, {"__builtins__": {}}, {})  # noqa: S307
        return f"計算結果：{expr} = {result}"
    except Exception:
        return f"（無法計算：{expr}）"


def run(handler: str, message: str) -> str:
    fn = REGISTRY.get(handler)
    if not fn:
        return ""
    try:
        return fn(message)
    except Exception as e:  # tool 出錯不該讓整個對話掛掉
        return f"（skill {handler} 執行出錯：{e}）"
