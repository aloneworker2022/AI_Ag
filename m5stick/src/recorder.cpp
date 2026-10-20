#include "recorder.h"

#include <M5Unified.h>
#include <freertos/queue.h>

Recorder recorder;

namespace {
// 每段 1024 樣本 = 64 ms，共 16 段 ≈ 1 秒緩衝。
// 錄音由獨立的背景工作負責排給麥克風，主迴圈（畫面、網路）卡一下也不會掉聲音。
constexpr size_t REC_CHUNK = 1024;
constexpr size_t REC_RING = 16;
int16_t recBuf[REC_RING][REC_CHUNK];
QueueHandle_t recFreeQ = nullptr;  // 空的緩衝
QueueHandle_t recDoneQ = nullptr;  // 錄好、等主迴圈送出的緩衝
volatile bool recActive = false;
volatile uint32_t recDropped = 0;

int recIndexOf(const void* p) {
  for (size_t i = 0; i < REC_RING; i++) {
    if (p == recBuf[i]) return i;
  }
  return -1;
}

// 麥克風錄滿一段時呼叫（麥克風的工作執行緒）：只把編號丟進佇列
void onRecReleased(void*, void* data, size_t) {
  int i = recIndexOf(data);
  if (i >= 0) xQueueSend(recDoneQ, &i, 0);
}

void resetRecQueues() {
  xQueueReset(recDoneQ);
  xQueueReset(recFreeQ);
  for (int i = 0; i < (int)REC_RING; i++) xQueueSend(recFreeQ, &i, 0);
}

// 背景工作：錄音中就一直把空緩衝排給麥克風（record() 在兩個位置都滿時會自己等）
void recTask(void*) {
  for (;;) {
    if (!recActive) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int i;
    if (xQueueReceive(recFreeQ, &i, pdMS_TO_TICKS(20)) != pdTRUE) {
      recDropped++;  // 緩衝全滿：主迴圈送得太慢（網路卡住），這段會掉
      continue;
    }
    if (!recActive || !M5.Mic.record(recBuf[i], REC_CHUNK, Recorder::SAMPLE_RATE)) {
      xQueueSend(recFreeQ, &i, 0);
      vTaskDelay(1);
    }
  }
}
}  // namespace

void Recorder::begin() {
  recFreeQ = xQueueCreate(REC_RING, sizeof(int));
  recDoneQ = xQueueCreate(REC_RING, sizeof(int));
  resetRecQueues();
  // StickS3 的喇叭和麥克風共用 codec。省電：平常兩個都關著，說話時才開麥克風、念的時候才開喇叭
  M5.Speaker.end();
  M5.Mic.setBufferReleaseCallback(nullptr, onRecReleased);
  Serial.printf("[boot] 麥克風：%s\n", M5.Mic.begin() ? "OK" : "失敗");
  M5.Mic.end();
  // 跑在 core 1、優先權高於 loop()，畫面重畫時也能準時接上麥克風
  xTaskCreatePinnedToCore(recTask, "rec", 3072, nullptr, 3, nullptr, 1);
}

void Recorder::start() {
  resetRecQueues();
  recDropped = 0;
  level_ = 0;
  startedAt_ = millis();
  if (!M5.Mic.isEnabled()) M5.Mic.begin();
  recording_ = true;
  recActive = true;
}

void Recorder::drain(const ChunkFn& onChunk) {
  int i;
  while (xQueueReceive(recDoneQ, &i, 0) == pdTRUE) {
    const int16_t* data = recBuf[i];
    int peak = 0;
    for (size_t k = 0; k < REC_CHUNK; k++) peak = max(peak, abs((int)data[k]));
    level_ = min(100, peak * 100 / 12000);
    onChunk(data, REC_CHUNK);
    xQueueSend(recFreeQ, &i, 0);  // 送完就還回去
  }
}

void Recorder::loop(const ChunkFn& onChunk) {
  if (recording_) drain(onChunk);
}

void Recorder::stop(const ChunkFn& onChunk) {
  if (!recording_) return;
  recActive = false;
  recording_ = false;
  uint32_t t = millis();
  while (M5.Mic.isRecording() && millis() - t < 300) delay(1);  // 等排著的段落錄完
  delay(3);  // 釋放回呼可能比 isRecording() 歸零晚一點點
  drain(onChunk);
  M5.Mic.end();  // 不錄就關掉，省電
  Serial.printf("[rec] 錄了 %.1f 秒，掉了 %u 段\n", (millis() - startedAt_) / 1000.0f,
                (unsigned)recDropped);
}

uint32_t Recorder::dropped() const { return recDropped; }
