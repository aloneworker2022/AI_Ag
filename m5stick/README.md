# M5StickS3 隨身終端

讓 M5StickS3 透過 Wi-Fi 跟 Raspberry Pi 5 上的 AI 伴侶對話。

目前是**第 1 階段：用說的，Pi 回覆文字**。按住 A 對 Stick 說話，Pi 用本地
Whisper 辨識成文字交給 AI，回覆顯示在 Stick 螢幕上。
之後依序加入：語音回覆（Stick 念出來）→ 圖片 → 感測器 / GPIO。
所有功能都走同一條 WebSocket（`ws://<pi>:8000/ws/device`），後面的階段只會新增訊息類型。

## 1. Pi 端準備

```bash
pip install -r requirements.txt      # 包含 faster-whisper（語音辨識）與 opencc（轉繁體）
# config.yaml 裡把 device.token 改成你自己的密碼
uvicorn backend.main:app --host 0.0.0.0 --port 8000
```

一定要加 `--host 0.0.0.0`，不然 Stick 連不進來。
建議在路由器幫 Pi 固定 IP。

語音辨識設定在 `config.yaml` 的 `stt:`。預設模型 `small`，第一次說話時會自動下載
（Pi 要能上網一次），之後完全離線。Pi 5 上一句話大約要幾秒；嫌慢可以改成 `base`。

## 2. 燒錄韌體

需要電腦裝 [PlatformIO](https://platformio.org/)（VS Code 外掛或 `pip install platformio`）。

```bash
cd m5stick
cp include/secrets.example.h include/secrets.h   # 可選：先填好 Wi-Fi，省得開機再設定
pio run -t upload
pio device monitor                                 # 看 log，也可以直接打字送訊息
```

`secrets.h` 已加入 `.gitignore`，密碼不會進版控。
沒建 `secrets.h` 也能編譯，開機後再用設定模式填。

如果上傳失敗（找不到序列埠或連不上），照 M5Stack 官方說明讓 Stick 進入下載模式後重試。

## 3. Wi-Fi 設定（多組、可切換）

Stick 最多記 6 組 Wi-Fi，**排越前面越優先**。開機時會掃描附近網路，
依清單順序連第一個「附近有、連得上」的。斷線超過 5 秒會自動重找。

每組 Wi-Fi 可以各自指定 **Pi 的位址**。用手機熱點時，Pi 也要連到同一個熱點，
它在熱點裡的 IP 跟在家裡不一樣，所以要分開填（留空 = 用預設位址）。

### 設定模式（用手機改設定）

1. Stick 上**按住 B 5 秒再放開**（第一次開機、還沒存任何 Wi-Fi 時會自動進入）
2. 手機連上螢幕顯示的 Wi-Fi `AIStick-XXXX`（密碼預設 `aistick123`）
3. 通常會自動跳出設定頁；沒跳的話用瀏覽器開 `http://192.168.4.1`
4. 可以：新增 / 更新 / 刪除 / 調整順序 Wi-Fi、改 Pi 位址、port、token、裝置名稱
5. 改完按「重新開機套用設定」，或在 Stick 上按 A

設定存在 Stick 的 NVS，重新燒錄韌體也不會消失。
想清掉就在設定頁按「恢復出廠預設」（回到 `secrets.h` 的值）。

## 4. 操作

| 按鍵 | 動作 |
|---|---|
| **A 按住** | 說話；放開就送出（太短當誤觸，最長 30 秒會自動送出） |
| B 單按 | 回覆太長時往下捲，捲到底回首頁 |
| **B 按住 1.5 秒放開** | 切到清單裡的下一組 Wi-Fi |
| **B 按住 5 秒放開** | 進設定模式 |

按住 B 時螢幕下方會提示「放開會做什麼」。

畫面流程：聆聽中（秒數＋音量條）→ 辨識中… → 顯示你說的話＋思考中… → 回覆。

狀態列：左邊是目前 Wi-Fi，右邊圓點綠色 = 已連上 Pi、紅色 = 沒連上，最右邊是電量。

在 serial monitor 打一行字按 Enter，會當成文字訊息送出（不用說話也能測試）。

### 從 Pi 推訊息到 Stick 螢幕

```bash
curl http://<pi>:8000/devices          # 看在線裝置
curl -X POST http://<pi>:8000/devices/stick/notify \
     -H 'Content-Type: application/json' -d '{"title":"小夜","text":"該睡覺囉"}'
```

## 5. 協定（v1）

文字訊息都是 JSON：`{"type": "...", ...}`。
二進位訊息一律屬於「最近一個 `*_start`」開啟的串流（目前只有語音）。

| 方向 | type | 欄位 | 說明 |
|---|---|---|---|
| Stick → Pi | `hello` | `device`, `fw`, `proto`, `token` | 連上後第一則，必須是它；token 錯會被斷線（code 4401） |
| Pi → Stick | `welcome` | `proto`, `companion` | 驗證通過 |
| Stick → Pi | `audio_start` | `id`, `format`=`pcm_s16le`, `rate`=16000 | 開始說話 |
| Stick → Pi | （二進位） | 16-bit little-endian 單聲道 PCM | 邊錄邊傳，每段 32 ms |
| Stick → Pi | `audio_end` / `audio_cancel` | `id` | 說完 / 誤觸取消 |
| Pi → Stick | `transcribing` | `id` | 辨識中 |
| Pi → Stick | `transcript` | `id`, `text` | 辨識結果（你說了什麼） |
| Stick → Pi | `chat` | `id`, `text` | 直接傳文字（序列埠測試用） |
| Pi → Stick | `thinking` | `id` | AI 正在想 |
| Pi → Stick | `reply` | `id`, `text` | AI 回覆 |
| Pi → Stick | `notify` | `title`, `text` | Pi 主動推的訊息 |
| Pi → Stick | `error` | `id`?, `message` | 錯誤（例如「沒聽清楚，再說一次」） |
| 雙向 | `ping` / `pong` | `t` | 應用層心跳（另外也有 WebSocket ping） |

Pi 主動對裝置下指令（之後的 `sensor_read`、`gpio_write` 等）會帶 `id`，
裝置回覆時帶 `re: <id>`，後端用 `device.hub.request()` 等待結果。

## 檔案

| 檔案 | 內容 |
|---|---|
| `src/main.cpp` | 按鍵、畫面流程 |
| `src/recorder.*` | 麥克風錄音、分段串流 |
| `src/wifi_manager.*` | 多組 Wi-Fi 選擇、斷線重找、手動切換 |
| `src/setup_portal.*` | 設定模式（熱點 + 網頁） |
| `src/settings.*` | 設定讀寫（NVS） |
| `src/link.*` | 跟 Pi 的 WebSocket |
| `src/ui.*` | 螢幕（中文字型、自動換行、捲動） |
| `include/secrets.example.h` | 出廠預設值範本 |
