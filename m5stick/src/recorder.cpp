#include "recorder.h"

#include <M5Unified.h>
#include <freertos/queue.h>

Recorder recorder;

namespace {
// 每段 512 樣本 = 32 ms。麥克風同時最多排 2 段，其餘是錄好等著送的
constexpr size_t CHUNK = 512;
constexpr size_t RING = 8;
int16_t buffers[RING][CHUNK];
QueueHandle_t done = nullptr;

// 麥克風工作執行緒填滿一段時呼叫：只把指標丟進佇列，不做別的
void onReleased(void*, void* data, size_t) {
  xQueueSend(done, &data, 0);
}
}  // namespace

void Recorder::begin() {
  done = xQueueCreate(RING, sizeof(void*));
  M5.Speaker.end();  // StickS3 的喇叭和麥克風共用 codec，第 1 階段只用麥克風
  M5.Mic.setBufferReleaseCallback(nullptr, onReleased);
  M5.Mic.begin();
}

void Recorder::start() {
  xQueueReset(done);
  next_ = 0;
  level_ = 0;
  startedAt_ = millis();
  recording_ = true;
}

void Recorder::drain(const ChunkFn& onChunk) {
  void* p;
  while (xQueueReceive(done, &p, 0) == pdTRUE) {
    const int16_t* data = static_cast<const int16_t*>(p);
    int peak = 0;
    for (size_t i = 0; i < CHUNK; i++) peak = max(peak, abs((int)data[i]));
    level_ = min(100, peak * 100 / 12000);
    onChunk(data, CHUNK);
  }
}

void Recorder::loop(const ChunkFn& onChunk) {
  if (!recording_) return;
  // isRecording() == 2 表示兩個位置都排滿了，這時再呼叫 record() 會卡住，所以先跳過
  while (M5.Mic.isRecording() < 2) {
    if (!M5.Mic.record(buffers[next_], CHUNK, SAMPLE_RATE)) break;
    next_ = (next_ + 1) % RING;
  }
  drain(onChunk);
}

void Recorder::stop(const ChunkFn& onChunk) {
  if (!recording_) return;
  recording_ = false;
  uint32_t t = millis();
  while (M5.Mic.isRecording() && millis() - t < 200) delay(1);  // 等排著的段落錄完
  delay(2);  // 釋放回呼可能比 isRecording() 歸零晚一點點
  drain(onChunk);
}
