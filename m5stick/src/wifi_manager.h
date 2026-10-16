// 多組 Wi-Fi：依清單順序找第一個「附近有、連得上」的網路。
// 斷線會自動重找；也可以手動切到清單裡的下一組（例如從家裡 Wi-Fi 切到手機熱點）。
#pragma once

#include <Arduino.h>
#include <functional>

class WifiManager {
 public:
  using StatusFn = std::function<void(const String&)>;

  void begin(StatusFn onStatus);
  void loop();

  // 切到清單中「目前這組之後」第一個連得上的網路
  void switchNext();

  bool connected() const;
  int currentIndex() const { return current_; }
  String ssid() const;
  int rssi() const;

  // 連線換了（新連上或換網路）時回傳 true 一次，讓主程式重連 Pi
  bool takeChanged();

 private:
  bool connectBest(int after);
  bool tryConnect(int index);

  StatusFn onStatus_;
  int current_ = -1;
  bool changed_ = false;
  uint32_t nextRetryAt_ = 0;
  uint32_t lostAt_ = 0;
};

extern WifiManager wifi;
