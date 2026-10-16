#include "setup_portal.h"

#include <DNSServer.h>
#include <M5Unified.h>
#include <WebServer.h>
#include <WiFi.h>

#include "settings.h"
#include "ui.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

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
  h += F("'>Token（跟 Pi 的 config.yaml device.token 一樣；留空 = 不改）<input name='token' type='password'>"
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
              "手機連 Wi-Fi：\n" + apName + "\n密碼：" SETUP_AP_PASSWORD "\n開 http://192.168.4.1\nA：重新開機");

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
