// 裝置設定：多組 Wi-Fi、Pi 位址、token。存在 NVS（斷電不會消失）。
#pragma once

#include <Arduino.h>

constexpr int MAX_NETWORKS = 6;

struct NetworkEntry {
  String ssid;
  String password;
  String serverHost;  // 空字串 = 用 Settings::serverHost
};

struct Settings {
  String deviceName;
  String serverHost;
  uint16_t serverPort = 8000;
  String token;
  NetworkEntry networks[MAX_NETWORKS];
  int networkCount = 0;

  // 連上某組 Wi-Fi 時要用的 Pi 位址
  const String& hostFor(int netIndex) const;

  bool addNetwork(const String& ssid, const String& password, const String& host);
  void removeNetwork(int index);
  void moveUp(int index);
};

// 全域設定
extern Settings settings;

void settingsLoad();          // 開機讀取；NVS 沒資料就用 secrets.h 預設值
void settingsSave();
void settingsFactoryReset();  // 清掉 NVS 回到 secrets.h 預設值
