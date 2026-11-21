// CYD Wi-Fi Mapper — firmware entry point for the ESP32-2432S028R ("Cheap Yellow Display").
// Hardware glue only: display, touch, SD card, Wi-Fi scanning and the setup hotspot.
// All behaviour lives in lib/core (portable, host-tested).
#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>
#include <algorithm>

#include "web_assets.h"
#include "wm_app.h"

#define FW_VERSION "0.1.0"

// ---- CYD pin map (the display pins are set in platformio.ini) ----
static const int PIN_T_CLK = 25, PIN_T_CS = 33, PIN_T_DIN = 32, PIN_T_DOUT = 39;  // XPT2046 touch
static const int PIN_SD_CS = 5;                                                   // SD on VSPI 18/19/23
static const char* AP_SSID = "CYD-WiFiMap";
static const char* AP_PASS = "mapmywifi";

static TFT_eSPI tft;
static TFT_eSprite spr(&tft);
static Preferences prefs;
static WebServer server(80);
static DNSServer dns;
static bool sdOk = false, hotspotOn = false;
static int bandH = 60;

// ============================================================ touch (bit-banged XPT2046)
// The touch controller sits on its own pins. Bit-banging it leaves both hardware SPI
// buses free for the display (HSPI) and the SD card (VSPI).
static uint16_t tRead(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_T_DIN, (cmd >> i) & 1);
    digitalWrite(PIN_T_CLK, HIGH);
    digitalWrite(PIN_T_CLK, LOW);
  }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(PIN_T_CLK, HIGH);
    v = (uint16_t)((v << 1) | digitalRead(PIN_T_DOUT));
    digitalWrite(PIN_T_CLK, LOW);
  }
  return (v >> 3) & 0x0FFF;
}

static uint16_t mid3(uint16_t a, uint16_t b, uint16_t c) { return max(min(a, b), min(max(a, b), c)); }

// Raw read. Returns false when the panel is not pressed.
static bool touchRaw(uint16_t& u, uint16_t& v) {
  digitalWrite(PIN_T_CS, LOW);
  int z = (int)tRead(0xB1) + 4095 - (int)tRead(0xC1);
  bool hit = z > 500;
  if (hit) {
    uint16_t a[3], b[3];
    for (int i = 0; i < 3; i++) {
      a[i] = tRead(0x91);
      b[i] = tRead(0xD1);
    }
    u = mid3(a[0], a[1], a[2]);
    v = mid3(b[0], b[1], b[2]);
    hit = ((int)tRead(0xB1) + 4095 - (int)tRead(0xC1)) > 500;  // still pressed after sampling?
  }
  tRead(0x90);  // power down between reads
  digitalWrite(PIN_T_CS, HIGH);
  return hit;
}

struct TouchCal {
  bool swap = false;
  float ax = 320.0f / 3500, bx = -200 * 320.0f / 3500, ay = 240.0f / 3560, by = -240 * 240.0f / 3560;
} cal;

static bool touchRead(int& x, int& y) {
  uint16_t u, v;
  if (!touchRaw(u, v)) return false;
  float X = cal.swap ? v : u, Y = cal.swap ? u : v;
  x = constrain((int)(cal.ax * X + cal.bx), 0, 319);
  y = constrain((int)(cal.ay * Y + cal.by), 0, 239);
  return true;
}

static void calLoad() {
  if (prefs.getBytesLength("cal") == sizeof(cal)) prefs.getBytes("cal", &cal, sizeof(cal));
}

static bool calPoint(int x, int y, uint16_t& u, uint16_t& v) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN);
  tft.drawString("Touch calibration: tap the cross", 40, 110, 2);
  tft.drawLine(x - 10, y, x + 10, y, TFT_WHITE);
  tft.drawLine(x, y - 10, x, y + 10, TFT_WHITE);
  uint32_t t0 = millis();
  while (touchRaw(u, v)) delay(20);  // wait for release
  delay(150);
  uint32_t su = 0, sv = 0;
  int n = 0;
  while (n < 12) {
    if (millis() - t0 > 30000) return false;
    uint16_t a, b;
    if (touchRaw(a, b)) {
      su += a;
      sv += b;
      n++;
    } else {
      su = sv = 0;
      n = 0;
    }
    delay(15);
  }
  u = (uint16_t)(su / n);
  v = (uint16_t)(sv / n);
  return true;
}

// Three taps (top-left, top-right, bottom-right) fix scale, direction and axis order.
static void calRun() {
  uint16_t u[3], v[3];
  if (!calPoint(20, 20, u[0], v[0]) || !calPoint(300, 20, u[1], v[1]) || !calPoint(300, 220, u[2], v[2])) return;
  TouchCal c;
  c.swap = abs((int)u[1] - (int)u[0]) < abs((int)v[1] - (int)v[0]);
  float x0 = c.swap ? v[0] : u[0], x1 = c.swap ? v[1] : u[1];
  float y1 = c.swap ? u[1] : v[1], y2 = c.swap ? u[2] : v[2];
  if (fabsf(x1 - x0) < 400 || fabsf(y2 - y1) < 400) return;  // implausible: keep the old values
  c.ax = 280.0f / (x1 - x0);
  c.bx = 20 - c.ax * x0;
  c.ay = 200.0f / (y2 - y1);
  c.by = 20 - c.ay * y1;
  cal = c;
  prefs.putBytes("cal", &cal, sizeof(cal));
  uint16_t a, b;
  while (touchRaw(a, b)) delay(20);
}

// ============================================================ storage + Wi-Fi platform
static bool safePath(const String& p) { return p.startsWith("/wifimap/") && p.indexOf("..") < 0 && p.length() < 120; }

static void mkParents(const String& path) {
  for (int i = 1; i < (int)path.length(); i++)
    if (path[i] == '/') {
      String d = path.substring(0, i);
      if (!SD.exists(d)) SD.mkdir(d);
    }
}

struct CydPlatform : wm::Platform {
  uint32_t millis() override { return ::millis(); }
  uint32_t epoch() override {
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
  }
  bool storageOk() override { return sdOk; }

  std::vector<std::string> listProjects() override {
    std::vector<std::string> out;
    if (!sdOk) return out;
    File root = SD.open("/wifimap");
    if (!root) return out;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) {
        String n = f.name();
        int s = n.lastIndexOf('/');
        if (s >= 0) n = n.substring(s + 1);
        if (SD.exists("/wifimap/" + n + "/project.json")) out.push_back(n.c_str());
      }
      f.close();
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  bool readFile(const std::string& p, std::string& out) override {
    File f = SD.open(p.c_str(), FILE_READ);
    if (!f) return false;
    out.clear();
    out.reserve(f.size());
    uint8_t buf[256];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) out.append((const char*)buf, n);
    f.close();
    return true;
  }

  bool writeFile(const std::string& p, const std::string& d) override {
    if (!sdOk) return false;
    mkParents(p.c_str());
    File f = SD.open(p.c_str(), FILE_WRITE);
    if (!f) return false;
    size_t n = f.write((const uint8_t*)d.data(), d.size());
    f.close();
    return n == d.size();
  }

  bool appendLine(const std::string& p, const std::string& l) override {
    if (!sdOk) return false;
    mkParents(p.c_str());
    File f = SD.open(p.c_str(), FILE_APPEND);
    if (!f) return false;
    size_t n = f.write((const uint8_t*)l.data(), l.size());
    n += f.write('\n');
    f.close();
    return n == l.size() + 1;
  }

  bool readLines(const std::string& p, const std::function<void(const std::string&)>& fn) override {
    File f = SD.open(p.c_str(), FILE_READ);
    if (!f) return false;
    std::string line;
    uint8_t buf[256];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
      for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          fn(line);
          line.clear();
        } else if (buf[i] != '\r' && line.size() < 4096) {
          line.push_back((char)buf[i]);
        }
      }
    }
    if (!line.empty()) fn(line);
    f.close();
    return true;
  }

  // One survey: (re)join the home network so the ESP32 picks its strongest node from where it
  // stands, then average three passive+active scans per BSSID.
  bool scan(std::vector<wm::ScanEntry>& out, bool& hasConn, wm::Mac& conn, int8_t& connRssi) override {
    WiFi.mode(WIFI_STA);
    hasConn = false;
    String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
    if (ssid.length()) {
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
      WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
      WiFi.begin(ssid.c_str(), pass.c_str());
      uint32_t t0 = ::millis();
      while (WiFi.status() != WL_CONNECTED && ::millis() - t0 < 8000) delay(100);
      if (WiFi.status() == WL_CONNECTED) {
        delay(300);
        const uint8_t* b = WiFi.BSSID();
        if (b) {
          memcpy(conn.b, b, 6);
          connRssi = (int8_t)WiFi.RSSI();
          hasConn = true;
        }
        if (time(nullptr) < 1600000000) configTime(0, 0, "pool.ntp.org");
      }
    }
    struct Acc { wm::ScanEntry e; int sum; int n; };
    std::vector<Acc> acc;
    for (int pass_ = 0; pass_ < 3; pass_++) {
      int n = WiFi.scanNetworks(false, true, false, 110);
      for (int i = 0; i < n; i++) {
        wm::Mac m;
        const uint8_t* b = WiFi.BSSID(i);
        if (!b) continue;
        memcpy(m.b, b, 6);
        Acc* a = nullptr;
        for (Acc& x : acc)
          if (x.e.mac == m) a = &x;
        if (!a) {
          acc.push_back({{m, 0, (uint8_t)WiFi.channel(i), WiFi.SSID(i).c_str()}, 0, 0});
          a = &acc.back();
        }
        a->sum += WiFi.RSSI(i);
        a->n++;
      }
      WiFi.scanDelete();
      if (n < 0 && pass_ == 2 && acc.empty()) return false;
    }
    out.clear();
    for (Acc& a : acc) {
      a.e.rssi = (int8_t)(a.sum / a.n);
      out.push_back(a.e);
    }
    std::sort(out.begin(), out.end(), [](const wm::ScanEntry& a, const wm::ScanEntry& b) { return a.rssi > b.rssi; });
    return true;
  }

  void hotspot(bool on) override;
  std::string hotspotLine(int i) override {
    switch (i) {
      case 0: return "1. On your phone or laptop, join:";
      case 1: return std::string("   ") + AP_SSID + "   password " + AP_PASS;
      case 2: return "2. Open this address in a browser:";
      case 3: return "   http://192.168.4.1";
      case 4: return "3. Add floor plans, trace walls, save.";
      case 5: return "   Tap DONE when finished.";
      default: return "";
    }
  }
};

static CydPlatform platform;
static wm::App app(platform);

// ============================================================ setup hotspot + web API
static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static String jsonEsc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

static void rmTree(const String& dir) {
  File d = SD.open(dir);
  if (!d) return;
  if (d.isDirectory()) {
    std::vector<String> kids;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      String n = f.name();
      int s = n.lastIndexOf('/');
      kids.push_back(dir + "/" + (s >= 0 ? n.substring(s + 1) : n));
      f.close();
    }
    d.close();
    for (String& k : kids) rmTree(k);
    SD.rmdir(dir);
  } else {
    d.close();
    SD.remove(dir);
  }
}

static void webSetup() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
  });
  server.on("/api/state", HTTP_GET, []() {
    String j = String("{\"version\":\"") + FW_VERSION + "\",\"sd\":" + (sdOk ? "true" : "false") + ",\"wifi\":\"" +
               jsonEsc(prefs.getString("ssid", "")) + "\",\"projects\":[";
    std::vector<std::string> p = platform.listProjects();
    for (size_t i = 0; i < p.size(); i++) j += String(i ? "," : "") + "\"" + jsonEsc(p[i].c_str()) + "\"";
    sendJson(200, j + "]}");
  });
  server.on("/api/file", HTTP_GET, []() {
    String p = server.arg("path");
    if (!safePath(p)) return sendJson(400, "{\"error\":\"bad path\"}");
    File f = SD.open(p, FILE_READ);
    if (!f || f.isDirectory()) return sendJson(404, "{\"error\":\"not found\"}");
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "application/octet-stream");
    f.close();
  });
  server.on("/api/file", HTTP_POST, []() {
    String p = server.arg("path");
    if (!sdOk) return sendJson(500, "{\"error\":\"no SD card\"}");
    if (!safePath(p) || !server.hasArg("plain")) return sendJson(400, "{\"error\":\"bad request\"}");
    const String& body = server.arg("plain");
    bool ok = platform.writeFile(p.c_str(), std::string(body.c_str(), body.length()));
    sendJson(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"write failed\"}");
  });
  server.on("/api/delete", HTTP_POST, []() {
    String p = server.arg("path");
    if (!safePath(p) || p.length() < 10) return sendJson(400, "{\"error\":\"bad path\"}");
    rmTree(p);
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/wifi", HTTP_POST, []() {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    sendJson(200, "{\"ok\":true}");
  });
  server.onNotFound([]() {  // captive-portal style redirect
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });
}

void CydPlatform::hotspot(bool on) {
  if (on == hotspotOn) return;
  hotspotOn = on;
  if (on) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(200);
    dns.start(53, "*", WiFi.softAPIP());
    server.begin();
  } else {
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
}

// ============================================================ display
struct TftCanvas : wm::Canvas {
  int oy = 0;
  void fillRect(int x, int y, int w, int h, uint16_t c) override { spr.fillRect(x, y - oy, w, h, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) override { spr.drawRect(x, y - oy, w, h, c); }
  void line(int x0, int y0, int x1, int y1, uint16_t c) override { spr.drawLine(x0, y0 - oy, x1, y1 - oy, c); }
  void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) override {
    spr.fillTriangle(x0, y0 - oy, x1, y1 - oy, x2, y2 - oy, c);
  }
  void fillCircle(int x, int y, int r, uint16_t c) override { spr.fillCircle(x, y - oy, r, c); }
  void text(int x, int y, const char* s, uint16_t c, int size) override {
    spr.setTextColor(c);
    spr.drawString(s, x, y - oy, size == 1 ? 1 : 2);
  }
  int textW(const char* s, int size) override { return spr.textWidth(s, size == 1 ? 1 : 2); }
};
static TftCanvas canvas;

static void render() {
  for (int y = 0; y < wm::SCR_H; y += bandH) {
    canvas.oy = canvas.bandY0 = y;
    canvas.bandY1 = y + bandH - 1;
    app.draw(canvas);
    spr.pushSprite(0, y);
  }
}

// ============================================================ Arduino entry points
void setup() {
  Serial.begin(115200);
  prefs.begin("wifimap", false);

  pinMode(PIN_T_CLK, OUTPUT);
  pinMode(PIN_T_CS, OUTPUT);
  pinMode(PIN_T_DIN, OUTPUT);
  pinMode(PIN_T_DOUT, INPUT);
  digitalWrite(PIN_T_CS, HIGH);
  digitalWrite(PIN_T_CLK, LOW);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif
  tft.setTextColor(TFT_CYAN);
  tft.drawString("CYD Wi-Fi Mapper " FW_VERSION, 8, 8, 2);

  // The band sprite is the only large allocation; shrink it if the heap is fragmented.
  spr.setColorDepth(16);
  while (bandH >= 20 && !spr.createSprite(wm::SCR_W, bandH)) bandH = bandH == 60 ? 40 : bandH / 2;

  sdOk = SD.begin(PIN_SD_CS);
  if (sdOk && !SD.exists("/wifimap")) SD.mkdir("/wifimap");
  Serial.printf("SD %s, band %d px, free heap %u\n", sdOk ? "ok" : "MISSING", bandH, (unsigned)ESP.getFreeHeap());

  // Calibrate on first boot, or when the screen is held during power-up.
  calLoad();
  uint16_t u, v;
  bool held = touchRaw(u, v);
  if (held) {
    delay(800);
    held = touchRaw(u, v);
  }
  if (held || prefs.getBytesLength("cal") != sizeof(cal)) calRun();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  webSetup();
  app.begin();
}

void loop() {
  if (hotspotOn) {
    dns.processNextRequest();
    server.handleClient();
  }
  static bool was = false;
  static int lx = 0, ly = 0, miss = 0;
  int x, y;
  if (touchRead(x, y)) {
    lx = x;
    ly = y;
    miss = 0;
    was = true;
  } else if (was && ++miss >= 2) {  // two misses in a row = finger lifted
    was = false;
  }
  app.touch(was, lx, ly);
  app.tick();
  if (app.dirty) {
    app.dirty = false;
    render();
  }
  delay(hotspotOn ? 2 : 8);
}
