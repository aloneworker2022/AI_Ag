// Hermes Agent 隨身終端（M5StickS3）：用說的，Hermes 回覆文字
//
// Stick ──WebSocket──▶ Pi 上的 bridge（Whisper 語音辨識）──▶ Hermes Agent
//
// 按鍵：
//   A 按住：說話（放開就送出給 Pi 辨識）
//   B 單按：回覆畫面往下捲，捲到底回首頁
//   B 在首頁連按兩下：開新對話（Hermes 換新 session）
//   Hermes 要求核准指令時：A 允許一次、B 拒絕
//   念回覆時按 A：停止播放（按住就直接開始說下一句）
//   B 長按 1.5 秒放開：切換到清單裡的下一組 Wi-Fi（家裡 ⇄ 手機熱點）
//   B 長按 5 秒放開：進入設定模式（用手機設定 Wi-Fi、Pi 位址、token）
// 序列埠：在 serial monitor 打一行字按 Enter，會當成文字訊息送給 Pi（測試用）。
#include <M5Unified.h>
#include <WiFi.h>

#include "link.h"
#include "player.h"
#include "recorder.h"
#include "settings.h"
#include "setup_portal.h"
#include "ui.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t IDLE_TIMEOUT_MS = 300000;  // 超過這麼久沒收到任何進度才算逾時（agent 可能跑很久）
constexpr uint32_t DOUBLE_CLICK_MS = 2500;
constexpr uint32_t RECONNECT_GRACE_MS = 60000;  // 等回覆時斷線，最多等這麼久重連
constexpr uint32_t MIN_SPEECH_MS = 400;        // 按太短當作誤觸
constexpr uint32_t MAX_SPEECH_MS = 30000;      // 跟 Pi 的 stt.max_seconds 一致
constexpr uint32_t HOLD_SWITCH_MS = 1500;
constexpr uint32_t HOLD_SETUP_MS = 5000;

int waitingId = -1;  // 等 Pi 回覆中的訊息 id
uint32_t waitingSince = 0;
int audioId = -1;    // 錄音串流中的訊息 id
String said;         // 辨識出來的話
int lastReplyId = -1;  // 最近一次顯示的回覆，語音回覆只播這一則
String agentName = "Hermes";
bool approvalPending = false;
uint32_t lastHomeClickAt = 0;
String serialLine;
uint32_t bPressedAt = 0;
int bHintShown = 0;

void startWaiting(int id, const String& text, const String& status) {
  waitingId = id;
  waitingSince = millis();
  said = text;
  ui.showThinking(text, status);
}

void showNotConnected() {
  ui.showReply("沒連上 Pi", "Pi：" + piLink.host() + ":" + String(settings.serverPort) +
                                "\n確認 Pi 有開、位址和 token 對不對");
}

void sendText(const String& text) {
  if (waitingId >= 0 || recorder.recording()) return;
  int id = piLink.sendChat(text);
  if (id < 0) {
    showNotConnected();
    return;
  }
  startWaiting(id, text, "思考中…");
}

// ---- 錄音 ----
void streamChunk(const int16_t* data, size_t n) {
  if (audioId >= 0) piLink.sendAudio(data, n);
}

void startSpeaking() {
  if (player.active()) player.stop();  // 正在念回覆：先停掉（Pi 收到 audio_start 也會停）
  if (waitingId >= 0) return;  // 上一句還沒回
  if (!piLink.ready()) {
    showNotConnected();
    return;
  }
  audioId = piLink.audioStart(Recorder::SAMPLE_RATE);
  if (audioId < 0) {
    showNotConnected();
    return;
  }
  recorder.start();
  ui.showRecording(0, 0);
}

void stopSpeaking() {
  if (!recorder.recording()) return;
  uint32_t ms = recorder.elapsedMs();
  recorder.stop(streamChunk);
  int id = audioId;
  audioId = -1;
  if (ms < MIN_SPEECH_MS) {
    piLink.audioCancel(id);
    ui.showHome();
    return;
  }
  piLink.audioEnd(id);
  startWaiting(id, "", "辨識中…");
}

// ---- Pi 傳來的訊息 ----
void onPiMessage(JsonDocument& doc) {
  String type = doc["type"] | "";
  int id = doc["id"] | -1;

  if (id >= 0 && id == waitingId) waitingSince = millis();  // 有進度就重新計時

  if (type == "welcome") {
    agentName = doc["agent"] | "Hermes";
    Serial.printf("[link] welcome，agent：%s\n", agentName.c_str());
    if (waitingId < 0 && !recorder.recording() && !ui.isReply()) ui.showHome();
  } else if (type == "transcript") {
    if (id != waitingId) return;
    said = doc["text"] | "";
    Serial.printf("你：%s\n", said.c_str());
    ui.showThinking(said, "思考中…");
  } else if (type == "thinking") {
    if (id == waitingId && said.isEmpty()) ui.showThinking("", "思考中…");
  } else if (type == "tool" || type == "status") {
    if (id != waitingId || approvalPending) return;
    String what = type == "tool" ? "使用工具：" + String(doc["label"] | "") : String(doc["text"] | "");
    Serial.println("[agent] " + what);
    ui.showThinking(said, what);
  } else if (type == "approval") {
    if (id != waitingId) return;
    approvalPending = true;
    String cmd = doc["command"] | "";
    Serial.printf("[approval] %s（A 允許 / B 拒絕）\n", cmd.c_str());
    ui.showApproval(cmd, doc["description"] | "");
  } else if (type == "reply") {
    if (id != waitingId) return;  // 過期的回覆（例如逾時後才到）
    waitingId = -1;
    approvalPending = false;
    lastReplyId = id;
    String text = doc["text"] | "";
    Serial.printf("%s：%s\n", agentName.c_str(), text.c_str());
    ui.showReply(agentName, text);
  } else if (type == "error") {
    if (id >= 0 && id != waitingId) return;
    waitingId = -1;
    approvalPending = false;
    String msg = doc["message"] | "未知錯誤";
    Serial.printf("[error] %s\n", msg.c_str());
    ui.showReply("錯誤", msg);
  } else if (type == "audio_reply_start") {
    if (id != lastReplyId || recorder.recording()) return;
    player.start(id, doc["rate"] | 16000);
    Serial.println("[voice] 開始播放回覆");
  } else if (type == "audio_reply_end") {
    player.finish(id);
  } else if (type == "notify") {
    if (recorder.recording()) return;  // 說話中不打斷
    String title = doc["title"] | "";
    ui.showReply(title.length() ? title : agentName, doc["text"] | "");
  }
  // transcribing / pong：不用處理（畫面已經是「辨識中…」）
}

void onWifiStatus(const String& msg) {
  Serial.println("[wifi] " + msg);
  ui.showInfo("Wi-Fi", msg);
}

void sendSimple(const char* type, const char* choice = nullptr) {
  JsonDocument doc;
  doc["type"] = type;
  if (waitingId >= 0) doc["id"] = waitingId;
  if (choice) doc["choice"] = choice;
  piLink.send(doc);
}

// ---- 按鍵 ----
void handleButtons() {
  // 核准畫面：A 允許一次、B 拒絕
  if (approvalPending) {
    bool allow = M5.BtnA.wasPressed();
    bool deny = M5.BtnB.wasPressed();
    if (allow || deny) {
      sendSimple("approval_reply", allow ? "once" : "deny");
      approvalPending = false;
      ui.showThinking(said, allow ? "已允許，繼續中…" : "已拒絕，繼續中…");
      bPressedAt = millis();
      bHintShown = 3;  // 這次 B 放開不要再觸發別的動作
    }
    return;
  }

  // A：按住說話
  if (M5.BtnA.wasPressed()) startSpeaking();
  if (M5.BtnA.wasReleased()) stopSpeaking();

  // B：依按住時間決定動作，按住時提示放開會做什麼
  if (M5.BtnB.wasPressed()) {
    bPressedAt = millis();
    bHintShown = 0;
  }
  if (M5.BtnB.isPressed() && !recorder.recording()) {
    uint32_t held = millis() - bPressedAt;
    if (held >= HOLD_SETUP_MS && bHintShown < 2) {
      ui.showHint("放開：進入設定模式");
      bHintShown = 2;
    } else if (held >= HOLD_SWITCH_MS && held < HOLD_SETUP_MS && bHintShown < 1) {
      ui.showHint("放開：換下一組 Wi-Fi");
      bHintShown = 1;
    }
  }
  if (M5.BtnB.wasReleased() && !recorder.recording()) {
    uint32_t held = millis() - bPressedAt;
    if (bHintShown == 3) {  // 這次按 B 是用來回覆核准
      bHintShown = 0;
      return;
    }
    if (held >= HOLD_SETUP_MS) {
      runSetupPortal();  // 不會返回
    } else if (held >= HOLD_SWITCH_MS) {
      piLink.stop();
      waitingId = -1;
      wifi.switchNext();
    } else if (ui.isReply()) {
      if (!ui.scrollDown()) ui.showHome();
    } else if (waitingId < 0 && ui.isHome()) {
      if (millis() - lastHomeClickAt < DOUBLE_CLICK_MS) {
        lastHomeClickAt = 0;
        if (piLink.ready()) {
          sendSimple("new_session");
        } else {
          showNotConnected();
        }
      } else {
        lastHomeClickAt = millis();
        ui.showHint("再按一次 B：開新對話");
      }
    } else if (waitingId < 0) {
      ui.showHome();
    }
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialLine.trim();
      if (serialLine.length()) sendText(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 500) {
      serialLine += c;
    }
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  ui.begin();
  Serial.printf("[boot] board=%d PSRAM=%s (%u bytes)\n", (int)M5.getBoard(),
                psramFound() ? "有" : "沒有", ESP.getPsramSize());

  recorder.begin();
  player.begin();
  Serial.printf("[boot] 麥克風：%s\n", M5.Mic.isEnabled() ? "OK" : "失敗");

  settingsLoad();
  if (settings.networkCount == 0) runSetupPortal();  // 第一次用：直接進設定

  piLink.onMessage(onPiMessage);
  piLink.onBinary([](const uint8_t* data, size_t len) { player.feed(data, len); });
  wifi.begin(onWifiStatus);
}

void loop() {
  M5.update();
  if (!recorder.recording()) wifi.loop();  // 說話中不做可能卡住的 Wi-Fi 重找

  if (wifi.takeChanged()) {
    if (wifi.connected()) {
      int idx = wifi.currentIndex();
      piLink.start(settings.hostFor(idx), settings.serverPort);
      ui.showInfo("Wi-Fi", "已連上 " + wifi.ssid() + "\n連線到 Pi…\n" + settings.hostFor(idx));
    } else {
      piLink.stop();
    }
  }
  piLink.loop();

  if (recorder.recording()) {
    recorder.loop(streamChunk);
    if (!piLink.ready()) {  // 錄到一半斷線
      recorder.stop([](const int16_t*, size_t) {});
      audioId = -1;
      ui.showReply("錯誤", "跟 Pi 的連線斷了");
    } else if (recorder.elapsedMs() >= MAX_SPEECH_MS) {
      stopSpeaking();  // 太長自動送出
    } else {
      static uint32_t lastDraw = 0;
      if (millis() - lastDraw > 250) {  // 少畫幾次，把時間留給錄音
        lastDraw = millis();
        ui.showRecording(recorder.elapsedMs(), recorder.level());
      }
    }
  }

  // Wi-Fi 省電：待機時開著省電；說話、等回覆時暫時關掉，連線比較穩
  static bool sleepOn = true;
  bool busy = recorder.recording() || waitingId >= 0 || player.active();
  if (busy == sleepOn && wifi.connected()) {
    sleepOn = !busy;
    WiFi.setSleep(sleepOn);
  }

  // 等回覆時斷線：先等它自動重連（Pi 會把這一輪跑完，重連後補送回覆），太久才放棄
  static uint32_t lostSince = 0;
  if (waitingId >= 0) {
    if (!piLink.ready()) {
      if (lostSince == 0) {
        lostSince = millis();
        ui.showThinking(said, "連線中斷，重新連線中…");
      } else if (millis() - lostSince > RECONNECT_GRACE_MS) {
        waitingId = -1;
        approvalPending = false;
        lostSince = 0;
        ui.showReply("錯誤", "跟 Pi 的連線斷了");
      }
    } else {
      if (lostSince != 0) {
        lostSince = 0;
        waitingSince = millis();
        ui.showThinking(said, "已重新連上，等待回覆…");
      }
      if (millis() - waitingSince > IDLE_TIMEOUT_MS) {
        waitingId = -1;
        approvalPending = false;
        ui.showReply("錯誤", "等太久沒有回覆");
      }
    }
  } else {
    lostSince = 0;
  }

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 500) {
    lastStatus = millis();
    // 每分鐘強制重畫一次，讓電量跟著更新
    static uint32_t lastForce = 0;
    bool force = millis() - lastForce > 60000;
    if (force) lastForce = millis();
    ui.setStatus(wifi.ssid(), piLink.ready(), force);
  }

  handleButtons();
  handleSerial();
  if (!recorder.recording()) delay(5);
}
