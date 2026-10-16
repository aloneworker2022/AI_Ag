#include "ui.h"

#include <M5Unified.h>

Ui ui;

namespace {
M5Canvas canvas(&M5.Display);

constexpr int W = 240;
constexpr int H = 135;
constexpr int BAR_H = 18;
constexpr int PAD = 4;
constexpr int LINE_H = 18;
const lgfx::IFont* FONT = &fonts::efontTW_16;

constexpr uint16_t COL_BG = TFT_BLACK;
constexpr uint16_t COL_BAR = 0x2104;     // 深灰
constexpr uint16_t COL_TEXT = TFT_WHITE;
constexpr uint16_t COL_DIM = 0x8410;     // 灰
constexpr uint16_t COL_ACCENT = 0xFD20;  // 橘
constexpr uint16_t COL_OK = TFT_GREEN;
constexpr uint16_t COL_BAD = TFT_RED;

int contentLines() { return (H - BAR_H - PAD) / LINE_H; }

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
  canvas.setFont(FONT);
  canvas.setTextWrap(false);
}

void Ui::setStatus(const String& wifiName, bool linkUp, bool force) {
  if (!force && wifiName == wifiName_ && linkUp == linkUp_) return;
  wifiName_ = wifiName;
  linkUp_ = linkUp;
  render();
}

void Ui::showInfo(const String& title, const String& body) {
  mode_ = Mode::Info;
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
  canvas.fillRect(0, H - LINE_H - 2, W, LINE_H + 2, COL_ACCENT);
  canvas.setTextColor(TFT_BLACK, COL_ACCENT);
  canvas.drawString(hint.c_str(), PAD, H - LINE_H);
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
  int total = wrap(body_, W - PAD * 2).size();
  int visible = contentLines() - 1;  // 第一行是標題
  if (scroll_ + visible >= total) return false;
  scroll_ += visible - 1;  // 保留一行重疊，比較好接著讀
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
  auto lines = wrap(text, w);
  for (int k = 0; k < maxLines && firstLine + k < (int)lines.size(); k++) {
    canvas.drawString(lines[firstLine + k].c_str(), x, y + k * LINE_H);
  }
}

void Ui::drawStatusBar() {
  canvas.fillRect(0, 0, W, BAR_H, COL_BAR);
  canvas.setTextColor(COL_TEXT, COL_BAR);
  String wifiText = wifiName_.length() ? wifiName_ : String("無網路");
  canvas.drawString(wifiText.c_str(), PAD, 1);

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
  const int top = BAR_H + PAD;
  const int w = W - PAD * 2;
  const int lines = contentLines();

  switch (mode_) {
    case Mode::Info:
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString(title_.c_str(), PAD, top);
      canvas.setTextColor(COL_TEXT, COL_BG);
      drawWrapped(body_, PAD, top + LINE_H, w, 0, lines - 1);
      break;

    case Mode::Home:
      canvas.setTextColor(COL_TEXT, COL_BG);
      canvas.drawString("按住 A 說話", PAD, top + LINE_H);
      canvas.setTextColor(COL_DIM, COL_BG);
      canvas.drawString("放開就送出", PAD, top + LINE_H * 2);
      canvas.drawString("B 兩下：新對話  長按：Wi-Fi", PAD, H - LINE_H);
      break;

    case Mode::Recording: {
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString("聆聽中…", PAD, top);
      canvas.setTextColor(COL_TEXT, COL_BG);
      String sec = String(recMs_ / 1000.0f, 1) + " 秒";
      canvas.drawString(sec.c_str(), PAD, top + LINE_H);
      // 音量條
      int barW = (W - PAD * 2) * level_ / 100;
      canvas.drawRect(PAD, top + LINE_H * 2 + 4, W - PAD * 2, 14, COL_DIM);
      canvas.fillRect(PAD + 1, top + LINE_H * 2 + 5, max(0, barW - 2), 12, COL_OK);
      canvas.setTextColor(COL_DIM, COL_BG);
      canvas.drawString("放開 A 送出", PAD, H - LINE_H);
      break;
    }

    case Mode::Thinking:
      if (body_.length()) {
        canvas.setTextColor(COL_DIM, COL_BG);
        drawWrapped("你：" + body_, PAD, top, w, 0, lines - 1);
      }
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString(status_.c_str(), PAD, H - LINE_H);
      break;

    case Mode::Approval:
      canvas.setTextColor(COL_BAD, COL_BG);
      canvas.drawString("Hermes 要執行：", PAD, top);
      canvas.setTextColor(COL_TEXT, COL_BG);
      drawWrapped(body_.length() ? body_ : title_, PAD, top + LINE_H, w, 0, lines - 2);
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString("A 允許一次  B 拒絕", PAD, H - LINE_H);
      break;

    case Mode::Reply: {
      canvas.setTextColor(COL_ACCENT, COL_BG);
      canvas.drawString(title_.c_str(), PAD, top);
      canvas.setTextColor(COL_TEXT, COL_BG);
      int total = wrap(body_, w).size();
      drawWrapped(body_, PAD, top + LINE_H, w, scroll_, lines - 1);
      if (scroll_ + lines - 1 < total) {
        canvas.fillTriangle(W - 10, H - 8, W - 2, H - 8, W - 6, H - 3, COL_ACCENT);  // 還有下一頁
      }
      break;
    }
  }
  canvas.pushSprite(0, 0);
}
