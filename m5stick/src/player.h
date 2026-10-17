// 播放 Pi 傳來的語音回覆（16-bit 單聲道 PCM），邊收邊播。
// StickS3 的喇叭和麥克風共用 codec：播放時先關麥克風，播完再切回來。
#pragma once

#include <Arduino.h>

class Player {
 public:
  void begin();  // 建立背景播放工作（setup 時呼叫一次）
  void start(int id, uint32_t sampleRate);
  void feed(const uint8_t* data, size_t len);  // 收到的語音片段
  void finish(int id);                         // Pi 說這段送完了，播完就結束
  void stop();                                 // 立刻停止（例如使用者按 A 要說話）
  void loop();  // 由背景工作呼叫

  bool active() const { return active_; }
  int id() const { return id_; }

 private:
  void queueCurrent();
  void end();

  volatile bool active_ = false;
  bool finished_ = false;
  int id_ = -1;
  uint32_t rate_ = 16000;
};

extern Player player;
