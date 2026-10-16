#include "link.h"

#include <ESPmDNS.h>
#include <WebSocketsClient.h>

#include "settings.h"

#ifndef FW_VERSION
#define FW_VERSION "0.2.0"
#endif

Link piLink;

namespace {
WebSocketsClient ws;
constexpr int PROTOCOL_VERSION = 2;

// xxx.local 用 mDNS 解析成 IP（WebSocket 函式庫本身只會查一般 DNS）
String resolveHost(const String& host) {
  if (!host.endsWith(".local")) return host;
  static bool mdnsStarted = false;
  if (!mdnsStarted) mdnsStarted = MDNS.begin(settings.deviceName.c_str());
  String name = host.substring(0, host.length() - 6);
  IPAddress ip = MDNS.queryHost(name, 3000);
  if (ip == IPAddress()) {
    Serial.printf("[link] mDNS 找不到 %s\n", host.c_str());
    return host;
  }
  Serial.printf("[link] %s -> %s\n", host.c_str(), ip.toString().c_str());
  return ip.toString();
}
}  // namespace

void Link::start(const String& host, uint16_t port) {
  stop();
  host_ = host;
  String addr = resolveHost(host);
  Serial.printf("[link] 連線 ws://%s:%u/ws/device\n", addr.c_str(), port);

  ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED: {
        Serial.println("[link] 已連上，送 hello");
        JsonDocument hello;
        hello["type"] = "hello";
        hello["proto"] = PROTOCOL_VERSION;
        hello["device"] = settings.deviceName;
        hello["fw"] = FW_VERSION;
        hello["token"] = settings.token;
        send(hello);
        break;
      }
      case WStype_DISCONNECTED:
        if (ready_) Serial.println("[link] 斷線，稍後自動重連");
        ready_ = false;
        break;
      case WStype_TEXT: {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length)) break;
        if (doc["type"] == "welcome") ready_ = true;
        if (onMessage_) onMessage_(doc);
        break;
      }
      default:
        break;
    }
  });
  ws.begin(addr, port, "/ws/device");
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 5000, 2);  // WebSocket ping；連續 2 次沒回就判定斷線
  started_ = true;
}

void Link::stop() {
  if (started_) ws.disconnect();
  started_ = false;
  ready_ = false;
}

void Link::loop() {
  if (started_) ws.loop();
}

bool Link::send(JsonDocument& doc) {
  if (!started_ || !ws.isConnected()) return false;
  String out;
  serializeJson(doc, out);
  return ws.sendTXT(out);
}

int Link::sendChat(const String& text) {
  if (!ready_) return -1;
  JsonDocument doc;
  int id = nextId_++;
  doc["type"] = "chat";
  doc["id"] = id;
  doc["text"] = text;
  return send(doc) ? id : -1;
}

int Link::audioStart(uint32_t sampleRate) {
  if (!ready_) return -1;
  JsonDocument doc;
  int id = nextId_++;
  doc["type"] = "audio_start";
  doc["id"] = id;
  doc["format"] = "pcm_s16le";
  doc["rate"] = sampleRate;
  return send(doc) ? id : -1;
}

bool Link::sendAudio(const int16_t* samples, size_t count) {
  if (!started_ || !ws.isConnected()) return false;
  // ESP32 是 little-endian，記憶體裡的 int16 就是 pcm_s16le
  return ws.sendBIN(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t));
}

void Link::audioEnd(int id) {
  JsonDocument doc;
  doc["type"] = "audio_end";
  doc["id"] = id;
  send(doc);
}

void Link::audioCancel(int id) {
  JsonDocument doc;
  doc["type"] = "audio_cancel";
  doc["id"] = id;
  send(doc);
}
