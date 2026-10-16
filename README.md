# 本地 AI 伴侶 — md 記憶 ＋ Skill ＋ 做夢系統

一個**簡單、透明、純本地**的 AI 伴侶。沒有 Telegram、沒有外部 API 串接、沒有花俏功能。
畫面是日式文字冒險（視覺小說）排版：中間人物立繪、頭上對話框、下方輸入框。

## 三個核心

1. **純 Markdown 記憶** — 所有記憶都是 `memory/` 底下的 `.md` 檔，你隨時能在
   網頁的記憶面板或直接用編輯器打開來看、改、刪。分四類：
   `characters/`（人物）、`scenes/`（場景）、`events/`（事件）、`facts/`（關於你的事實）。
2. **Skill 系統** — 每個 skill 是一個 md（`skills/`）。用關鍵字觸發：
   - `persona` 型：命中時改變說話風格／情境（body 注入 prompt）
   - `tool` 型：命中時執行小工具（`backend/skills_impl.py`），如查時間、算式
3. **做夢系統** — 固定睡覺時段（預設 23:00–07:00）會自動呼叫你既有的 **ComfyUI**，
   把聊天中出現過、還沒畫過的人物／場景／事件畫出來。下次再聊到同一對象，
   畫面就會貼出當初夢到的圖。

## 前置需求

- [Ollama](https://ollama.com/) 在本機跑，且已 `ollama pull` 了一個模型
- [ComfyUI](https://github.com/comfyanonymous/ComfyUI) 在本機跑（做夢用，預設 `127.0.0.1:8188`）
- Python 3.10+

## 安裝與啟動

```bash
pip install -r requirements.txt
uvicorn backend.main:app --reload
```

開瀏覽器到 http://127.0.0.1:8000 即可。

想讓區網內其他裝置（手機、M5StickS3）連進來，改用 `--host 0.0.0.0`。

## M5StickS3 隨身終端

`m5stick/` 是 M5StickS3 的韌體：按住按鍵說話，Pi 用本地 Whisper 辨識後交給伴侶，
回覆文字顯示在 Stick 上。透過 Wi-Fi + WebSocket（`/ws/device`）連線，
支援多組 Wi-Fi（家裡 / 手機熱點）自動選擇與手動切換。設定與燒錄見
[m5stick/README.md](m5stick/README.md)。

## 設定

全部在 `config.yaml`：

- `ollama.model`：改成你 `ollama list` 裡有的模型名
- `comfyui.checkpoint`：想指定 checkpoint 就填，留空用 workflow 範本內的
- `comfyui.workflow`：可換成你自己從 ComfyUI 匯出的 **API 格式** workflow
  （慣例：正向提示詞節點 title 含 `Positive`）
- `dream.sleep_start` / `sleep_end`：睡覺時段

## 手動做夢（測試用）

不用等到睡覺時間，可直接打：

```bash
curl -X POST http://127.0.0.1:8000/sleep
```

它會把目前還沒畫過的對象畫一批，圖存在 `dreams/images/`，
關聯記在 `dreams/index.json`。

## API 一覽

| 方法 | 路徑 | 用途 |
|------|------|------|
| POST | `/chat` | 對話 |
| GET  | `/status` | 睡眠/做夢狀態 |
| POST | `/sleep` | 手動觸發做夢 |
| GET  | `/dreams` | 夢境畫廊 |
| GET/PUT/DELETE | `/memory/{path}` | 瀏覽/編輯/刪除記憶 |
| GET  | `/skills` | skill 清單 |

## 目錄

```
backend/    FastAPI 後端
memory/     純 md 記憶（可看可改）
skills/     skill 模組（md）
dreams/     做夢產出的圖 + 關聯
frontend/   視覺小說網頁
config.yaml 設定
```
