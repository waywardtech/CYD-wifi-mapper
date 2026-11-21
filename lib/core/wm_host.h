// Host-only helpers (tests + simulator): in-memory storage, a recording canvas and a fake radio.
// Never included by the firmware.
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include "wm_app.h"

namespace wm {

struct HostPlatform : Platform {
  std::map<std::string, std::string> files;
  uint32_t now = 1000;
  bool hot = false;
  std::function<bool(std::vector<ScanEntry>&, bool&, Mac&, int8_t&)> scanFn;

  uint32_t millis() override { return now; }
  uint32_t epoch() override { return 1794700000u + now / 1000; }
  bool storageOk() override { return true; }
  std::vector<std::string> listProjects() override {
    std::vector<std::string> out;
    const std::string pre = "/wifimap/", suf = "/project.json";
    for (auto& kv : files) {
      const std::string& k = kv.first;
      if (k.compare(0, pre.size(), pre) == 0 && k.size() > pre.size() + suf.size() &&
          k.compare(k.size() - suf.size(), suf.size(), suf) == 0)
        out.push_back(k.substr(pre.size(), k.size() - pre.size() - suf.size()));
    }
    return out;
  }
  bool readFile(const std::string& p, std::string& out) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool writeFile(const std::string& p, const std::string& d) override {
    files[p] = d;
    return true;
  }
  bool appendLine(const std::string& p, const std::string& l) override {
    files[p] += l + "\n";
    return true;
  }
  bool readLines(const std::string& p, const std::function<void(const std::string&)>& fn) override {
    auto it = files.find(p);
    if (it == files.end()) return false;
    size_t a = 0;
    while (a < it->second.size()) {
      size_t b = it->second.find('\n', a);
      if (b == std::string::npos) b = it->second.size();
      fn(it->second.substr(a, b - a));
      a = b + 1;
    }
    return true;
  }
  bool scan(std::vector<ScanEntry>& out, bool& hc, Mac& c, int8_t& r) override {
    return scanFn ? scanFn(out, hc, c, r) : false;
  }
  void hotspot(bool on) override { hot = on; }
  std::string hotspotLine(int i) override {
    static const char* L[] = {"1. Join this Wi-Fi network:", "   CYD-WiFiMap  (pw mapmywifi)", "2. Open in a browser:", "   http://192.168.4.1",
                              "3. Add floor plans, then DONE."};
    return i < 5 ? L[i] : "";
  }
};

// Records draw calls as text; sim/render.py turns them into a PNG.
struct RecCanvas : Canvas {
  FILE* f = nullptr;
  void fillRect(int x, int y, int w, int h, uint16_t c) override { if (f) fprintf(f, "R %d %d %d %d %u\n", x, y, w, h, c); }
  void drawRect(int x, int y, int w, int h, uint16_t c) override { if (f) fprintf(f, "r %d %d %d %d %u\n", x, y, w, h, c); }
  void line(int a, int b, int c2, int d, uint16_t c) override { if (f) fprintf(f, "L %d %d %d %d %u\n", a, b, c2, d, c); }
  void fillTri(int a, int b, int c2, int d, int e, int g, uint16_t c) override { if (f) fprintf(f, "T %d %d %d %d %d %d %u\n", a, b, c2, d, e, g, c); }
  void fillCircle(int x, int y, int r, uint16_t c) override { if (f) fprintf(f, "C %d %d %d %u\n", x, y, r, c); }
  void text(int x, int y, const char* s, uint16_t c, int size) override { if (f) fprintf(f, "S %d %d %d %u %s\n", x, y, size, c, s); }
  int textW(const char* s, int size) override { return (int)strlen(s) * (size == 1 ? 6 : 7); }
};

// Crude indoor path-loss model so the simulator and tests have plausible numbers.
inline bool segCross(float ax, float ay, float bx, float by, const Seg& s) {
  auto o = [](float px, float py, float qx, float qy, float rx, float ry) { return (qx - px) * (ry - py) - (qy - py) * (rx - px); };
  float d1 = o(s.x1, s.y1, s.x2, s.y2, ax, ay), d2 = o(s.x1, s.y1, s.x2, s.y2, bx, by);
  float d3 = o(ax, ay, bx, by, s.x1, s.y1), d4 = o(ax, ay, bx, by, s.x2, s.y2);
  return (d1 * d2 < 0) && (d3 * d4 < 0);
}

inline float fakeRssi(const Project& p, const MapObject& ap, int floorId, float x, float y, float z) {
  int fa = p.floorIndex(ap.floor), fb = p.floorIndex(floorId);
  if (fa < 0 || fb < 0) return -100;
  float dz = (p.floors[fb].level - p.floors[fa].level) * 2.8f + z - ap.z;
  float d = sqrtf((x - ap.x) * (x - ap.x) + (y - ap.y) * (y - ap.y) + dz * dz);
  float r = -36 - 26 * log10f(d < 0.3f ? 0.3f : d);
  for (const Seg& w : p.floors[fb].walls)
    if (segCross(x, y, ap.x, ap.y, w)) r -= 6;
  r -= 15 * abs(p.floors[fb].level - p.floors[fa].level);
  return r < -100 ? -100 : r;
}

}  // namespace wm
