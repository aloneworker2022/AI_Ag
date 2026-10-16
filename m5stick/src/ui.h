// 螢幕畫面（橫向 240x135）：上方狀態列 + 內容區。
// 用整張畫布重畫，避免閃爍。中文用 M5GFX 內建的 efontTW 字型。
#pragma once

#include <Arduino.h>
#include <vector>

class Ui {
 public:
  void begin();

  // 狀態列：Wi-Fi 名稱、是否連上 Pi
  void setStatus(const String& wifiName, bool linkUp, bool force = false);

  void showInfo(const String& title, const String& body);
  void showHome(const String& preset, int index, int total);
  void showThinking(const String& said);
  void showReply(const String& title, const String& text);

  // 回覆畫面捲動；捲到底回傳 false
  bool scrollDown();
  bool isReply() const { return mode_ == Mode::Reply; }

 private:
  enum class Mode { Info, Home, Thinking, Reply };

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
  int homeIndex_ = 0;
  int homeTotal_ = 0;
};

extern Ui ui;
