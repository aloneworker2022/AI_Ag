# Win11 電腦上的語音服務

在跑 Ollama 的那台 Win11 電腦（RTX 4070 Ti Super）上，用 Docker 開兩個服務：

| 服務 | 用途 | Port |
|---|---|---|
| **tts**：BreezyVoice（聯發科） | 台灣腔語音合成，可用 10 秒錄音換聲音 | 8080 |
| **asr**：Breeze ASR 25（聯發科） | 台灣華語語音辨識（中英夾雜也行） | 8081 |

全部免費、在自己電腦上跑。Pi 上的 bridge 會把 Stick 收到的聲音送來辨識，
再把 Hermes 的回覆一句一句送來合成、傳回 Stick 播放。

## 1. 準備（只要做一次）

1. 更新 NVIDIA 顯示卡驅動到最新版。
2. 安裝 [Docker Desktop](https://www.docker.com/products/docker-desktop/)（安裝時選 **WSL 2**），裝完重開機。
3. 確認 Docker 用得到顯卡（PowerShell）：
   ```powershell
   docker run --rm --gpus all nvidia/cuda:11.8.0-base-ubuntu22.04 nvidia-smi
   ```
   看到顯卡資訊（RTX 4070 Ti SUPER）就 OK。

## 2. 下載與啟動

```powershell
git clone -b ccr-a91c4a99-4xno4s https://github.com/aloneworker2022/AI_Ag.git
cd AI_Ag\pc_voice
git clone https://github.com/mtkresearch/BreezyVoice.git
copy .env.example .env
docker compose up -d --build
```

第一次會下載很多東西（Docker 映像檔加模型，十幾 GB），要等一陣子。
之後開機 Docker Desktop 啟動，服務就會自動跑起來（`restart: unless-stopped`）。

看狀態與 log：

```powershell
docker compose ps
docker compose logs -f tts
docker compose logs -f asr
```

## 3. 測試

辨識服務：瀏覽器打開 <http://localhost:8081/health>，看到 `"ok": true` 就好。

語音合成（產生 `test.wav`，雙擊聽聽看）：

```powershell
docker compose exec asr python -c "import urllib.request,json; r=urllib.request.urlopen(urllib.request.Request('http://tts:8080/audio/speech', data=json.dumps({'input':'你好，今天過得怎麼樣？'}).encode(), headers={'Content-Type':'application/json'})); open('/tmp/test.wav','wb').write(r.read()); print('ok')"
docker compose cp asr:/tmp/test.wav .\test.wav
```

（`你好…` 是「你好，今天過得怎麼樣？」，這樣寫是為了避開 PowerShell 的中文編碼問題。）

## 4. 讓 Pi 連得進來

Pi 要能連到電腦的 8080、8081。如果 Pi 那邊連不到，用**系統管理員** PowerShell 開防火牆：

```powershell
New-NetFirewallRule -DisplayName "AIStick voice" -Direction Inbound -Protocol TCP -LocalPort 8080,8081 -Action Allow
```

查電腦 IP：`ipconfig`，找「IPv4 位址」（例如 `192.168.68.50`）。建議在路由器幫電腦固定 IP。

然後在 **Pi** 的 `bridge/config.yaml` 加上（IP 換成你的電腦）：

```yaml
stt:
  engine: remote
  remote_url: http://192.168.68.50:8081/asr

voice:
  enabled: true
  tts_url: http://192.168.68.50:8080/audio/speech
```

`style_prompt` 等其他設定可以從 `bridge/config.example.yaml` 複製過去。改完重啟 bridge。

## 5. 換成你喜歡的聲音

1. 準備 8～12 秒、安靜環境、單一個人自然講話的錄音，存成 `pc_voice\voices\speaker.wav`。
   （請用自己或經過本人同意的聲音。）
2. 編輯 `pc_voice\.env`，把這兩行前面的 `#` 拿掉，填上錄音裡講的**逐字稿**：
   ```
   SPEAKER_PROMPT_AUDIO_PATH=/voices/speaker.wav
   SPEAKER_PROMPT_TEXT_TRANSCRIPTION=錄音裡講的每一個字
   ```
3. `docker compose up -d` 重新套用。

聲音的情緒會跟著參考錄音：錄音平靜，講出來就平靜；想活潑一點就錄活潑一點的。

## 顯示卡記憶體

16 GB 要同時放 Ollama（Hermes 的模型）、BreezyVoice、Breeze ASR，會比較緊。
用 `nvidia-smi` 看用量；如果 Ollama 變很慢（模型被擠到 CPU），可以：

- Hermes 改用小一點的模型，或
- 辨識改回在 Pi 上跑（`bridge/config.yaml` 的 `stt.engine: whisper`），然後 `docker compose stop asr`。
