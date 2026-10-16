// =====================================================================
//  AIStick — M5StickS3 × Hermes Agent（Arduino IDE 單檔版）
//
//  按住 A 說話 → Pi 上的 bridge 用 Whisper 轉文字 → Hermes Agent → 回覆顯示在螢幕
//
//  這個檔案是由 m5stick/tools/make_ino.py 從 m5stick/src/ 自動產生的。
//
//  Arduino IDE 設定（工具選單）：
//    開發板：ESP32S3 Dev Module（開發板管理員安裝 "esp32 by Espressif Systems"）
//    USB CDC On Boot：Enabled
//    Flash Size：8MB (64Mb)
//    Partition Scheme：8M with spiffs (3MB APP/1.5MB SPIFFS)
//    PSRAM：OPI PSRAM
//  需要的程式庫（程式庫管理員）：
//    M5Unified（會一起裝 M5GFX）、WebSockets（by Markus Sattler）、ArduinoJson（v7）
// =====================================================================

// ---------------------------------------------------------------------
//  出廠預設值：可以先改好再燒錄，也可以燒錄後用「設定模式」網頁再填
//  （設定網頁改過的值會存在 Stick 裡，重新燒錄不會被蓋掉）
// ---------------------------------------------------------------------
#define DEFAULT_DEVICE_NAME "stick"

// Pi 的位址。可以是 IP（推薦，在路由器固定 Pi 的 IP）或 mDNS 名稱（xxx.local）
#define DEFAULT_SERVER_HOST "raspberrypi.local"
#define DEFAULT_SERVER_PORT 3799

// 要跟 Pi 上 bridge/config.yaml 的 device.token 一樣
#define DEFAULT_TOKEN "change-me"

// 設定模式時，Stick 自己開的熱點密碼（至少 8 碼）
#define SETUP_AP_PASSWORD "aistick123"

// 預設 Wi-Fi 清單，排越前面越優先。
// 第三欄 server_host 可留空 = 用上面的 DEFAULT_SERVER_HOST。
// 用手機熱點時 Pi 的 IP 通常不一樣（Pi 也要連到同一個熱點），就填在這裡。
#define DEFAULT_NETWORKS                                  \
  {                                                       \
    {"MyHomeWiFi", "home-password", ""},                  \
    {"MyPhoneHotspot", "hotspot-password", "172.20.10.2"}, \
  }

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <M5Unified.h>
#include <functional>
#include <freertos/queue.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <DNSServer.h>
#include <WebServer.h>

// ===== settings.h =====
// 裝置設定：多組 Wi-Fi、Pi 位址、token。存在 NVS（斷電不會消失）。


constexpr int MAX_NETWORKS = 6;

struct NetworkEntry {
  String ssid;
  String password;
  String serverHost;  // 空字串 = 用 Settings::serverHost
};

struct Settings {
  String deviceName;
  String serverHost;
  uint16_t serverPort = 3799;
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

// ===== settings.cpp =====
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

// ===== ui.h =====
// 螢幕畫面（橫向 240x135）：上方狀態列 + 內容區。
// 用整張畫布重畫，避免閃爍。中文用 M5GFX 內建的 efontTW 字型。


class Ui {
 public:
  void begin();

  // 狀態列：Wi-Fi 名稱、是否連上 Pi
  void setStatus(const String& wifiName, bool linkUp, bool force = false);

  // small = true 用小字（內容行數多時，例如設定模式的說明）
  void showInfo(const String& title, const String& body, bool small = false);
  void showHome();
  void showRecording(uint32_t elapsedMs, int level);
  // said：使用者說的話（還沒辨識完可以是空字串）；status：例如「辨識中…」「思考中…」
  void showThinking(const String& said, const String& status);
  // 長按 B 時的提示（放開會做什麼）
  void showHint(const String& hint);
  void showReply(const String& title, const String& text);
  // Hermes 要執行需要核准的指令：A 允許一次、B 拒絕
  void showApproval(const String& command, const String& description);

  // 回覆畫面捲動；捲到底回傳 false
  bool scrollDown();
  bool isReply() const { return mode_ == Mode::Reply; }
  bool isHome() const { return mode_ == Mode::Home; }

 private:
  enum class Mode { Info, Home, Recording, Thinking, Reply, Approval };

  void render();
  void drawStatusBar();
  void drawWrapped(const String& text, int x, int y, int w, int firstLine, int maxLines);
  std::vector<String> wrap(const String& text, int width);

  Mode mode_ = Mode::Info;
  String title_;
  String body_;
  String wifiName_;
  bool linkUp_ = false;
  int scroll_ = 0;
  String status_;
  uint32_t recMs_ = 0;
  int level_ = 0;
  bool infoSmall_ = false;
};

extern Ui ui;

// ===== ui.cpp =====
Ui ui;

namespace {
M5Canvas canvas(&M5.Display);

constexpr int W = 240;
constexpr int H = 135;
constexpr int BAR_H = 18;
constexpr int PAD = 4;
// 大字：回覆、你說的話、主要提示。想再大/小可換 efontTW_16 / efontTW_24 / efontTW_32
const lgfx::IFont* BIG = &fonts::efontTW_24;
constexpr int BIG_H = 26;
// 小字：狀態列、標題、底部操作提示
const lgfx::IFont* SMALL = &fonts::efontTW_16;
constexpr int SMALL_H = 18;
constexpr int TOP = BAR_H + PAD;            // 內容區頂端
constexpr int FOOT_Y = H - SMALL_H;         // 底部提示列

constexpr uint16_t COL_BG = TFT_BLACK;
constexpr uint16_t COL_BAR = 0x2104;     // 深灰
constexpr uint16_t COL_TEXT = TFT_WHITE;
constexpr uint16_t COL_DIM = 0x8410;     // 灰
constexpr uint16_t COL_ACCENT = 0xFD20;  // 橘
constexpr uint16_t COL_OK = TFT_GREEN;
constexpr uint16_t COL_BAD = TFT_RED;

// 從 y 到 bottom 之間放得下幾行大字
int bigLines(int y, int bottom) { return (bottom - y) / BIG_H; }

// UTF-8 一個字元佔幾個 byte
int utf8Len(uint8_t c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}
}  // namespace

void Ui::begin() {
  M5.Display.setRotation(1);
  M5.Display.setBrightness(120);
  canvas.setColorDepth(16);
  canvas.createSprite(W, H);
  canvas.setTextWrap(false);
}

void Ui::setStatus(const String& wifiName, bool linkUp, bool force) {
  if (!force && wifiName == wifiName_ && linkUp == linkUp_) return;
  wifiName_ = wifiName;
  linkUp_ = linkUp;
  render();
}

void Ui::showInfo(const String& title, const String& body, bool small) {
  mode_ = Mode::Info;
  infoSmall_ = small;
  title_ = title;
  body_ = body;
  render();
}

void Ui::showHome() {
  mode_ = Mode::Home;
  render();
}

void Ui::showRecording(uint32_t elapsedMs, int level) {
  mode_ = Mode::Recording;
  recMs_ = elapsedMs;
  level_ = level;
  render();
}

void Ui::showThinking(const String& said, const String& status) {
  mode_ = Mode::Thinking;
  body_ = said;
  status_ = status;
  render();
}

void Ui::showHint(const String& hint) {
  canvas.setFont(SMALL);
  canvas.fillRect(0, FOOT_Y - 2, W, SMALL_H + 2, COL_ACCENT);
  canvas.setTextColor(TFT_BLACK, COL_ACCENT);
  canvas.drawString(hint.c_str(), PAD, FOOT_Y);
  canvas.pushSprite(0, 0);
}

void Ui::showReply(const String& title, const String& text) {
  mode_ = Mode::Reply;
  title_ = title;
  body_ = text;
  scroll_ = 0;
  render();
}

void Ui::showApproval(const String& command, const String& description) {
  mode_ = Mode::Approval;
  title_ = description;
  body_ = command;
  render();
}

bool Ui::scrollDown() {
  if (mode_ != Mode::Reply) return false;
  canvas.setFont(BIG);
  int total = wrap(body_, W - PAD * 2).size();
  int visible = bigLines(TOP, H);
  if (scroll_ + visible >= total) return false;
  scroll_ += max(1, visible - 1);  // 保留一行重疊，比較好接著讀
  render();
  return true;
}

std::vector<String> Ui::wrap(const String& text, int width) {
  std::vector<String> lines;
  String line;
  size_t i = 0;
  while (i < text.length()) {
    uint8_t c = text[i];
    if (c == '\n') {
      lines.push_back(line);
      line = "";
      i++;
      continue;
    }
    int n = utf8Len(c);
    String ch = text.substring(i, i + n);
    if (canvas.textWidth((line + ch).c_str()) > width && line.length()) {
      lines.push_back(line);
      line = "";
      if (ch == " ") {  // 換行處的空白不要留在行首
        i += n;
        continue;
      }
    }
    line += ch;
    i += n;
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

void Ui::drawWrapped(const String& text, int x, int y, int w, int firstLine, int maxLines) {
  // 用目前設定的字型；行高依字型決定
  int lineH = canvas.getFont() == BIG ? BIG_H : SMALL_H;
  auto lines = wrap(text, w);
  for (int k = 0; k < maxLines && firstLine + k < (int)lines.size(); k++) {
    canvas.drawString(lines[firstLine + k].c_str(), x, y + k * lineH);
  }
}

void Ui::drawStatusBar() {
  canvas.setFont(SMALL);
  canvas.fillRect(0, 0, W, BAR_H, COL_BAR);
  // 回覆畫面左上角顯示標題（Hermes / 錯誤 / 通知），其他畫面顯示 Wi-Fi
  bool showTitle = mode_ == Mode::Reply && title_.length();
  canvas.setTextColor(showTitle ? COL_ACCENT : COL_TEXT, COL_BAR);
  String left = showTitle ? title_ : (wifiName_.length() ? wifiName_ : String("無網路"));
  canvas.drawString(left.c_str(), PAD, 1);

  int level = M5.Power.getBatteryLevel();
  int right = W - PAD;
  if (level >= 0) {
    String bat = String(level) + "%";
    right -= canvas.textWidth(bat.c_str());
    canvas.drawString(bat.c_str(), right, 1);
    right -= 6;
  }
  canvas.fillCircle(right - 5, BAR_H / 2, 5, linkUp_ ? COL_OK : COL_BAD);
}

void Ui::render() {
  canvas.fillScreen(COL_BG);
  drawStatusBar();
  const int w = W - PAD * 2;

  switch (mode_) {
    case Mode::Info:
      canvas.setFont(SMALL);
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString(title_.c_str(), PAD, TOP);
      canvas.setFont(infoSmall_ ? SMALL : BIG);
      canvas.setTextColor(COL_TEXT, COL_BG);
      drawWrapped(body_, PAD, TOP + SMALL_H, w, 0,
                  infoSmall_ ? (H - TOP - SMALL_H) / SMALL_H : bigLines(TOP + SMALL_H, H));
      break;

    case Mode::Home:
      canvas.setFont(BIG);
      canvas.setTextColor(COL_TEXT, COL_BG);
      canvas.drawString("按住 A 說話", PAD, TOP + 8);
      canvas.setTextColor(COL_DIM, COL_BG);
      canvas.drawString("放開就送出", PAD, TOP + 8 + BIG_H);
      canvas.setFont(SMALL);
      canvas.drawString("B 兩下：新對話  長按：Wi-Fi", PAD, FOOT_Y);
      break;

    case Mode::Recording: {
      canvas.setFont(BIG);
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString("聆聽中…", PAD, TOP);
      canvas.setTextColor(COL_TEXT, COL_BG);
      String sec = String(recMs_ / 1000.0f, 1) + " 秒";
      canvas.drawString(sec.c_str(), PAD, TOP + BIG_H);
      // 音量條
      int barY = TOP + BIG_H * 2 + 2;
      int barW = w * level_ / 100;
      canvas.drawRect(PAD, barY, w, 12, COL_DIM);
      canvas.fillRect(PAD + 1, barY + 1, max(0, barW - 2), 10, COL_OK);
      canvas.setFont(SMALL);
      canvas.setTextColor(COL_DIM, COL_BG);
      canvas.drawString("放開 A 送出", PAD, FOOT_Y);
      break;
    }

    case Mode::Thinking:
      canvas.setFont(BIG);
      if (body_.length()) {
        canvas.setTextColor(COL_DIM, COL_BG);
        drawWrapped("你：" + body_, PAD, TOP, w, 0, bigLines(TOP, FOOT_Y));
      }
      canvas.setFont(SMALL);
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString(status_.c_str(), PAD, FOOT_Y);
      break;

    case Mode::Approval:
      // 指令通常很長，用小字才放得下
      canvas.setFont(SMALL);
      canvas.setTextColor(COL_BAD, COL_BG);
      canvas.drawString("Hermes 要執行：", PAD, TOP);
      canvas.setTextColor(COL_TEXT, COL_BG);
      drawWrapped(body_.length() ? body_ : title_, PAD, TOP + SMALL_H, w, 0,
                  (FOOT_Y - TOP - SMALL_H) / SMALL_H);
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString("A 允許一次  B 拒絕", PAD, FOOT_Y);
      break;

    case Mode::Reply: {
      canvas.setFont(BIG);
      canvas.setTextColor(COL_TEXT, COL_BG);
      int visible = bigLines(TOP, H);
      int total = wrap(body_, w).size();
      drawWrapped(body_, PAD, TOP, w, scroll_, visible);
      if (scroll_ + visible < total) {
        canvas.fillTriangle(W - 12, H - 9, W - 2, H - 9, W - 7, H - 2, COL_ACCENT);  // 還有下一頁（按 B）
      }
      break;
    }
  }
  canvas.pushSprite(0, 0);
}

// ===== recorder.h =====
// 麥克風錄音：16 kHz、16-bit 單聲道，一邊錄一邊把小段 PCM 交給呼叫端（傳給 Pi）。


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

// ===== recorder.cpp =====
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

// ===== wifi_manager.h =====
// 多組 Wi-Fi：依清單順序找第一個「附近有、連得上」的網路。
// 斷線會自動重找；也可以手動切到清單裡的下一組（例如從家裡 Wi-Fi 切到手機熱點）。


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

// ===== wifi_manager.cpp =====
WifiManager wifi;

namespace {
constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;
constexpr uint32_t RETRY_INTERVAL_MS = 30000;
constexpr uint32_t LOST_GRACE_MS = 5000;  // 斷線超過這麼久才重找網路
}  // namespace

void WifiManager::begin(StatusFn onStatus) {
  onStatus_ = onStatus;
  WiFi.persistent(false);       // 密碼由我們自己存在 NVS，不讓 Wi-Fi 驅動另存一份
  WiFi.setAutoReconnect(false);  // 斷線由 loop() 決定要重連哪一組
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(settings.deviceName.c_str());
  connectBest(-1);
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

String WifiManager::ssid() const { return connected() ? WiFi.SSID() : String(); }

int WifiManager::rssi() const { return connected() ? WiFi.RSSI() : 0; }

bool WifiManager::takeChanged() {
  bool c = changed_;
  changed_ = false;
  return c;
}

void WifiManager::switchNext() {
  if (settings.networkCount == 0) {
    onStatus_("沒有存 Wi-Fi\nB 按 5 秒：設定");
    return;
  }
  connectBest(current_);
}

void WifiManager::loop() {
  uint32_t now = millis();
  if (connected()) {
    lostAt_ = 0;
    return;
  }
  if (current_ >= 0) {
    // 原本連著，剛斷：給它幾秒緩衝再重找
    if (lostAt_ == 0) {
      lostAt_ = now;
      changed_ = true;  // 讓主程式知道 Pi 連線也斷了
      return;
    }
    if (now - lostAt_ < LOST_GRACE_MS) return;
    current_ = -1;
    nextRetryAt_ = 0;
  }
  if (now >= nextRetryAt_) connectBest(-1);
}

bool WifiManager::tryConnect(int index) {
  const NetworkEntry& net = settings.networks[index];
  onStatus_("連線中\n" + net.ssid);
  WiFi.disconnect();
  WiFi.begin(net.ssid.c_str(), net.password.c_str());
  uint32_t start = millis();
  while (millis() - start < CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  WiFi.disconnect();
  return false;
}

bool WifiManager::connectBest(int after) {
  const int count = settings.networkCount;
  if (count == 0) {
    onStatus_("沒有存 Wi-Fi\nB 按 5 秒：設定");
    nextRetryAt_ = millis() + RETRY_INTERVAL_MS;
    return false;
  }

  onStatus_("掃描 Wi-Fi…");
  WiFi.disconnect();
  int found = WiFi.scanNetworks();

  // 嘗試順序：一般從清單第 0 組開始；手動切換時從「目前這組的下一組」開始，目前這組放最後
  int order[MAX_NETWORKS];
  for (int k = 0; k < count; k++) order[k] = (after + 1 + k) % count;

  bool ok = false;
  for (int k = 0; k < count && !ok; k++) {
    int idx = order[k];
    bool visible = false;
    for (int s = 0; s < found; s++) {
      if (WiFi.SSID(s) == settings.networks[idx].ssid) {
        visible = true;
        break;
      }
    }
    if (!visible) continue;
    if (tryConnect(idx)) {
      current_ = idx;
      ok = true;
    }
  }
  WiFi.scanDelete();

  if (ok) {
    lostAt_ = 0;
    changed_ = true;
    onStatus_("已連上\n" + settings.networks[current_].ssid);
    return true;
  }
  current_ = -1;
  nextRetryAt_ = millis() + RETRY_INTERVAL_MS;
  onStatus_("找不到 Wi-Fi\n30 秒後重試\nB 按 5 秒：設定");
  return false;
}

// ===== link.h =====
// 跟 Pi 的 WebSocket 長連線（ws://<host>:<port>/ws/device）。
// 連上就送 hello（含 token），斷線自動重連。訊息格式見 m5stick/README.md。


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

// ===== link.cpp =====
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

// ===== setup_portal.h =====
// 設定模式：Stick 自己開一個 Wi-Fi 熱點，手機連上後打開 http://192.168.4.1
// 就能新增 / 刪除 / 排序 Wi-Fi，以及改 Pi 位址、token、裝置名稱。
// 進入後不會返回；按 A 重新開機套用設定。

[[noreturn]] void runSetupPortal();

// ===== setup_portal.cpp =====
namespace {
WebServer server(80);
DNSServer dns;
String apName;

String esc(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

void redirectHome(const String& note = "") {
  server.sendHeader("Location", note.isEmpty() ? "/" : "/?m=" + note);
  server.send(303);
}

void handleRoot() {
  String note = server.arg("m");
  String h;
  h.reserve(4096);
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>AI Stick 設定</title><style>"
         "body{font-family:sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#f6f6f6}"
         "h2{margin-top:28px}section{background:#fff;border-radius:10px;padding:12px 14px;margin:10px 0}"
         "input{width:100%;box-sizing:border-box;padding:8px;margin:4px 0 10px;font-size:16px}"
         "button{padding:8px 14px;font-size:15px;margin:2px}"
         ".net{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #eee;padding:6px 0}"
         ".muted{color:#777;font-size:13px}.note{background:#e8f5e9;padding:8px;border-radius:6px}"
         "</style></head><body><h1>AI Stick 設定</h1>");
  if (note == "saved") h += F("<p class='note'>已儲存。全部改完後按最下面的「重新開機」。</p>");
  if (note == "full") h += F("<p class='note'>Wi-Fi 清單已滿，請先刪除一組。</p>");

  h += F("<h2>Wi-Fi 清單</h2><p class='muted'>開機時從第 1 組開始找，附近有、連得上就用。"
         "平常在 Stick 上長按 B 可以切到下一組。</p><section>");
  if (settings.networkCount == 0) h += F("<p>（還沒有）</p>");
  for (int i = 0; i < settings.networkCount; i++) {
    const NetworkEntry& n = settings.networks[i];
    h += "<div class='net'><div><b>" + String(i + 1) + ". " + esc(n.ssid) + "</b><br><span class='muted'>Pi："
         + esc(n.serverHost.length() ? n.serverHost : settings.serverHost + "（預設）") + "</span></div><div>";
    if (i > 0) h += "<form method='post' action='/up' style='display:inline'><input type='hidden' name='i' value='"
                    + String(i) + "'><button>↑</button></form>";
    h += "<form method='post' action='/del' style='display:inline' onsubmit=\"return confirm('刪除 "
         + esc(n.ssid) + "？')\"><input type='hidden' name='i' value='" + String(i)
         + "'><button>刪除</button></form></div></div>";
  }
  h += F("</section>");

  h += F("<h2>新增 / 更新 Wi-Fi</h2><section><form method='post' action='/add'>"
         "SSID（名稱）<input name='ssid' list='scan' required maxlength='32'>"
         "<datalist id='scan'>");
  int found = WiFi.scanComplete();
  if (found > 0) {
    for (int s = 0; s < found; s++) h += "<option value='" + esc(WiFi.SSID(s)) + "'>";
  }
  h += F("</datalist>密碼<input name='pass' type='password' maxlength='64'>"
         "這組網路下 Pi 的位址（可留空 = 用預設）<input name='host' placeholder='例如手機熱點下 Pi 的 IP'>"
         "<button>儲存</button></form>"
         "<p class='muted'>同名的 SSID 會直接更新密碼。用手機熱點時，Pi 也要連到同一個熱點，"
         "再把 Pi 在熱點裡的 IP 填上。</p>"
         "<form method='post' action='/scan'><button>重新掃描附近 Wi-Fi</button></form></section>");

  h += F("<h2>Pi 連線</h2><section><form method='post' action='/server'>預設 Pi 位址（IP 或 xxx.local）<input name='host' value='");
  h += esc(settings.serverHost);
  h += F("' required>Port<input name='port' type='number' min='1' max='65535' value='");
  h += String(settings.serverPort);
  h += F("'>Token（跟 Pi 的 bridge/config.yaml device.token 一樣；留空 = 不改）<input name='token' type='password'>"
         "裝置名稱<input name='device' maxlength='32' value='");
  h += esc(settings.deviceName);
  h += F("'><button>儲存</button></form></section>");

  h += F("<h2>完成</h2><section><form method='post' action='/reboot'><button>重新開機套用設定</button></form>"
         "<form method='post' action='/reset' onsubmit=\"return confirm('清除所有設定，回到出廠預設？')\">"
         "<button>恢復出廠預設</button></form></section></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

void handleAdd() {
  if (!settings.addNetwork(server.arg("ssid"), server.arg("pass"), server.arg("host"))) {
    redirectHome("full");
    return;
  }
  settingsSave();
  redirectHome("saved");
}

void handleDel() {
  settings.removeNetwork(server.arg("i").toInt());
  settingsSave();
  redirectHome("saved");
}

void handleUp() {
  settings.moveUp(server.arg("i").toInt());
  settingsSave();
  redirectHome("saved");
}

void handleServer() {
  String host = server.arg("host");
  host.trim();
  if (host.length()) settings.serverHost = host;
  long port = server.arg("port").toInt();
  if (port > 0 && port < 65536) settings.serverPort = port;
  if (server.arg("token").length()) settings.token = server.arg("token");
  String dev = server.arg("device");
  dev.trim();
  if (dev.length()) settings.deviceName = dev;
  settingsSave();
  redirectHome("saved");
}

void handleScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // 非同步，掃完下次打開頁面就有
  redirectHome();
}

void handleReboot() {
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'><p>重新開機中…可以把手機切回原本的 Wi-Fi 了。</p>");
  delay(500);
  ESP.restart();
}

void handleReset() {
  settingsFactoryReset();
  redirectHome("saved");
}
}  // namespace

void runSetupPortal() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apName = String("AIStick-") + suffix;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // 保留 STA 才能掃描附近的 Wi-Fi
  WiFi.softAP(apName.c_str(), SETUP_AP_PASSWORD);
  WiFi.scanNetworks(true);
  dns.start(53, "*", WiFi.softAPIP());  // 手機連上後自動跳出設定頁

  server.on("/", HTTP_GET, handleRoot);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/del", HTTP_POST, handleDel);
  server.on("/up", HTTP_POST, handleUp);
  server.on("/server", HTTP_POST, handleServer);
  server.on("/scan", HTTP_POST, handleScan);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound([] { redirectHome(); });
  server.begin();

  ui.showInfo("設定模式",
              "手機連 Wi-Fi：\n" + apName + "\n密碼：" SETUP_AP_PASSWORD "\n開 http://192.168.4.1\nA：重新開機",
              true);

  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    M5.update();
    if (M5.BtnA.wasClicked()) {
      ui.showInfo("設定模式", "重新開機…");
      delay(300);
      ESP.restart();
    }
    delay(2);
  }
}

// ===== main.cpp =====
// Hermes Agent 隨身終端（M5StickS3）：用說的，Hermes 回覆文字
//
// Stick ──WebSocket──▶ Pi 上的 bridge（Whisper 語音辨識）──▶ Hermes Agent
//
// 按鍵：
//   A 按住：說話（放開就送出給 Pi 辨識）
//   B 單按：回覆畫面往下捲，捲到底回首頁
//   B 在首頁連按兩下：開新對話（Hermes 換新 session）
//   Hermes 要求核准指令時：A 允許一次、B 拒絕
//   B 長按 1.5 秒放開：切換到清單裡的下一組 Wi-Fi（家裡 ⇄ 手機熱點）
//   B 長按 5 秒放開：進入設定模式（用手機設定 Wi-Fi、Pi 位址、token）
// 序列埠：在 serial monitor 打一行字按 Enter，會當成文字訊息送給 Pi（測試用）。


namespace {
constexpr uint32_t IDLE_TIMEOUT_MS = 300000;  // 超過這麼久沒收到任何進度才算逾時（agent 可能跑很久）
constexpr uint32_t DOUBLE_CLICK_MS = 2500;
constexpr uint32_t MIN_SPEECH_MS = 400;        // 按太短當作誤觸
constexpr uint32_t MAX_SPEECH_MS = 30000;      // 跟 Pi 的 stt.max_seconds 一致
constexpr uint32_t HOLD_SWITCH_MS = 1500;
constexpr uint32_t HOLD_SETUP_MS = 5000;

int waitingId = -1;  // 等 Pi 回覆中的訊息 id
uint32_t waitingSince = 0;
int audioId = -1;    // 錄音串流中的訊息 id
String said;         // 辨識出來的話
String agentName = "Hermes";
bool approvalPending = false;
uint32_t lastHomeClickAt = 0;
String serialLine;
uint32_t bPressedAt = 0;
int bHintShown = 0;

void startWaiting(int id, const String& text, const String& status) {
  waitingId = id;
  waitingSince = millis();
  said = text;
  ui.showThinking(text, status);
}

void showNotConnected() {
  ui.showReply("沒連上 Pi", "Pi：" + piLink.host() + ":" + String(settings.serverPort) +
                                "\n確認 Pi 有開、位址和 token 對不對");
}

void sendText(const String& text) {
  if (waitingId >= 0 || recorder.recording()) return;
  int id = piLink.sendChat(text);
  if (id < 0) {
    showNotConnected();
    return;
  }
  startWaiting(id, text, "思考中…");
}

// ---- 錄音 ----
void streamChunk(const int16_t* data, size_t n) {
  if (audioId >= 0) piLink.sendAudio(data, n);
}

void startSpeaking() {
  if (waitingId >= 0) return;  // 上一句還沒回
  if (!piLink.ready()) {
    showNotConnected();
    return;
  }
  audioId = piLink.audioStart(Recorder::SAMPLE_RATE);
  if (audioId < 0) {
    showNotConnected();
    return;
  }
  recorder.start();
  ui.showRecording(0, 0);
}

void stopSpeaking() {
  if (!recorder.recording()) return;
  uint32_t ms = recorder.elapsedMs();
  recorder.stop(streamChunk);
  int id = audioId;
  audioId = -1;
  if (ms < MIN_SPEECH_MS) {
    piLink.audioCancel(id);
    ui.showHome();
    return;
  }
  piLink.audioEnd(id);
  startWaiting(id, "", "辨識中…");
}

// ---- Pi 傳來的訊息 ----
void onPiMessage(JsonDocument& doc) {
  String type = doc["type"] | "";
  int id = doc["id"] | -1;

  if (id >= 0 && id == waitingId) waitingSince = millis();  // 有進度就重新計時

  if (type == "welcome") {
    agentName = doc["agent"] | "Hermes";
    Serial.printf("[link] welcome，agent：%s\n", agentName.c_str());
    if (waitingId < 0 && !recorder.recording() && !ui.isReply()) ui.showHome();
  } else if (type == "transcript") {
    if (id != waitingId) return;
    said = doc["text"] | "";
    Serial.printf("你：%s\n", said.c_str());
    ui.showThinking(said, "思考中…");
  } else if (type == "thinking") {
    if (id == waitingId && said.isEmpty()) ui.showThinking("", "思考中…");
  } else if (type == "tool" || type == "status") {
    if (id != waitingId || approvalPending) return;
    String what = type == "tool" ? "使用工具：" + String(doc["label"] | "") : String(doc["text"] | "");
    Serial.println("[agent] " + what);
    ui.showThinking(said, what);
  } else if (type == "approval") {
    if (id != waitingId) return;
    approvalPending = true;
    String cmd = doc["command"] | "";
    Serial.printf("[approval] %s（A 允許 / B 拒絕）\n", cmd.c_str());
    ui.showApproval(cmd, doc["description"] | "");
  } else if (type == "reply") {
    if (id != waitingId) return;  // 過期的回覆（例如逾時後才到）
    waitingId = -1;
    approvalPending = false;
    String text = doc["text"] | "";
    Serial.printf("%s：%s\n", agentName.c_str(), text.c_str());
    ui.showReply(agentName, text);
  } else if (type == "error") {
    if (id >= 0 && id != waitingId) return;
    waitingId = -1;
    approvalPending = false;
    String msg = doc["message"] | "未知錯誤";
    Serial.printf("[error] %s\n", msg.c_str());
    ui.showReply("錯誤", msg);
  } else if (type == "notify") {
    if (recorder.recording()) return;  // 說話中不打斷
    String title = doc["title"] | "";
    ui.showReply(title.length() ? title : agentName, doc["text"] | "");
  }
  // transcribing / pong：不用處理（畫面已經是「辨識中…」）
}

void onWifiStatus(const String& msg) {
  Serial.println("[wifi] " + msg);
  ui.showInfo("Wi-Fi", msg);
}

void sendSimple(const char* type, const char* choice = nullptr) {
  JsonDocument doc;
  doc["type"] = type;
  if (waitingId >= 0) doc["id"] = waitingId;
  if (choice) doc["choice"] = choice;
  piLink.send(doc);
}

// ---- 按鍵 ----
void handleButtons() {
  // 核准畫面：A 允許一次、B 拒絕
  if (approvalPending) {
    bool allow = M5.BtnA.wasPressed();
    bool deny = M5.BtnB.wasPressed();
    if (allow || deny) {
      sendSimple("approval_reply", allow ? "once" : "deny");
      approvalPending = false;
      ui.showThinking(said, allow ? "已允許，繼續中…" : "已拒絕，繼續中…");
      bPressedAt = millis();
      bHintShown = 3;  // 這次 B 放開不要再觸發別的動作
    }
    return;
  }

  // A：按住說話
  if (M5.BtnA.wasPressed()) startSpeaking();
  if (M5.BtnA.wasReleased()) stopSpeaking();

  // B：依按住時間決定動作，按住時提示放開會做什麼
  if (M5.BtnB.wasPressed()) {
    bPressedAt = millis();
    bHintShown = 0;
  }
  if (M5.BtnB.isPressed() && !recorder.recording()) {
    uint32_t held = millis() - bPressedAt;
    if (held >= HOLD_SETUP_MS && bHintShown < 2) {
      ui.showHint("放開：進入設定模式");
      bHintShown = 2;
    } else if (held >= HOLD_SWITCH_MS && held < HOLD_SETUP_MS && bHintShown < 1) {
      ui.showHint("放開：換下一組 Wi-Fi");
      bHintShown = 1;
    }
  }
  if (M5.BtnB.wasReleased() && !recorder.recording()) {
    uint32_t held = millis() - bPressedAt;
    if (bHintShown == 3) {  // 這次按 B 是用來回覆核准
      bHintShown = 0;
      return;
    }
    if (held >= HOLD_SETUP_MS) {
      runSetupPortal();  // 不會返回
    } else if (held >= HOLD_SWITCH_MS) {
      piLink.stop();
      waitingId = -1;
      wifi.switchNext();
    } else if (ui.isReply()) {
      if (!ui.scrollDown()) ui.showHome();
    } else if (waitingId < 0 && ui.isHome()) {
      if (millis() - lastHomeClickAt < DOUBLE_CLICK_MS) {
        lastHomeClickAt = 0;
        if (piLink.ready()) {
          sendSimple("new_session");
        } else {
          showNotConnected();
        }
      } else {
        lastHomeClickAt = millis();
        ui.showHint("再按一次 B：開新對話");
      }
    } else if (waitingId < 0) {
      ui.showHome();
    }
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialLine.trim();
      if (serialLine.length()) sendText(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 500) {
      serialLine += c;
    }
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  ui.begin();
  Serial.printf("[boot] board=%d PSRAM=%s (%u bytes)\n", (int)M5.getBoard(),
                psramFound() ? "有" : "沒有", ESP.getPsramSize());

  recorder.begin();
  Serial.printf("[boot] 麥克風：%s\n", M5.Mic.isEnabled() ? "OK" : "失敗");

  settingsLoad();
  if (settings.networkCount == 0) runSetupPortal();  // 第一次用：直接進設定

  piLink.onMessage(onPiMessage);
  wifi.begin(onWifiStatus);
}

void loop() {
  M5.update();
  if (!recorder.recording()) wifi.loop();  // 說話中不做可能卡住的 Wi-Fi 重找

  if (wifi.takeChanged()) {
    if (wifi.connected()) {
      int idx = wifi.currentIndex();
      piLink.start(settings.hostFor(idx), settings.serverPort);
      ui.showInfo("Wi-Fi", "已連上 " + wifi.ssid() + "\n連線到 Pi…\n" + settings.hostFor(idx));
    } else {
      piLink.stop();
    }
  }
  piLink.loop();

  if (recorder.recording()) {
    recorder.loop(streamChunk);
    if (!piLink.ready()) {  // 錄到一半斷線
      recorder.stop([](const int16_t*, size_t) {});
      audioId = -1;
      ui.showReply("錯誤", "跟 Pi 的連線斷了");
    } else if (recorder.elapsedMs() >= MAX_SPEECH_MS) {
      stopSpeaking();  // 太長自動送出
    } else {
      static uint32_t lastDraw = 0;
      if (millis() - lastDraw > 100) {
        lastDraw = millis();
        ui.showRecording(recorder.elapsedMs(), recorder.level());
      }
    }
  }

  if (waitingId >= 0 && (!piLink.ready() || millis() - waitingSince > IDLE_TIMEOUT_MS)) {
    waitingId = -1;
    approvalPending = false;
    ui.showReply("錯誤", piLink.ready() ? "等太久沒有回覆" : "跟 Pi 的連線斷了");
  }

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 500) {
    lastStatus = millis();
    // 每分鐘強制重畫一次，讓電量跟著更新
    static uint32_t lastForce = 0;
    bool force = millis() - lastForce > 60000;
    if (force) lastForce = millis();
    ui.setStatus(wifi.ssid(), piLink.ready(), force);
  }

  handleButtons();
  handleSerial();
  if (!recorder.recording()) delay(5);
}
