#include "wifi_manager.h"

#include <M5Unified.h>
#include <WiFi.h>

#include "settings.h"

WifiManager wifi;

namespace {
constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
constexpr uint32_t RETRY_INTERVAL_MS = 30000;
constexpr uint32_t LOST_GRACE_MS = 5000;  // 斷線超過這麼久才重找網路
}  // namespace

void WifiManager::begin(StatusFn onStatus) {
  onStatus_ = onStatus;
  WiFi.persistent(false);       // 密碼由我們自己存在 NVS，不讓 Wi-Fi 驅動另存一份
  WiFi.setAutoReconnect(false);  // 斷線由 loop() 決定要重連哪一組
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings.deviceName.c_str());
  connectBest(-1);
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

String WifiManager::ssid() const { return connected() ? WiFi.SSID() : String(); }

int WifiManager::rssi() const { return connected() ? WiFi.RSSI() : 0; }

bool WifiManager::takeChanged() {
  bool c = changed_;
  changed_ = false;
  return c;
}

void WifiManager::switchNext() {
  if (settings.networkCount == 0) {
    onStatus_("沒有存 Wi-Fi\nB 按 5 秒：設定");
    return;
  }
  connectBest(current_);
}

void WifiManager::loop() {
  uint32_t now = millis();
  if (connected()) {
    lostAt_ = 0;
    return;
  }
  if (current_ >= 0) {
    // 原本連著，剛斷：給它幾秒緩衝再重找
    if (lostAt_ == 0) {
      lostAt_ = now;
      changed_ = true;  // 讓主程式知道 Pi 連線也斷了
      return;
    }
    if (now - lostAt_ < LOST_GRACE_MS) return;
    current_ = -1;
    nextRetryAt_ = 0;
  }
  if (now >= nextRetryAt_) connectBest(-1);
}

bool WifiManager::tryConnect(int index) {
  const NetworkEntry& net = settings.networks[index];
  onStatus_("連線中\n" + net.ssid);
  WiFi.disconnect();
  WiFi.begin(net.ssid.c_str(), net.password.c_str());
  uint32_t start = millis();
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  WiFi.disconnect();
  return false;
}

bool WifiManager::connectBest(int after) {
  const int count = settings.networkCount;
  if (count == 0) {
    onStatus_("沒有存 Wi-Fi\nB 按 5 秒：設定");
    nextRetryAt_ = millis() + RETRY_INTERVAL_MS;
    return false;
  }

  onStatus_("掃描 Wi-Fi…");
  WiFi.disconnect();
  int found = WiFi.scanNetworks();

  // 嘗試順序：一般從清單第 0 組開始；手動切換時從「目前這組的下一組」開始，目前這組放最後
  int order[MAX_NETWORKS];
  for (int k = 0; k < count; k++) order[k] = (after + 1 + k) % count;

  bool ok = false;
  for (int k = 0; k < count && !ok; k++) {
    int idx = order[k];
    bool visible = false;
    for (int s = 0; s < found; s++) {
      if (WiFi.SSID(s) == settings.networks[idx].ssid) {
        visible = true;
        break;
      }
    }
    if (!visible) continue;
    if (tryConnect(idx)) {
      current_ = idx;
      ok = true;
    }
  }
  WiFi.scanDelete();

  if (ok) {
    lostAt_ = 0;
    changed_ = true;
    onStatus_("已連上\n" + settings.networks[current_].ssid);
    return true;
  }
  current_ = -1;
  nextRetryAt_ = millis() + RETRY_INTERVAL_MS;
  onStatus_("找不到 Wi-Fi\n30 秒後重試\nB 按 5 秒：設定");
  return false;
}
