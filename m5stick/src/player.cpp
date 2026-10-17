#include "player.h"

#include "settings.h"

#include <M5Unified.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

Player player;

// 名稱加 play 前綴：Arduino 單檔版會把所有模組合在一起，避免跟 recorder 撞名
namespace {
// 每段 2048 樣本 = 128 ms，共 16 段 ≈ 2 秒緩衝（Pi 最多只會超前約 0.8 秒）
constexpr size_t PLAY_CHUNK = 2048;
constexpr size_t PLAY_POOL = 16;
constexpr uint8_t PLAY_CH = 0;           // 喇叭聲道
int16_t playPool[PLAY_POOL][PLAY_CHUNK];
size_t playLen[PLAY_POOL];
QueueHandle_t playFreeQ = nullptr;      // 可以拿來裝資料的緩衝（喇叭播完放回來）
int playReadyQ[PLAY_POOL];                   // 裝好、等著送給喇叭的緩衝（只在 loop 裡用）
size_t playHead = 0, playCount = 0;
int playCur = -1;                       // 正在裝資料的緩衝
size_t playFill = 0;                 // 已裝的 byte 數
uint8_t playCarry = 0;                  // 跨片段的奇數 byte
bool playHasCarry = false;
SemaphoreHandle_t playLock = nullptr;   // 主迴圈（收資料）和背景工作（餵喇叭）共用狀態

struct PlayGuard {
  PlayGuard() { xSemaphoreTake(playLock, portMAX_DELAY); }
  ~PlayGuard() { xSemaphoreGive(playLock); }
};

int playIndexOf(const void* p) {
  for (size_t i = 0; i < PLAY_POOL; i++) {
    if (p == playPool[i]) return i;
  }
  return -1;
}

// 喇叭播完一段時呼叫（喇叭的工作執行緒）：只把緩衝放回 playFreeQ
void onPlayReleased(void*, const void* data, uint8_t) {
  int i = playIndexOf(data);
  if (i >= 0) xQueueSend(playFreeQ, &i, 0);
}

// 背景工作：每 4 ms 檢查一次，喇叭有空位就補資料。不受畫面重畫、網路卡頓影響。
void playTask(void*) {
  for (;;) {
    player.loop();
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

void resetPlayPool() {
  xQueueReset(playFreeQ);
  for (int i = 0; i < (int)PLAY_POOL; i++) xQueueSend(playFreeQ, &i, 0);
  playHead = playCount = 0;
  playCur = -1;
  playFill = 0;
  playHasCarry = false;
}
}  // namespace

void Player::begin() {
  playFreeQ = xQueueCreate(PLAY_POOL, sizeof(int));
  playLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(playTask, "play", 3072, nullptr, 3, nullptr, 1);
}

void Player::start(int id, uint32_t sampleRate) {
  if (active_) stop();
  PlayGuard g;
  resetPlayPool();
  id_ = id;
  rate_ = sampleRate;
  finished_ = false;
  // 麥克風 → 喇叭
  M5.Mic.end();
  M5.Speaker.setBufferReleaseCallback(nullptr, onPlayReleased);
  M5.Speaker.begin();
  M5.Speaker.setVolume(settings.volume);
  active_ = true;
}

void Player::queueCurrent() {
  if (playCur < 0 || playFill < 2) return;
  playLen[playCur] = playFill / 2;
  playReadyQ[(playHead + playCount) % PLAY_POOL] = playCur;
  playCount++;
  playCur = -1;
  playFill = 0;
}

void Player::feed(const uint8_t* data, size_t len) {
  if (!active_ || finished_) return;
  PlayGuard g;
  uint8_t* dst = nullptr;
  while (len > 0) {
    if (playCur < 0) {
      if (xQueueReceive(playFreeQ, &playCur, 0) != pdTRUE) {
        playCur = -1;
        return;  // 緩衝滿了（不該發生，Pi 有控制速度），丟掉
      }
      playFill = 0;
    }
    dst = reinterpret_cast<uint8_t*>(playPool[playCur]);
    if (playHasCarry) {  // 上一片段剩下的奇數 byte
      dst[playFill++] = playCarry;
      playHasCarry = false;
    }
    size_t room = PLAY_CHUNK * 2 - playFill;
    size_t n = min(room, len);
    memcpy(dst + playFill, data, n);
    playFill += n;
    data += n;
    len -= n;
    if (playFill == PLAY_CHUNK * 2) queueCurrent();
  }
  if (playFill % 2) {  // 樣本不能拆半，奇數 byte 留給下一片段
    playCarry = dst[--playFill];
    playHasCarry = true;
  }
}

void Player::finish(int id) {
  if (!active_ || id != id_) return;
  PlayGuard g;
  queueCurrent();
  finished_ = true;
}

void Player::loop() {
  if (!active_) return;
  PlayGuard g;
  if (!active_) return;
  // 喇叭同時最多排 2 段；有空位就把裝好的送進去
  while (playCount > 0 && M5.Speaker.isPlaying(PLAY_CH) < 2) {
    int i = playReadyQ[playHead];
    if (!M5.Speaker.playRaw(playPool[i], playLen[i], rate_, false, 1, PLAY_CH, false)) break;
    playHead = (playHead + 1) % PLAY_POOL;
    playCount--;
  }
  // 片段太久沒湊滿一段（例如句子之間），先把手上的播掉
  if (playCount == 0 && playCur >= 0 && playFill >= 2 && M5.Speaker.isPlaying(PLAY_CH) == 0) queueCurrent();
  if (finished_ && playCount == 0 && playCur < 0 && M5.Speaker.isPlaying(PLAY_CH) == 0) end();
}

void Player::stop() {
  if (!active_) return;
  PlayGuard g;
  if (!active_) return;
  M5.Speaker.stop();
  end();
}

void Player::end() {
  // 喇叭 → 麥克風
  M5.Speaker.end();
  M5.Mic.begin();
  active_ = false;
  finished_ = false;
  id_ = -1;
}

void Player::setVolume(uint8_t volume) {
  // 正在播就馬上套用；沒在播，下次 start() 會用 settings.volume
  if (active_) M5.Speaker.setVolume(volume);
}
