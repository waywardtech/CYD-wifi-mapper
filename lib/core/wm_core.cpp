#include "wm_core.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace wm {

const char* const UNDO_LINE = "{\"undo\":1}";

// ---------- Mac ----------
bool Mac::operator==(const Mac& o) const { return memcmp(b, o.b, 6) == 0; }
bool Mac::zero() const {
  for (int i = 0; i < 6; i++)
    if (b[i]) return false;
  return true;
}

std::string macToStr(const Mac& m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m.b[0], m.b[1], m.b[2], m.b[3], m.b[4], m.b[5]);
  return buf;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool macFromStr(const char* s, Mac& out) {
  if (!s) return false;
  int n = 0, hi = -1;
  for (; *s; s++) {
    int v = hexVal(*s);
    if (v < 0) {
      if (*s == ':' || *s == '-' || *s == ' ') continue;
      return false;
    }
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= 6) return false;
      out.b[n++] = (uint8_t)(hi * 16 + v);
      hi = -1;
    }
  }
  return n == 6 && hi < 0;
}

// ---------- kinds ----------
static const char* const KIND_NAMES[K_COUNT] = {"Router", "Mesh node", "Switch", "Speaker", "TV", "Console", "Other"};
const char* kindName(uint8_t k) { return KIND_NAMES[k < K_COUNT ? k : (uint8_t)K_OTHER]; }
float kindDefaultZ(uint8_t k) { return k == K_CONSOLE ? 0.4f : (k == K_TV ? 1.1f : 0.9f); }

int Project::floorIndex(int id) const {
  for (size_t i = 0; i < floors.size(); i++)
    if (floors[i].id == id) return (int)i;
  return -1;
}

// ---------- JSON ----------
static float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

bool projectFromJson(const std::string& json, Project& out) {
  JsonDocument d;
  if (deserializeJson(d, json)) return false;
  out.name = d["name"] | "";
  out.floors.clear();
  out.objects.clear();
  for (JsonObject jf : d["floors"].as<JsonArray>()) {
    Floor f;
    f.id = jf["id"] | 0;
    f.name = jf["name"] | "";
    f.level = jf["level"] | 0;
    f.height = jf["height"] | 2.5f;
    if (f.height < 1.0f) f.height = 2.5f;
    for (JsonArray pt : jf["outline"].as<JsonArray>()) f.outline.push_back({pt[0] | 0.0f, pt[1] | 0.0f});
    for (JsonArray w : jf["walls"].as<JsonArray>())
      f.walls.push_back({w[0] | 0.0f, w[1] | 0.0f, w[2] | 0.0f, w[3] | 0.0f});
    out.floors.push_back(f);
  }
  std::stable_sort(out.floors.begin(), out.floors.end(), [](const Floor& a, const Floor& b) { return a.level < b.level; });
  for (JsonObject jo : d["objects"].as<JsonArray>()) {
    MapObject o;
    o.name = jo["name"] | "";
    int k = jo["kind"] | (int)K_OTHER;
    o.kind = (uint8_t)(k >= 0 && k < K_COUNT ? k : K_OTHER);
    o.floor = jo["floor"] | 0;
    o.x = jo["x"] | 0.0f;
    o.y = jo["y"] | 0.0f;
    o.z = jo["z"] | 1.0f;
    for (JsonVariant b : jo["bssids"].as<JsonArray>()) {
      Mac m;
      if (macFromStr(b.as<const char*>(), m)) o.bssids.push_back(m);
    }
    out.objects.push_back(o);
  }
  return true;
}

std::string projectToJson(const Project& p) {
  JsonDocument d;
  d["v"] = 1;
  d["name"] = p.name;
  JsonArray fl = d["floors"].to<JsonArray>();
  for (const Floor& f : p.floors) {
    JsonObject jf = fl.add<JsonObject>();
    jf["id"] = f.id;
    jf["name"] = f.name;
    jf["level"] = f.level;
    jf["height"] = r2(f.height);
    JsonArray o = jf["outline"].to<JsonArray>();
    for (const Vec2& v : f.outline) {
      JsonArray pt = o.add<JsonArray>();
      pt.add(r2(v.x));
      pt.add(r2(v.y));
    }
    JsonArray w = jf["walls"].to<JsonArray>();
    for (const Seg& s : f.walls) {
      JsonArray a = w.add<JsonArray>();
      a.add(r2(s.x1));
      a.add(r2(s.y1));
      a.add(r2(s.x2));
      a.add(r2(s.y2));
    }
  }
  JsonArray ob = d["objects"].to<JsonArray>();
  for (const MapObject& o : p.objects) {
    JsonObject jo = ob.add<JsonObject>();
    jo["name"] = o.name;
    jo["kind"] = o.kind;
    jo["floor"] = o.floor;
    jo["x"] = r2(o.x);
    jo["y"] = r2(o.y);
    jo["z"] = r2(o.z);
    JsonArray b = jo["bssids"].to<JsonArray>();
    for (const Mac& m : o.bssids) b.add(macToStr(m));
  }
  std::string s;
  serializeJson(d, s);
  return s;
}

std::string sampleToLine(const Sample& s, const std::vector<std::string>* ssids) {
  JsonDocument d;
  d["t"] = s.t;
  d["f"] = s.floor;
  JsonArray p = d["p"].to<JsonArray>();
  p.add(r2(s.x));
  p.add(r2(s.y));
  p.add(r2(s.z));
  if (s.hasConn) {
    JsonArray c = d["c"].to<JsonArray>();
    c.add(macToStr(s.conn));
    c.add(s.connRssi);
  }
  JsonArray a = d["a"].to<JsonArray>();
  for (size_t i = 0; i < s.aps.size(); i++) {
    JsonArray e = a.add<JsonArray>();
    e.add(macToStr(s.aps[i].mac));
    e.add(s.aps[i].rssi);
    e.add(s.aps[i].ch);
    if (ssids && i < ssids->size()) e.add((*ssids)[i]);
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

bool applySampleLine(const std::string& line, std::vector<Sample>& samples, size_t maxAps) {
  if (line.size() < 2) return false;
  JsonDocument d;
  if (deserializeJson(d, line)) return false;
  if (d["undo"] | 0) {
    if (!samples.empty()) samples.pop_back();
    return true;
  }
  if (!d["p"].is<JsonArray>()) return false;
  Sample s;
  s.t = d["t"] | 0u;
  s.floor = d["f"] | 0;
  s.x = d["p"][0] | 0.0f;
  s.y = d["p"][1] | 0.0f;
  s.z = d["p"][2] | 1.2f;
  if (d["c"].is<JsonArray>() && macFromStr(d["c"][0] | "", s.conn)) {
    s.hasConn = true;
    s.connRssi = (int8_t)(d["c"][1] | 0);
  }
  for (JsonArray e : d["a"].as<JsonArray>()) {
    if (s.aps.size() >= maxAps) break;
    ApReading r;
    if (!macFromStr(e[0] | "", r.mac)) continue;
    r.rssi = (int8_t)(e[1] | -100);
    r.ch = (uint8_t)(e[2] | 0);
    s.aps.push_back(r);
  }
  samples.push_back(s);
  return true;
}

// ---------- view ----------
void View::project(float x, float y, float z, float& sx, float& sy) const {
  float dx = x - cx, dy = y - cy, c = cosf(yaw), s = sinf(yaw);
  float xr = dx * c - dy * s, yr = dx * s + dy * c;
  sx = sx0 + scale * xr;
  sy = sy0 + scale * (yr * sinf(pitch) - (z - cz) * cosf(pitch));
}

void View::unproject(float sx, float sy, float z, float& x, float& y) const {
  float sp = sinf(pitch);
  if (sp < 0.05f) sp = 0.05f;
  float xr = (sx - sx0) / scale;
  float yr = ((sy - sy0) / scale + (z - cz) * cosf(pitch)) / sp;
  float c = cosf(yaw), s = sinf(yaw);
  x = cx + xr * c + yr * s;
  y = cy - xr * s + yr * c;
}

float View::zFromScreen(float sy, float baseSy) const {
  float cp = cosf(pitch);
  if (cp < 0.05f) return 0;
  return (baseSy - sy) / (scale * cp);
}

// ---------- geometry / colour ----------
bool pointInPoly(const std::vector<Vec2>& poly, float x, float y) {
  bool in = false;
  size_t n = poly.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 &a = poly[i], &b = poly[j];
    if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) in = !in;
  }
  return in;
}

Bounds floorBounds(const Floor& f) {
  Bounds b = {1e9f, 1e9f, -1e9f, -1e9f};
  auto add = [&b](float x, float y) {
    b.x0 = std::min(b.x0, x);
    b.y0 = std::min(b.y0, y);
    b.x1 = std::max(b.x1, x);
    b.y1 = std::max(b.y1, y);
  };
  for (const Vec2& v : f.outline) add(v.x, v.y);
  for (const Seg& s : f.walls) {
    add(s.x1, s.y1);
    add(s.x2, s.y2);
  }
  if (b.x1 < b.x0) b = {0, 0, 4, 4};
  return b;
}

uint16_t dim565(uint16_t c) { return (uint16_t)((c >> 1) & 0x7BEF); }

uint16_t rssiColor(float rssi) {
  static const struct { float v; uint8_t r, g, b; } STOPS[] = {
      {-92, 110, 0, 30}, {-82, 235, 40, 30}, {-70, 245, 205, 30}, {-60, 70, 215, 60}, {-48, 0, 235, 130}};
  const int N = sizeof(STOPS) / sizeof(STOPS[0]);
  if (rssi <= STOPS[0].v) return rgb565(STOPS[0].r, STOPS[0].g, STOPS[0].b);
  for (int i = 1; i < N; i++) {
    if (rssi <= STOPS[i].v) {
      float t = (rssi - STOPS[i - 1].v) / (STOPS[i].v - STOPS[i - 1].v);
      return rgb565((uint8_t)(STOPS[i - 1].r + t * (STOPS[i].r - STOPS[i - 1].r)),
                    (uint8_t)(STOPS[i - 1].g + t * (STOPS[i].g - STOPS[i - 1].g)),
                    (uint8_t)(STOPS[i - 1].b + t * (STOPS[i].b - STOPS[i - 1].b)));
    }
  }
  return rgb565(STOPS[N - 1].r, STOPS[N - 1].g, STOPS[N - 1].b);
}

uint16_t categoryColor(int idx) {
  static const uint16_t PAL[] = {rgb565(60, 160, 255), rgb565(255, 150, 40), rgb565(170, 110, 255),
                                 rgb565(60, 220, 190), rgb565(255, 90, 150), rgb565(220, 220, 70)};
  if (idx < 0 || idx >= 100) return rgb565(110, 110, 110);
  return PAL[idx % 6];
}

// ---------- sources ----------
static bool hasMac(const std::vector<Mac>& v, const Mac& m) {
  for (const Mac& x : v)
    if (x == m) return true;
  return false;
}

int linkedObject(const Project& p, const Sample& s) {
  if (!s.hasConn) return -1;
  for (size_t i = 0; i < p.objects.size(); i++)
    if (hasMac(p.objects[i].bssids, s.conn)) return (int)i;
  return -1;
}

bool sampleValue(const Project& p, const Sample& s, int src, float& v) {
  if (src == SRC_CONN) {
    if (!s.hasConn) return false;
    v = s.connRssi;
    return true;
  }
  if (src == SRC_LINK) {
    if (!s.hasConn) return false;
    int o = linkedObject(p, s);
    v = o < 0 ? 100 : o;
    return true;
  }
  if (src >= 0) {
    if (src >= (int)p.objects.size()) return false;
    const MapObject& o = p.objects[src];
    if (o.bssids.empty()) return false;
    v = RSSI_FLOOR;
    for (const ApReading& a : s.aps)
      if (hasMac(o.bssids, a.mac) && a.rssi > v) v = a.rssi;
    return true;
  }
  // SRC_BEST: strongest of the placed+bound sources; if nothing is bound yet, strongest of anything heard.
  bool anyBound = false;
  for (const MapObject& o : p.objects)
    if (!o.bssids.empty()) anyBound = true;
  v = RSSI_FLOOR;
  for (const ApReading& a : s.aps) {
    if (a.rssi <= v) continue;
    if (!anyBound) {
      v = a.rssi;
      continue;
    }
    for (const MapObject& o : p.objects)
      if (hasMac(o.bssids, a.mac)) {
        v = a.rssi;
        break;
      }
  }
  return true;
}

void computeHeat(const Project& p, int floorId, int src, HeatGrid& g) {
  g = HeatGrid();
  int fi = p.floorIndex(floorId);
  if (fi < 0) return;
  const Floor& f = p.floors[fi];
  Bounds b = floorBounds(f);
  float w = b.x1 - b.x0, h = b.y1 - b.y0;
  if (w <= 0 || h <= 0) return;
  g.cell = std::max(0.2f, std::max(w / 48.0f, h / 36.0f));
  g.nx = (int)ceilf(w / g.cell);
  g.ny = (int)ceilf(h / g.cell);
  g.x0 = b.x0;
  g.y0 = b.y0;
  g.categorical = (src == SRC_LINK);
  g.v.assign((size_t)g.nx * g.ny, (int8_t)HeatGrid::NONE);
  g.far.assign((size_t)g.nx * g.ny, 0);

  struct Pt { float x, y, v; };
  std::vector<Pt> pts;
  for (const Sample& s : p.samples) {
    float v;
    if (s.floor == floorId && sampleValue(p, s, src, v)) pts.push_back({s.x, s.y, v});
  }
  if (pts.empty()) return;
  bool mask = f.outline.size() >= 3;
  for (int j = 0; j < g.ny; j++) {
    for (int i = 0; i < g.nx; i++) {
      float x = g.x0 + (i + 0.5f) * g.cell, y = g.y0 + (j + 0.5f) * g.cell;
      if (mask && !pointInPoly(f.outline, x, y)) continue;
      float sw = 0, sv = 0, best = 1e9f, bestV = 0;
      for (const Pt& q : pts) {
        float d2 = (q.x - x) * (q.x - x) + (q.y - y) * (q.y - y);
        if (d2 < best) {
          best = d2;
          bestV = q.v;
        }
        float wgt = 1.0f / (d2 * d2 + 0.02f);  // power 4: keeps local detail, still smooth
        sw += wgt;
        sv += wgt * q.v;
      }
      float val = g.categorical ? bestV : sv / sw;
      size_t k = (size_t)j * g.nx + i;
      g.v[k] = (int8_t)std::max(-127.0f, std::min(120.0f, roundf(val)));
      g.far[k] = best > 16.0f;
    }
  }
}

}  // namespace wm
