// 麥克風錄音：16 kHz、16-bit 單聲道，一邊錄一邊把小段 PCM 交給呼叫端（傳給 Pi）。
#pragma once

#include <Arduino.h>
#include <functional>

class Recorder {
 public:
  using ChunkFn = std::function<void(const int16_t* data, size_t samples)>;

  static constexpr uint32_t SAMPLE_RATE = 16000;

  void begin();
  void start();
  // 錄音中每圈 loop 呼叫：補排緩衝、把錄好的段落交給 onChunk
  void loop(const ChunkFn& onChunk);
  // 停止並把剩下的段落交出去
  void stop(const ChunkFn& onChunk);

  bool recording() const { return recording_; }
  uint32_t elapsedMs() const { return recording_ ? millis() - startedAt_ : 0; }
  int level() const { return level_; }  // 最近一段的音量 0–100，給畫面用

 private:
  void drain(const ChunkFn& onChunk);

  bool recording_ = false;
  uint32_t startedAt_ = 0;
  size_t next_ = 0;
  int level_ = 0;
};

extern Recorder recorder;
