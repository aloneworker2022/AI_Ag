# M5StickS3 韌體

按住 A 說話，聲音串流到 Pi 上的 `bridge/`，由 Whisper 轉文字後交給 Hermes Agent，
回覆文字顯示在螢幕上。整體架構與 Pi 端安裝見[根目錄 README](../README.md)。

## 1. 先確認 Pi 端

Pi 上的 bridge 要先跑起來（`python -m bridge`），並記下：

- Pi 的 IP（建議在路由器固定）
- `bridge/config.yaml` 裡的 `device.token`

## 2. 燒錄

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

1. Stick 上**長按 B 開選單 → Wi-Fi → 手機設定模式**（第一次開機、還沒存任何 Wi-Fi 時會自動進入）
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
| **B 在首頁連按兩下** | 開新對話（Hermes 換一個新 session） |
| **B 長按（1 秒）** | 打開選單 |
| 核准畫面：**A / B** | Hermes 要執行需要核准的指令時：A 允許一次、B 拒絕 |
| AI 說話或還在想時按住 **A** | 直接插話（停止播放、取消上一輪）；只點一下 = 叫它閉嘴 |

### 選單（長按 B 打開）

選單裡：**B = 往下一個**（到底回到第一個）、**A = 選擇**。每一層最後一項都是「離開」
（子選單離開回到主選單，主選單離開就關掉選單）。

```
選單
├ Wi-Fi
│  ├ 存好的每組 Wi-Fi（● = 目前連的那組），選了就改連那組
│  ├ 手機設定模式（Stick 開熱點，用手機網頁設定 Wi-Fi、Pi 位址、token、音量）
│  └ 離開
├ 音量
│  ├ 不說話（關掉語音，只顯示文字；Pi 也不會合成聲音）
│  ├ 1 小聲 / 2 / 3 / 4 / 5 最大（● = 目前；選了會嗶一聲讓你試聽，也會重新開啟語音）
│  └ 離開
└ 離開
```

畫面流程：聆聽中（秒數＋音量條）→ 辨識中… → 你說的話＋思考中 →
Hermes 用工具時顯示「使用工具：…」→ 回覆。

狀態列：左邊是目前 Wi-Fi，右邊圓點綠色 = 已連上 Pi、紅色 = 沒連上，最右邊是電量。

在 serial monitor 打一行字按 Enter，會當成文字訊息送出（不用說話也能測試；打 `/new` 開新對話）。

### 從 Pi 推訊息到 Stick 螢幕

```bash
curl http://<pi>:3799/devices          # 看在線裝置
curl -X POST http://<pi>:3799/devices/stick/notify \
     -H 'Content-Type: application/json' -d '{"title":"提醒","text":"該休息囉"}'
```

## 5. 協定（v2）

文字訊息都是 JSON：`{"type": "...", ...}`。
二進位訊息一律屬於「最近一個 `*_start`」開啟的串流（目前只有語音）。

| 方向 | type | 欄位 | 說明 |
|---|---|---|---|
| Stick → Pi | `hello` | `device`, `fw`, `proto`, `token` | 連上後第一則，必須是它；token 錯會被斷線（code 4401） |
| Pi → Stick | `welcome` | `proto`, `agent` | 驗證通過 |
| Stick → Pi | `audio_start` | `id`, `format`=`pcm_s16le`, `rate`=16000 | 開始說話 |
| Stick → Pi | （二進位） | 16-bit little-endian 單聲道 PCM | 邊錄邊傳，每段 32 ms |
| Stick → Pi | `audio_end` / `audio_cancel` | `id` | 說完 / 誤觸取消 |
| Pi → Stick | `transcribing` | `id` | 辨識中 |
| Pi → Stick | `transcript` | `id`, `text` | 辨識結果（你說了什麼） |
| Stick → Pi | `chat` | `id`, `text` | 直接傳文字（序列埠測試用） |
| Pi → Stick | `thinking` | `id` | 已送給 Hermes |
| Pi → Stick | `tool` | `id`, `label` | Hermes 正在用某個工具 |
| Pi → Stick | `status` | `id`, `text` | Hermes 狀態（例如等待模型） |
| Pi → Stick | `approval` | `id`, `command`, `description` | 需要核准的指令 |
| Stick → Pi | `approval_reply` | `id`, `choice`=`once`/`deny` | 核准結果 |
| Pi → Stick | `reply_partial` | `id`, `text` | 回覆產生到一半的文字（每 0.4 秒更新） |
| Pi → Stick | `reply` | `id`, `text` | Hermes 的回覆 |
| Pi → Stick | `audio_reply_start` | `id`, `format`=`pcm_s16le`, `rate` | 語音模式：開始念 |
| Pi → Stick | （二進位） | 16-bit 單聲道 PCM | 語音回覆，Pi 照播放速度送 |
| Pi → Stick | `audio_reply_end` | `id` | 念完 |
| Stick → Pi | `tts_stop` | | 停止念 |
| Stick → Pi | `voice_mode` | `enabled` | 開／關語音回覆（`hello` 也會帶 `voice`） |
| Stick → Pi | `new_session` | | 開新對話 |
| Pi → Stick | `notify` | `title`, `text` | Pi 主動推的訊息 |
| Pi → Stick | `error` | `id`?, `message` | 錯誤（例如「沒聽清楚，再說一次」「連不到 Hermes」） |
| 雙向 | `ping` / `pong` | `t` | 應用層心跳（另外也有 WebSocket ping） |

## 檔案

| 檔案 | 內容 |
|---|---|
| `src/main.cpp` | 按鍵、畫面流程 |
| `src/recorder.*` | 麥克風錄音、分段串流 |
| `src/player.*` | 播放語音回覆（背景工作邊收邊播） |
| `src/wifi_manager.*` | 多組 Wi-Fi 選擇、斷線重找、手動切換 |
| `src/setup_portal.*` | 設定模式（熱點 + 網頁） |
| `src/settings.*` | 設定讀寫（NVS） |
| `src/link.*` | 跟 Pi 的 WebSocket |
| `src/ui.*` | 螢幕（中文字型、自動換行、捲動） |
| `include/secrets.example.h` | 出廠預設值範本 |
