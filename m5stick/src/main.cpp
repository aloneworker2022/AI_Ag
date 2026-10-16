// AI 伴侶隨身終端（M5StickS3）— 第 1 階段：文字對話
//
// 按鍵：
//   A 單按：送出目前這句 / 在回覆畫面回到首頁
//   B 單按：下一句 / 在回覆畫面往下捲
//   B 長按（1.5 秒）：切換到清單裡的下一組 Wi-Fi（家裡 ⇄ 手機熱點）
//   A 長按（3 秒）：進入設定模式（用手機設定 Wi-Fi、Pi 位址、token）
// 序列埠：在 serial monitor 打一行字按 Enter，也會當成訊息送給 Pi。
#include <M5Unified.h>

#include "link.h"
#include "settings.h"
#include "setup_portal.h"
#include "ui.h"
#include "wifi_manager.h"

namespace {
const char* PRESETS[] = {
    "早安！",
    "我回來了～",
    "今天好累喔",
    "陪我聊聊天",
    "你現在在做什麼？",
    "晚安，明天見",
};
constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
constexpr uint32_t REPLY_TIMEOUT_MS = 180000;  // Pi 上的 LLM 可能很慢

int presetIndex = 0;
int waitingId = -1;
uint32_t waitingSince = 0;
String companionName = "伴侶";
String serialLine;

void showHome() { ui.showHome(PRESETS[presetIndex], presetIndex, PRESET_COUNT); }

void sendText(const String& text) {
  if (waitingId >= 0) return;
  int id = piLink.sendChat(text);
  if (id < 0) {
    ui.showReply("沒連上 Pi", "Pi：" + piLink.host() + ":" + String(settings.serverPort) +
                                  "\n確認 Pi 有開、位址和 token 對不對");
    return;
  }
  waitingId = id;
  waitingSince = millis();
  ui.showThinking(text);
}

void onPiMessage(JsonDocument& doc) {
  String type = doc["type"] | "";
  int id = doc["id"] | -1;

  if (type == "welcome") {
    companionName = doc["companion"] | "伴侶";
    Serial.printf("[link] welcome，伴侶：%s\n", companionName.c_str());
    if (waitingId < 0 && !ui.isReply()) showHome();
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
    String title = doc["title"] | "";
    ui.showReply(title.length() ? title : companionName, doc["text"] | "");
  }
  // thinking / pong：不用處理
}

void onWifiStatus(const String& msg) {
  Serial.println("[wifi] " + msg);
  ui.showInfo("Wi-Fi", msg);
}

void handleButtons() {
  if (M5.BtnA.wasHold()) {
    runSetupPortal();  // 不會返回
  }
  if (M5.BtnB.wasHold()) {
    piLink.stop();
    waitingId = -1;
    wifi.switchNext();
    return;
  }
  if (M5.BtnA.wasClicked()) {
    if (ui.isReply()) {
      showHome();
    } else if (waitingId < 0) {
      sendText(PRESETS[presetIndex]);
    }
  }
  if (M5.BtnB.wasClicked()) {
    if (ui.isReply()) {
      if (!ui.scrollDown()) showHome();
    } else if (waitingId < 0) {
      presetIndex = (presetIndex + 1) % PRESET_COUNT;
      showHome();
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
  M5.BtnA.setHoldThresh(3000);
  M5.BtnB.setHoldThresh(1500);

  ui.begin();
  Serial.printf("[boot] board=%d PSRAM=%s (%u bytes)\n", (int)M5.getBoard(),
                psramFound() ? "有" : "沒有", ESP.getPsramSize());

  settingsLoad();
  if (settings.networkCount == 0) runSetupPortal();  // 第一次用：直接進設定

  piLink.onMessage(onPiMessage);
  wifi.begin(onWifiStatus);
}

void loop() {
  M5.update();
  wifi.loop();

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
  delay(5);
}
