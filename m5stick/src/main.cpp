// AI 伴侶隨身終端（M5StickS3）— 第 1 階段：用說的，Pi 回覆文字
//
// 按鍵：
//   A 按住：說話（放開就送出給 Pi 辨識）
//   B 單按：回覆畫面往下捲，捲到底回首頁
//   B 長按 1.5 秒放開：切換到清單裡的下一組 Wi-Fi（家裡 ⇄ 手機熱點）
//   B 長按 5 秒放開：進入設定模式（用手機設定 Wi-Fi、Pi 位址、token）
// 序列埠：在 serial monitor 打一行字按 Enter，會當成文字訊息送給 Pi（測試用）。
#include <M5Unified.h>

#include "link.h"
#include "recorder.h"
#include "settings.h"
#include "setup_portal.h"
#include "ui.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t REPLY_TIMEOUT_MS = 180000;  // Pi 上的辨識 + LLM 可能很慢
constexpr uint32_t MIN_SPEECH_MS = 400;        // 按太短當作誤觸
constexpr uint32_t MAX_SPEECH_MS = 30000;      // 跟 Pi 的 stt.max_seconds 一致
constexpr uint32_t HOLD_SWITCH_MS = 1500;
constexpr uint32_t HOLD_SETUP_MS = 5000;

int waitingId = -1;  // 等 Pi 回覆中的訊息 id
uint32_t waitingSince = 0;
int audioId = -1;    // 錄音串流中的訊息 id
String said;         // 辨識出來的話
String companionName = "伴侶";
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

  if (type == "welcome") {
    companionName = doc["companion"] | "伴侶";
    Serial.printf("[link] welcome，伴侶：%s\n", companionName.c_str());
    if (waitingId < 0 && !recorder.recording() && !ui.isReply()) ui.showHome();
  } else if (type == "transcript") {
    if (id != waitingId) return;
    said = doc["text"] | "";
    Serial.printf("你：%s\n", said.c_str());
    ui.showThinking(said, "思考中…");
  } else if (type == "thinking") {
    if (id == waitingId && said.isEmpty()) ui.showThinking("", "思考中…");
  } else if (type == "reply") {
    if (id != waitingId) return;  // 過期的回覆（例如逾時後才到）
    waitingId = -1;
    String text = doc["text"] | "";
    Serial.printf("%s：%s\n", companionName.c_str(), text.c_str());
    ui.showReply(companionName, text);
  } else if (type == "error") {
    if (id >= 0 && id != waitingId) return;
    waitingId = -1;
    String msg = doc["message"] | "未知錯誤";
    Serial.printf("[error] %s\n", msg.c_str());
    ui.showReply("錯誤", msg);
  } else if (type == "notify") {
    if (recorder.recording()) return;  // 說話中不打斷
    String title = doc["title"] | "";
    ui.showReply(title.length() ? title : companionName, doc["text"] | "");
  }
  // transcribing / pong：不用處理（畫面已經是「辨識中…」）
}

void onWifiStatus(const String& msg) {
  Serial.println("[wifi] " + msg);
  ui.showInfo("Wi-Fi", msg);
}

// ---- 按鍵 ----
void handleButtons() {
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
    if (held >= HOLD_SETUP_MS) {
      runSetupPortal();  // 不會返回
    } else if (held >= HOLD_SWITCH_MS) {
      piLink.stop();
      waitingId = -1;
      wifi.switchNext();
    } else if (ui.isReply()) {
      if (!ui.scrollDown()) ui.showHome();
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
  Serial.printf("[boot] 麥克風：%s\n", M5.Mic.isEnabled() ? "OK" : "失敗");

  settingsLoad();
  if (settings.networkCount == 0) runSetupPortal();  // 第一次用：直接進設定

  piLink.onMessage(onPiMessage);
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
      if (millis() - lastDraw > 100) {
        lastDraw = millis();
        ui.showRecording(recorder.elapsedMs(), recorder.level());
      }
    }
  }

  if (waitingId >= 0 && (!piLink.ready() || millis() - waitingSince > REPLY_TIMEOUT_MS)) {
    waitingId = -1;
    ui.showReply("錯誤", piLink.ready() ? "等太久沒有回覆" : "跟 Pi 的連線斷了");
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
