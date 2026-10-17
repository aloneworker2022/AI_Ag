#include "ui.h"

#include <M5Unified.h>

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
constexpr uint16_t COL_TEXT = 0x3FE2;    // 螢光綠（RGB 57,255,20）
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
  if (menuActive_) return;
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

void Ui::showReplyLive(const String& title, const String& text) {
  mode_ = Mode::Reply;
  title_ = title;
  body_ = text;
  canvas.setFont(BIG);
  int total = wrap(body_, W - PAD * 2).size();
  int visible = bigLines(TOP, H);
  scroll_ = max(0, total - visible);
  render();
}

void Ui::showMenu(const String& title, const std::vector<String>& items, int cursor) {
  menuActive_ = true;
  menuTitle_ = title;
  menuItems_ = items;
  menuCursor_ = cursor;
  render();
}

void Ui::hideMenu() {
  menuActive_ = false;
  render();
}

void Ui::drawMenu() {
  constexpr int TITLE_H = 20;
  constexpr int ROW_H = 28;
  const int visible = (H - TITLE_H) / ROW_H;  // 一次顯示 4 列
  canvas.fillScreen(COL_BG);
  canvas.setFont(SMALL);
  canvas.fillRect(0, 0, W, TITLE_H, COL_ACCENT);
  canvas.setTextColor(TFT_BLACK, COL_ACCENT);
  canvas.drawString(menuTitle_.c_str(), PAD, 2);
  const char* help = "B 下一個  A 選擇";
  canvas.drawString(help, W - PAD - canvas.textWidth(help), 2);

  int n = menuItems_.size();
  int first = 0;  // 讓游標那一列一定看得到
  if (menuCursor_ >= visible) first = menuCursor_ - visible + 1;
  canvas.setFont(BIG);
  for (int k = 0; k < visible && first + k < n; k++) {
    int i = first + k;
    int y = TITLE_H + k * ROW_H;
    bool sel = i == menuCursor_;
    if (sel) canvas.fillRect(0, y, W, ROW_H, COL_TEXT);
    canvas.setTextColor(sel ? TFT_BLACK : COL_TEXT, sel ? COL_TEXT : COL_BG);
    canvas.drawString(menuItems_[i].c_str(), PAD + 4, y + 1);
  }
  // 上下還有選項時，右邊畫小三角形提示
  if (first > 0) canvas.fillTriangle(W - 12, TITLE_H + 10, W - 2, TITLE_H + 10, W - 7, TITLE_H + 3, COL_DIM);
  if (first + visible < n) canvas.fillTriangle(W - 12, H - 9, W - 2, H - 9, W - 7, H - 2, COL_DIM);
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

  // 電量每 30 秒才讀一次（每次讀都要走 I2C，重畫很頻繁時會拖慢）
  static int level = -1;
  static uint32_t readAt = 0;
  if (readAt == 0 || millis() - readAt > 30000) {
    level = M5.Power.getBatteryLevel();
    readAt = millis();
  }
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
  if (menuActive_) {
    drawMenu();
    canvas.pushSprite(0, 0);
    return;
  }
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
      canvas.drawString("B 兩下：新對話  長按：選單", PAD, FOOT_Y);
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
