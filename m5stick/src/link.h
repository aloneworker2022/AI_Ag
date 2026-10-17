// 跟 Pi 的 WebSocket 長連線（ws://<host>:<port>/ws/device）。
// 連上就送 hello（含 token），斷線自動重連。訊息格式見 m5stick/README.md。
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

class Link {
 public:
  using MessageFn = std::function<void(JsonDocument&)>;

  void onMessage(MessageFn fn) { onMessage_ = fn; }

  // Wi-Fi 換了就重新呼叫，會改連到該網路對應的 Pi 位址
  void start(const String& host, uint16_t port);
  void stop();
  void loop();

  // 收到 welcome 之後才算連上
  bool ready() const { return ready_; }
  const String& host() const { return host_; }

  bool send(JsonDocument& doc);
  int sendChat(const String& text);  // 回傳訊息 id；沒連上回傳 -1

  // 語音串流：audioStart → sendAudio（多次）→ audioEnd（或 audioCancel）
  int audioStart(uint32_t sampleRate);  // 回傳訊息 id；沒連上回傳 -1
  bool sendAudio(const int16_t* samples, size_t count);
  void audioEnd(int id);
  void audioCancel(int id);

 private:
  MessageFn onMessage_;
  String host_;
  bool started_ = false;
  bool ready_ = false;
  int nextId_ = 1;
};

extern Link piLink;
