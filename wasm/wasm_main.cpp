// Browser build of the CYD app: the same lib/core UI code, compiled to WebAssembly.
// Display, touch, storage and the "radio" are provided by the page (web/emulator.html).
// Build with tools/build_artifact.py.
#include <stdlib.h>
#include <algorithm>
#include "wm_host.h"

using namespace wm;

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#define EXPORT(name) __attribute__((export_name(#name)))

extern "C" {
IMPORT(js_fill_rect) void js_fill_rect(int x, int y, int w, int h, int c);
IMPORT(js_draw_rect) void js_draw_rect(int x, int y, int w, int h, int c);
IMPORT(js_line) void js_line(int x0, int y0, int x1, int y1, int c);
IMPORT(js_tri) void js_tri(int x0, int y0, int x1, int y1, int x2, int y2, int c);
IMPORT(js_circle) void js_circle(int x, int y, int r, int c);
IMPORT(js_text) void js_text(int x, int y, const char* s, int len, int c, int size);
IMPORT(js_text_w) int js_text_w(const char* s, int len, int size);
IMPORT(js_now) double js_now();
IMPORT(js_epoch) double js_epoch();
// Files live in the page's storage. *_size returns -1 if missing and parks the text for js_take.
IMPORT(js_file_size) int js_file_size(const char* path, int len);
IMPORT(js_list_size) int js_list_size();
IMPORT(js_take) void js_take(char* dst);
IMPORT(js_file_write) void js_file_write(const char* path, int plen, const char* data, int dlen, int append);
IMPORT(js_hotspot) void js_hotspot(int on);
}

struct JsCanvas : Canvas {
  void fillRect(int x, int y, int w, int h, uint16_t c) override { js_fill_rect(x, y, w, h, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) override { js_draw_rect(x, y, w, h, c); }
  void line(int a, int b, int c2, int d, uint16_t c) override { js_line(a, b, c2, d, c); }
  void fillTri(int a, int b, int c2, int d, int e, int f, uint16_t c) override { js_tri(a, b, c2, d, e, f, c); }
  void fillCircle(int x, int y, int r, uint16_t c) override { js_circle(x, y, r, c); }
  void text(int x, int y, const char* s, uint16_t c, int size) override { js_text(x, y, s, (int)strlen(s), c, size); }
  int textW(const char* s, int size) override { return js_text_w(s, (int)strlen(s), size); }
};

static App* appPtr();

struct JsPlatform : Platform {
  uint32_t millis() override { return (uint32_t)js_now(); }
  uint32_t epoch() override { return (uint32_t)js_epoch(); }
  bool storageOk() override { return true; }
  static bool take(int n, std::string& out) {
    if (n < 0) return false;
    out.assign((size_t)n, '\0');
    if (n) js_take(&out[0]);
    return true;
  }
  std::vector<std::string> listProjects() override {
    std::vector<std::string> v;
    std::string all;
    take(js_list_size(), all);
    size_t a = 0;
    while (a < all.size()) {
      size_t b = all.find('\n', a);
      if (b == std::string::npos) b = all.size();
      if (b > a) v.push_back(all.substr(a, b - a));
      a = b + 1;
    }
    return v;
  }
  bool readFile(const std::string& p, std::string& out) override { return take(js_file_size(p.data(), (int)p.size()), out); }
  bool writeFile(const std::string& p, const std::string& d) override {
    js_file_write(p.data(), (int)p.size(), d.data(), (int)d.size(), 0);
    return true;
  }
  bool appendLine(const std::string& p, const std::string& l) override {
    std::string d = l + "\n";
    js_file_write(p.data(), (int)p.size(), d.data(), (int)d.size(), 1);
    return true;
  }
  bool readLines(const std::string& p, const std::function<void(const std::string&)>& fn) override {
    std::string all;
    if (!readFile(p, all)) return false;
    size_t a = 0;
    while (a < all.size()) {
      size_t b = all.find('\n', a);
      if (b == std::string::npos) b = all.size();
      fn(all.substr(a, b - a));
      a = b + 1;
    }
    return true;
  }

  // There is no Wi-Fi radio in a browser. Signal is *simulated* with a simple indoor path-loss
  // model: every placed router / mesh node transmits, walls and floors attenuate. A node that has
  // not been bound yet broadcasts a made-up address so BIND can find it.
  static Mac simMac(size_t objIndex) {
    Mac m;
    m.b[0] = 0x02;
    m.b[1] = 0x51;
    m.b[5] = (uint8_t)(objIndex + 1);
    return m;
  }
  bool scan(std::vector<ScanEntry>& out, bool& hasConn, Mac& conn, int8_t& connRssi) override {
    App& a = *appPtr();
    const Floor* f = a.curFloor();
    if (!f) return false;
    float x = a.meX, y = a.meY, z = a.meZ;
    if (a.pendingScan == 0 && a.mode == M_PLACE && a.selObj >= 0) {  // BIND: you stand next to the object
      const MapObject& o = a.proj.objects[a.selObj];
      x = o.x + 0.3f;
      y = o.y + 0.2f;
      z = o.z;
    }
    float best = -200;
    for (size_t i = 0; i < a.proj.objects.size(); i++) {
      const MapObject& o = a.proj.objects[i];
      if (o.kind != K_ROUTER && o.kind != K_MESH) continue;
      float v = fakeRssi(a.proj, o, f->id, x, y, z) + (float)(rand() % 5 - 2);
      if (v < -93) continue;
      Mac m = o.bssids.empty() ? simMac(i) : o.bssids[0];
      out.push_back({m, (int8_t)v, (uint8_t)(1 + 5 * (i % 3)), "HomeNet (sim)"});
      if (!o.bssids.empty() && v > best) {
        best = v;
        conn = m;
      }
    }
    for (int i = 0; i < 4; i++) {  // a few neighbours for realism
      Mac m;
      m.b[0] = 0x5c;
      m.b[5] = (uint8_t)i;
      out.push_back({m, (int8_t)(-76 - rand() % 16), (uint8_t)(1 + 3 * i), "Neighbour (sim)"});
    }
    std::sort(out.begin(), out.end(), [](const ScanEntry& p, const ScanEntry& q) { return p.rssi > q.rssi; });
    hasConn = best > -200;
    connRssi = (int8_t)best;
    return true;
  }
  void hotspot(bool on) override { js_hotspot(on ? 1 : 0); }
  std::string hotspotLine(int i) override {
    static const char* L[] = {"On a real CYD this starts a Wi-Fi", "hotspot for adding floor plans.", "Here, use the SETUP tab above:", "add or edit floors, then come back", "and tap DONE."};
    return i < 5 ? L[i] : "";
  }
};

static JsPlatform platform;
static JsCanvas canvas;
static App* appPtr() {
  static App a(platform);
  return &a;
}

extern "C" {
EXPORT(wm_init) void wm_init() { appPtr()->begin(); }
EXPORT(wm_touch) void wm_touch(int down, int x, int y) { appPtr()->touch(down != 0, x, y); }
// Run one loop iteration. Returns 1 if the screen was redrawn.
EXPORT(wm_frame) int wm_frame() {
  App& a = *appPtr();
  a.tick();
  if (!a.dirty) return 0;
  a.dirty = false;
  canvas.bandY0 = 0;
  canvas.bandY1 = SCR_H - 1;
  a.draw(canvas);
  return 1;
}
EXPORT(wm_screen) int wm_screen() { return (int)appPtr()->screen; }
}
