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

  // small = true 用小字（內容行數多時，例如設定模式的說明）
  void showInfo(const String& title, const String& body, bool small = false);
  void showHome();
  void showRecording(uint32_t elapsedMs, int level);
  // said：使用者說的話（還沒辨識完可以是空字串）；status：例如「辨識中…」「思考中…」
  void showThinking(const String& said, const String& status);
  // 底部提示列（例如「再按一次 B：開新對話」）
  void showHint(const String& hint);
  void showReply(const String& title, const String& text);
  // 回覆一邊產生一邊顯示：永遠捲到最後幾行
  void showReplyLive(const String& title, const String& text);
  // Hermes 要執行需要核准的指令：A 允許一次、B 拒絕
  void showApproval(const String& command, const String& description);

  // 選單：蓋在目前畫面上，關掉後回到原本的畫面（期間其他畫面更新會在背景記著）
  void showMenu(const String& title, const std::vector<String>& items, int cursor);
  void hideMenu();
  bool menuShown() const { return menuActive_; }

  // 回覆畫面捲動；捲到底回傳 false
  bool scrollDown();
  bool isReply() const { return mode_ == Mode::Reply; }
  bool isHome() const { return mode_ == Mode::Home; }

 private:
  enum class Mode { Info, Home, Recording, Thinking, Reply, Approval };

  void render();
  void drawMenu();
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
  bool menuActive_ = false;
  String menuTitle_;
  std::vector<String> menuItems_;
  int menuCursor_ = 0;
};

extern Ui ui;
