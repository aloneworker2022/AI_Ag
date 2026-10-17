#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

Settings settings;

namespace {
constexpr const char* NVS_NAMESPACE = "aistick";
constexpr const char* NVS_KEY = "cfg";

struct DefaultNet {
  const char* ssid;
  const char* password;
  const char* host;
};

void loadDefaults() {
  settings = Settings();
  settings.deviceName = DEFAULT_DEVICE_NAME;
  settings.serverHost = DEFAULT_SERVER_HOST;
  settings.serverPort = DEFAULT_SERVER_PORT;
  settings.volume = 160;
  settings.voiceOn = true;
  settings.token = DEFAULT_TOKEN;
  static const DefaultNet defaults[] = DEFAULT_NETWORKS;
  for (const auto& n : defaults) {
    settings.addNetwork(n.ssid, n.password, n.host);
  }
}
}  // namespace

const String& Settings::hostFor(int netIndex) const {
  if (netIndex >= 0 && netIndex < networkCount && networks[netIndex].serverHost.length()) {
    return networks[netIndex].serverHost;
  }
  return serverHost;
}

bool Settings::addNetwork(const String& ssid, const String& password, const String& host) {
  if (ssid.isEmpty()) return false;
  // 同名就更新
  for (int i = 0; i < networkCount; i++) {
    if (networks[i].ssid == ssid) {
      networks[i].password = password;
      networks[i].serverHost = host;
      return true;
    }
  }
  if (networkCount >= MAX_NETWORKS) return false;
  networks[networkCount++] = {ssid, password, host};
  return true;
}

void Settings::removeNetwork(int index) {
  if (index < 0 || index >= networkCount) return;
  for (int i = index; i < networkCount - 1; i++) networks[i] = networks[i + 1];
  networks[--networkCount] = NetworkEntry();
}

void Settings::moveUp(int index) {
  if (index <= 0 || index >= networkCount) return;
  NetworkEntry tmp = networks[index - 1];
  networks[index - 1] = networks[index];
  networks[index] = tmp;
}

void settingsLoad() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  String raw = prefs.getString(NVS_KEY, "");
  prefs.end();

  JsonDocument doc;
  if (raw.isEmpty() || deserializeJson(doc, raw)) {
    loadDefaults();
    return;
  }
  settings = Settings();
  settings.deviceName = doc["device"] | DEFAULT_DEVICE_NAME;
  settings.serverHost = doc["host"] | DEFAULT_SERVER_HOST;
  settings.serverPort = doc["port"] | DEFAULT_SERVER_PORT;
  settings.volume = doc["vol"] | 160;
  settings.voiceOn = doc["voice"] | true;
  settings.token = doc["token"] | DEFAULT_TOKEN;
  for (JsonObject n : doc["nets"].as<JsonArray>()) {
    settings.addNetwork(n["ssid"] | "", n["pass"] | "", n["host"] | "");
  }
}

void settingsSave() {
  JsonDocument doc;
  doc["device"] = settings.deviceName;
  doc["host"] = settings.serverHost;
  doc["port"] = settings.serverPort;
  doc["vol"] = settings.volume;
  doc["voice"] = settings.voiceOn;
  doc["token"] = settings.token;
  JsonArray nets = doc["nets"].to<JsonArray>();
  for (int i = 0; i < settings.networkCount; i++) {
    JsonObject n = nets.add<JsonObject>();
    n["ssid"] = settings.networks[i].ssid;
    n["pass"] = settings.networks[i].password;
    n["host"] = settings.networks[i].serverHost;
  }
  String raw;
  serializeJson(doc, raw);

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY, raw);
  prefs.end();
}

void settingsFactoryReset() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  loadDefaults();
}
