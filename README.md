# M5StickS3 × Hermes Agent

對著 M5StickS3 說話，由 [Hermes Agent](https://github.com/NousResearch/hermes-agent)
（Nous Research 的開源 agent）回答，回覆文字顯示在 Stick 螢幕上；
開啟語音模式後，還會用台灣腔（聯發科 BreezyVoice）一邊想一邊說出來，可以隨時插話。
全部免費，在自己的機器上跑。

```
M5StickS3 ──Wi-Fi(WebSocket)──▶ Raspberry Pi 5                       你的電腦
 按住 A 說話                     ├ bridge（本專案）                    ├ Ollama / LM Studio
 螢幕顯示回覆                    │  ├ Whisper 語音轉文字                │  （提供模型）
                                │  └ 轉送給 Hermes ──▶ Hermes Agent ──▶┘
                                │                     （記憶、skills、工具）
```

| 角色 | 跑在哪 | 做什麼 |
|---|---|---|
| `m5stick/` 韌體 | M5StickS3 | 錄音串流、顯示回覆、多組 Wi-Fi |
| `bridge/` | Raspberry Pi 5 | 收 Stick 的語音 → Whisper 轉文字 → 呼叫 Hermes → 回傳文字 |
| Hermes Agent | Raspberry Pi 5 | agent 本體：對話歷史、長期記憶、skills、工具 |
| 模型伺服器 | 你的電腦 | Ollama / LM Studio 等 OpenAI 相容服務，給 Hermes 用 |
| `pc_voice/` 語音服務（選用） | 你的電腦 | BreezyVoice 台灣腔語音合成 + Breeze ASR 25 台灣華語辨識，見 [pc_voice/README.md](pc_voice/README.md) |

## 1. 電腦：準備模型

以 Ollama 為例（LM Studio、vLLM、llama.cpp 也可以，只要是 OpenAI 相容 API）：

```bash
ollama pull qwen2.5:14b            # 挑一個支援 tool calling、你的顯卡跑得動的模型
OLLAMA_HOST=0.0.0.0 OLLAMA_CONTEXT_LENGTH=64000 ollama serve
```

- `OLLAMA_HOST=0.0.0.0`：讓 Pi 連得進來（預設只聽本機）。
- `OLLAMA_CONTEXT_LENGTH=64000`：**Hermes 要求至少 64K context**，Ollama 預設只有 4K，
  不設會被 Hermes 拒絕。
- 建議在路由器幫電腦固定 IP，並確認防火牆允許 11434 port。

## 2. Pi：安裝 Hermes Agent

```bash
curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash
hermes model
#   選 "Custom endpoint (self-hosted / VLLM / etc.)"
#   URL 填 http://<電腦IP>:11434/v1，API key 留空，模型名稱填 qwen2.5:14b
```

開啟 API server，在 `~/.hermes/.env` 加上：

```bash
API_SERVER_ENABLED=true
API_SERVER_KEY=換成至少16個字元的金鑰
```

**金鑰一定要 16 個字元以上**，太短或像 `changeme` 這種預設值，Hermes 會**直接不開 API server、也不報錯**
（8642 port 就不會有人聽）。可以用 `openssl rand -hex 16` 產生一組。

然後啟動 `hermes gateway`（已經在跑的話，改完 `.env` 要 `hermes gateway restart`），
看到 `API server listening on http://127.0.0.1:8642` 就成功了。
確認：`ss -ltnp | grep 8642`。
bridge 跟 Hermes 在同一台 Pi，所以 Hermes 只聽 127.0.0.1 就好，不用對外開。

測試：

```bash
curl http://127.0.0.1:8642/v1/chat/completions \
  -H "Authorization: Bearer <API_SERVER_KEY>" -H "Content-Type: application/json" \
  -d '{"model":"hermes-agent","messages":[{"role":"user","content":"你好"}]}'
```

## 3. Pi：啟動 bridge

```bash
cd AI_Ag
python3 -m venv .venv && . .venv/bin/activate
pip install -r bridge/requirements.txt
cp bridge/config.example.yaml bridge/config.yaml
#   改 hermes.api_key（= API_SERVER_KEY）和 device.token（Stick 用的密碼）
python -m bridge
```

- 第一次說話時會下載 Whisper 模型（`stt.model`，預設 `small`），Pi 要能上網一次。
- 開機自動啟動：見 `bridge/systemd/m5stick-bridge.service`。
- 檢查：`curl http://<PiIP>:3799/health`、`curl http://<PiIP>:3799/devices`。

## 4. 電腦：語音服務（選用，要語音回覆才需要）

見 [pc_voice/README.md](pc_voice/README.md)：Docker Desktop 一鍵啟動 BreezyVoice 與 Breeze ASR，
再在 Pi 的 `bridge/config.yaml` 開 `voice.enabled`、`stt.engine: remote`。

## 5. Stick：燒錄韌體

見 [m5stick/README.md](m5stick/README.md)：燒錄、Wi-Fi 設定（多組、手機熱點切換）、按鍵操作、協定。

## 用起來

- **按住 A 說話**，放開送出 → 螢幕顯示「辨識中」→ 你說的話 →
  Hermes 用了哪些工具（例如「使用工具：web_search」）→ 回覆（語音模式會一邊產生一邊念出來，AI 講話時按住 A 可以直接插話）。
- Hermes 要執行需要核准的指令時，Stick 會顯示指令內容：**A 允許一次、B 拒絕**（2 分鐘沒按就拒絕）。
- 對話會一直接續（Hermes 記得前面講過什麼）。想重新開始：首頁**連按兩下 B**，
  或說「開新對話」。
- 從 Pi 推訊息到 Stick：
  `curl -X POST http://<PiIP>:3799/devices/stick/notify -H 'Content-Type: application/json' -d '{"text":"該休息了"}'`
