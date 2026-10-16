# M5StickS3 隨身終端

讓 M5StickS3 透過 Wi-Fi 跟 Raspberry Pi 5 上的 AI 伴侶對話。

目前是**第 1 階段：文字對話**。之後依序加入：語音 → 圖片 → 感測器 / GPIO。
所有功能都走同一條 WebSocket（`ws://<pi>:8000/ws/device`），後面的階段只會新增訊息類型。

## 1. Pi 端準備

```bash
# config.yaml 裡把 device.token 改成你自己的密碼
uvicorn backend.main:app --host 0.0.0.0 --port 8000
```

一定要加 `--host 0.0.0.0`，不然 Stick 連不進來。
建議在路由器幫 Pi 固定 IP。

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

1. Stick 上**長按 A 3 秒**（第一次開機、還沒存任何 Wi-Fi 時會自動進入）
2. 手機連上螢幕顯示的 Wi-Fi `AIStick-XXXX`（密碼預設 `aistick123`）
3. 通常會自動跳出設定頁；沒跳的話用瀏覽器開 `http://192.168.4.1`
4. 可以：新增 / 更新 / 刪除 / 調整順序 Wi-Fi、改 Pi 位址、port、token、裝置名稱
5. 改完按「重新開機套用設定」，或在 Stick 上按 A

設定存在 Stick 的 NVS，重新燒錄韌體也不會消失。
想清掉就在設定頁按「恢復出廠預設」（回到 `secrets.h` 的值）。

## 4. 操作

| 按鍵 | 首頁 | 回覆畫面 |
|---|---|---|
| A 單按 | 送出這句 | 回首頁 |
| B 單按 | 下一句 | 往下捲，捲到底回首頁 |
| **B 長按 1.5 秒** | 切到清單裡的下一組 Wi-Fi | 同左 |
| **A 長按 3 秒** | 進設定模式 | 同左 |

狀態列：左邊是目前 Wi-Fi，右邊圓點綠色 = 已連上 Pi、紅色 = 沒連上，最右邊是電量。

在 serial monitor 打一行字按 Enter，也會當成訊息送出（方便測試任意文字）。

### 從 Pi 推訊息到 Stick 螢幕

```bash
curl http://<pi>:8000/devices          # 看在線裝置
curl -X POST http://<pi>:8000/devices/stick/notify \
     -H 'Content-Type: application/json' -d '{"title":"小夜","text":"該睡覺囉"}'
```

## 5. 協定（v1）

文字訊息都是 JSON：`{"type": "...", ...}`。之後的語音 / 圖片會用二進位訊息。

| 方向 | type | 欄位 | 說明 |
|---|---|---|---|
| Stick → Pi | `hello` | `device`, `fw`, `proto`, `token` | 連上後第一則，必須是它；token 錯會被斷線（code 4401） |
| Pi → Stick | `welcome` | `proto`, `companion` | 驗證通過 |
| Stick → Pi | `chat` | `id`, `text` | 傳話給 AI |
| Pi → Stick | `thinking` | `id` | 收到了，正在想 |
| Pi → Stick | `reply` | `id`, `text` | AI 回覆 |
| Pi → Stick | `notify` | `title`, `text` | Pi 主動推的訊息 |
| Pi → Stick | `error` | `id`?, `message` | 錯誤 |
| 雙向 | `ping` / `pong` | `t` | 應用層心跳（另外也有 WebSocket ping） |

Pi 主動對裝置下指令（之後的 `sensor_read`、`gpio_write` 等）會帶 `id`，
裝置回覆時帶 `re: <id>`，後端用 `device.hub.request()` 等待結果。

## 檔案

| 檔案 | 內容 |
|---|---|
| `src/main.cpp` | 按鍵、畫面流程、預設句子 |
| `src/wifi_manager.*` | 多組 Wi-Fi 選擇、斷線重找、手動切換 |
| `src/setup_portal.*` | 設定模式（熱點 + 網頁） |
| `src/settings.*` | 設定讀寫（NVS） |
| `src/link.*` | 跟 Pi 的 WebSocket |
| `src/ui.*` | 螢幕（中文字型、自動換行、捲動） |
| `include/secrets.example.h` | 出廠預設值範本 |
