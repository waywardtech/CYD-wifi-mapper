#include "wm_app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

namespace wm {

namespace {
const float PI_2 = 1.5707963f;
const int ROWS = 6, ROW_H = 30, ROW_Y = 24;

// CAD-style palette
const uint16_t C_BG = 0x0000;
const uint16_t C_GRID = rgb565(10, 36, 52);
const uint16_t C_WALL = rgb565(70, 225, 255);
const uint16_t C_WALL2 = rgb565(40, 160, 190);
const uint16_t C_WALLDIM = rgb565(22, 90, 112);
const uint16_t C_GHOST = rgb565(18, 58, 76);
const uint16_t C_TEXT = rgb565(200, 235, 245);
const uint16_t C_MUTED = rgb565(110, 150, 165);
const uint16_t C_AMBER = rgb565(255, 180, 0);
const uint16_t C_BAR = rgb565(6, 22, 30);
const uint16_t C_BTN = rgb565(40, 120, 145);
const uint16_t C_ON = rgb565(20, 80, 100);
const uint16_t C_ME = rgb565(255, 70, 255);
const uint16_t C_OK = rgb565(60, 220, 90);
const uint16_t C_DEV = rgb565(150, 170, 200);

std::string trunc(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}
}  // namespace

// ------------------------------------------------------------------ lifecycle

void App::begin() {
  slugs = P.listProjects();
  screen = S_HOME;
  dirty = true;
}

std::string App::path(const char* file) const { return "/wifimap/" + slug + "/" + file; }

bool App::openProject(const std::string& s) {
  std::string json;
  Project p;
  if (!P.readFile("/wifimap/" + s + "/project.json", json) || !projectFromJson(json, p)) {
    say("Could not read project");
    return false;
  }
  slug = s;
  proj = p;
  if (proj.name.empty()) proj.name = s;
  P.readLines(path("samples.jsonl"), [this](const std::string& l) { applySampleLine(l, proj.samples); });
  floorIdx = 0;
  for (size_t i = 0; i < proj.floors.size(); i++)
    if (proj.floors[i].level == 0) floorIdx = (int)i;
  selObj = -1;
  addArmed = meSet = false;
  src = SRC_BEST;
  zoom = 1;
  panX = panY = 0;
  iso = true;
  isoT = 1;
  mode = proj.objects.empty() ? M_PLACE : (proj.samples.empty() ? M_SURVEY : M_VIEW);
  heatDirty = true;
  screen = S_MAP;
  updateView();
  dirty = true;
  return true;
}

void App::saveProject() {
  if (!P.writeFile(path("project.json"), projectToJson(proj))) say("SAVE FAILED - check SD");
}

const Floor* App::curFloor() const {
  if (floorIdx < 0 || floorIdx >= (int)proj.floors.size()) return nullptr;
  return &proj.floors[floorIdx];
}

void App::say(const std::string& msg) {
  toast = msg;
  toastUntil = P.millis() + 2500;
  dirty = true;
}

void App::updateView() {
  view.sx0 = SCR_W / 2;
  view.sy0 = TOP_H + MAP_H / 2;
  const Floor* f = curFloor();
  if (!f) return;
  Bounds b = floorBounds(*f);
  float bw = std::max(b.x1 - b.x0, 0.5f), bh = std::max(b.y1 - b.y0, 0.5f);
  float t = isoT;
  view.pitch = PI_2 + (isoPitch - PI_2) * t;
  view.yaw = isoYaw * t;
  float fit2 = 0.9f * std::min(300.0f / bw, (MAP_H - 10.0f) / bh);
  float diag = sqrtf(bw * bw + bh * bh);
  float fit3 = 0.92f * std::min(310.0f / diag, (MAP_H - 6.0f) / (diag * sinf(isoPitch) + f->height * cosf(isoPitch)));
  view.scale = (fit2 + (fit3 - fit2) * t) * zoom;
  view.cx = (b.x0 + b.x1) / 2 + panX;
  view.cy = (b.y0 + b.y1) / 2 + panY;
  view.cz = f->height * 0.5f * t;
}

void App::tick() {
  uint32_t now = P.millis();
  float target = iso ? 1.0f : 0.0f;
  if (isoT != target) {
    float step = (now - lastTick) / 250.0f;
    if (step <= 0) step = 0.05f;
    isoT = target > isoT ? std::min(target, isoT + step) : std::max(target, isoT - step);
    updateView();
    dirty = true;
  }
  lastTick = now;
  if (!toast.empty() && (int32_t)(now - toastUntil) > 0) {
    toast.clear();
    dirty = true;
  }
  if (pendingScan && overlayShown) doScan();
}

// ------------------------------------------------------------------ sources

std::vector<int> App::sources() const {
  std::vector<int> s;
  s.push_back(SRC_BEST);
  for (size_t i = 0; i < proj.objects.size(); i++)
    if (!proj.objects[i].bssids.empty()) s.push_back((int)i);
  for (const Sample& x : proj.samples)
    if (x.hasConn) {
      s.push_back(SRC_CONN);
      s.push_back(SRC_LINK);
      break;
    }
  return s;
}

std::string App::sourceName(int s) const {
  if (s == SRC_BEST) return "Best AP";
  if (s == SRC_CONN) return "Link dBm";
  if (s == SRC_LINK) return "Link node";
  if (s >= 0 && s < (int)proj.objects.size()) return proj.objects[s].name;
  return "?";
}

// ------------------------------------------------------------------ buttons

void App::buildButtons(std::vector<Btn>& o) const {
  o.clear();
  const int by = SCR_H - BOT_H;
  if (screen == S_HOME) {
    for (int i = 0; i < ROWS; i++) {
      size_t k = (size_t)page * ROWS + i;
      if (k < slugs.size()) o.push_back({8, ROW_Y + i * ROW_H, 304, ROW_H - 4, B_ROW + (int)k, slugs[k], false});
    }
    o.push_back({0, by, 160, BOT_H, B_HOTSPOT, "HOTSPOT SETUP", false});
    o.push_back({160, by, 80, BOT_H, B_PREV, "PREV", false});
    o.push_back({240, by, 80, BOT_H, B_NEXT, "NEXT", false});
  } else if (screen == S_HOTSPOT) {
    o.push_back({0, by, 320, BOT_H, B_DONE, "DONE", false});
  } else if (screen == S_LIST) {
    for (int i = 0; i < ROWS; i++) {
      size_t k = (size_t)page * ROWS + i;
      if (k < listItems.size()) o.push_back({8, ROW_Y + i * ROW_H, 304, ROW_H - 4, B_ROW + (int)k, listItems[k], false});
    }
    o.push_back({0, by, 110, BOT_H, B_CANCEL, "CANCEL", false});
    o.push_back({110, by, 105, BOT_H, B_PREV, "PREV", false});
    o.push_back({215, by, 105, BOT_H, B_NEXT, "NEXT", false});
  } else if (screen == S_MAP) {
    const Floor* f = curFloor();
    o.push_back({0, 0, 26, TOP_H, B_HOME, "=", false});
    o.push_back({26, 0, 112, TOP_H, B_FLOOR, f ? trunc(f->name, 12) + " >" : "-", false});
    o.push_back({276, 0, 44, TOP_H, B_ISO, iso ? "ISO" : "2D", false});
    static const char* MODES[] = {"PLACE", "SURVEY", "VIEW"};
    o.push_back({0, by, 64, BOT_H, B_MODE, MODES[mode], true});
    if (mode == M_PLACE) {
      o.push_back({64, by, 56, BOT_H, B_ADD, "ADD", addArmed});
      o.push_back({120, by, 56, BOT_H, B_BIND, "BIND", false});
      o.push_back({176, by, 56, BOT_H, B_DEL, "DEL", false});
    } else if (mode == M_SURVEY) {
      o.push_back({64, by, 112, BOT_H, B_SCAN, "SCAN HERE", false});
      o.push_back({176, by, 56, BOT_H, B_UNDO, "UNDO", false});
    } else {
      o.push_back({64, by, 112, BOT_H, B_SRC, trunc(sourceName(src), 13), false});
      o.push_back({176, by, 56, BOT_H, B_FIELD, field ? "FIELD" : "DOTS", false});
    }
    o.push_back({232, by, 44, BOT_H, B_ZOUT, "-", false});
    o.push_back({276, by, 44, BOT_H, B_ZIN, "+", false});
  }
}

int App::hitButton(int x, int y) const {
  std::vector<Btn> b;
  buildButtons(b);
  for (const Btn& k : b)
    if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return k.id;
  return B_NONE;
}

void App::openList(ListPurpose p, const std::string& title) {
  listPurpose = p;
  listTitle = title;
  page = 0;
  screen = S_LIST;
  dirty = true;
}

void App::press(int id) {
  dirty = true;
  if (id >= B_ROW) {
    int idx = id - B_ROW;
    if (screen == S_HOME) {
      if (idx < (int)slugs.size()) openProject(slugs[idx]);
    } else if (screen == S_LIST) {
      listPick(idx);
    }
    return;
  }
  size_t count = screen == S_HOME ? slugs.size() : listItems.size();
  switch (id) {
    case B_PREV:
      if (page > 0) page--;
      break;
    case B_NEXT:
      if ((size_t)(page + 1) * ROWS < count) page++;
      break;
    case B_CANCEL:
      screen = S_MAP;
      break;
    case B_HOTSPOT:
      P.hotspot(true);
      screen = S_HOTSPOT;
      break;
    case B_DONE:
      P.hotspot(false);
      begin();
      break;
    case B_HOME:
      page = 0;
      begin();
      break;
    case B_FLOOR:
      if (!proj.floors.empty()) {
        floorIdx = (floorIdx + 1) % (int)proj.floors.size();
        selObj = -1;
        meSet = false;
        panX = panY = 0;
        heatDirty = true;
        updateView();
      }
      break;
    case B_ISO:
      iso = !iso;
      break;
    case B_MODE:
      mode = (Mode)((mode + 1) % 3);
      addArmed = false;
      heatDirty = true;
      break;
    case B_ZIN:
    case B_ZOUT:
      zoom = clampf(zoom * (id == B_ZIN ? 1.25f : 0.8f), 0.5f, 6.0f);
      updateView();
      break;
    case B_ADD:
      addArmed = !addArmed;
      if (addArmed) say(iso ? "Switch to 2D, then tap the spot" : "Tap where it sits");
      break;
    case B_DEL:
      if (selObj < 0) {
        say("Select an object first");
      } else {
        proj.objects.erase(proj.objects.begin() + selObj);
        selObj = -1;
        src = SRC_BEST;
        heatDirty = true;
        saveProject();
      }
      break;
    case B_BIND:
      if (selObj < 0) {
        say("Select an object first");
      } else {
        pendingScan = 2;
        overlayShown = false;
      }
      break;
    case B_SCAN:
      if (!curFloor()) break;
      if (!meSet) {
        say("Tap your spot in 2D first");
      } else {
        pendingScan = 1;
        overlayShown = false;
      }
      break;
    case B_UNDO:
      if (proj.samples.empty()) {
        say("Nothing to undo");
      } else {
        proj.samples.pop_back();
        P.appendLine(path("samples.jsonl"), UNDO_LINE);
        heatDirty = true;
        say(fmt("Removed. %d samples", (int)proj.samples.size()));
      }
      break;
    case B_SRC: {
      std::vector<int> s = sources();
      size_t i = 0;
      while (i < s.size() && s[i] != src) i++;
      src = s[(i + 1) % s.size()];
      heatDirty = true;
      break;
    }
    case B_FIELD:
      field = !field;
      break;
    default:
      break;
  }
}

void App::listPick(int idx) {
  screen = S_MAP;
  const Floor* f = curFloor();
  if (listPurpose == L_KIND && f && idx < K_COUNT) {
    MapObject o;
    o.kind = (uint8_t)idx;
    int n = 1;
    for (const MapObject& e : proj.objects)
      if (e.kind == o.kind) n++;
    o.name = fmt("%s %d", kindName(o.kind), n);
    o.floor = f->id;
    o.x = pendX;
    o.y = pendY;
    o.z = std::min(kindDefaultZ(o.kind), f->height);
    proj.objects.push_back(o);
    selObj = (int)proj.objects.size() - 1;
    addArmed = false;
    saveProject();
    say(o.name + " - set height in ISO");
  } else if (listPurpose == L_BIND && selObj >= 0 && selObj < (int)proj.objects.size()) {
    MapObject& o = proj.objects[selObj];
    if (!o.bssids.empty()) {
      if (idx == 0) {
        o.bssids.clear();
        saveProject();
        heatDirty = true;
        say("Bindings cleared");
        return;
      }
      idx--;
    }
    if (idx < (int)scanList.size()) {
      const Mac& m = scanList[idx].mac;
      if (std::find(o.bssids.begin(), o.bssids.end(), m) == o.bssids.end()) o.bssids.push_back(m);
      saveProject();
      heatDirty = true;
      say(o.name + " = " + macToStr(m).substr(9));
    }
  }
}

// ------------------------------------------------------------------ scanning

void App::doScan() {
  int what = pendingScan;
  pendingScan = 0;
  overlayShown = false;
  dirty = true;
  std::vector<ScanEntry> res;
  bool hc = false;
  Mac cm;
  int8_t cr = 0;
  if (!P.scan(res, hc, cm, cr)) {
    say("Scan failed");
    return;
  }
  if (what == 2) {
    if (selObj < 0 || selObj >= (int)proj.objects.size()) return;
    const MapObject& o = proj.objects[selObj];
    if (res.size() > 17) res.resize(17);
    scanList = res;
    listItems.clear();
    if (!o.bssids.empty()) listItems.push_back("(clear bindings)");
    for (const ScanEntry& e : res) {
      bool mine = std::find(o.bssids.begin(), o.bssids.end(), e.mac) != o.bssids.end();
      listItems.push_back(fmt("%c%d ch%d %s %s", mine ? '*' : ' ', e.rssi, e.ch, trunc(e.ssid.empty() ? "(hidden)" : e.ssid, 14).c_str(),
                              macToStr(e.mac).substr(9).c_str()));
    }
    openList(L_BIND, "Stand by " + trunc(o.name, 12) + ": strongest first");
    return;
  }
  const Floor* f = curFloor();
  if (!f) return;
  Sample s;
  s.floor = f->id;
  s.x = meX;
  s.y = meY;
  s.z = meZ;
  s.t = P.epoch();
  s.hasConn = hc;
  s.conn = cm;
  s.connRssi = cr;
  std::vector<std::string> ssids;
  for (size_t i = 0; i < res.size() && i < 24; i++) {
    s.aps.push_back({res[i].mac, res[i].rssi, res[i].ch});
    ssids.push_back(res[i].ssid);
  }
  bool ok = P.appendLine(path("samples.jsonl"), sampleToLine(s, &ssids));
  if (s.aps.size() > 12) s.aps.resize(12);
  proj.samples.push_back(s);
  heatDirty = true;
  float v = RSSI_FLOOR;
  sampleValue(proj, s, SRC_BEST, v);
  int lo = linkedObject(proj, s);
  std::string msg = fmt("#%d best %d", (int)proj.samples.size(), (int)v);
  if (lo >= 0) msg += " via " + trunc(proj.objects[lo].name, 10);
  say(ok ? msg : "SD WRITE FAILED");
}

// ------------------------------------------------------------------ touch

bool App::poleTarget(float& x, float& y, float** z) const {
  App* self = const_cast<App*>(this);
  if (mode == M_PLACE && selObj >= 0 && selObj < (int)proj.objects.size()) {
    MapObject& o = self->proj.objects[selObj];
    const Floor* f = curFloor();
    if (!f || o.floor != f->id) return false;
    x = o.x;
    y = o.y;
    *z = &o.z;
    return true;
  }
  if (mode == M_SURVEY && meSet) {
    x = meX;
    y = meY;
    *z = &self->meZ;
    return true;
  }
  return false;
}

bool App::poleHit(int sx, int sy) const {
  float x, y, *z;
  const Floor* f = curFloor();
  if (!f || !poleTarget(x, y, &z)) return false;
  float bx, by, tx_, ty_;
  view.project(x, y, 0, bx, by);
  view.project(x, y, f->height, tx_, ty_);
  return fabsf(sx - bx) <= 18 && sy >= ty_ - 12 && sy <= by + 12;
}

void App::poleSet(int sy) {
  float x, y, *z;
  const Floor* f = curFloor();
  if (!f || !poleTarget(x, y, &z)) return;
  float bx, by;
  view.project(x, y, 0, bx, by);
  *z = clampf(roundf(view.zFromScreen((float)sy, by) * 20) / 20, 0, f->height);
  dirty = true;
}

int App::pickObject(int sx, int sy) const {
  const Floor* f = curFloor();
  int best = -1;
  float bd = 16 * 16;
  for (size_t i = 0; f && i < proj.objects.size(); i++) {
    const MapObject& o = proj.objects[i];
    if (o.floor != f->id) continue;
    float px, py;
    view.project(o.x, o.y, o.z, px, py);
    float d = (px - sx) * (px - sx) + (py - sy) * (py - sy);
    if (d < bd) {
      bd = d;
      best = (int)i;
    }
  }
  return best;
}

int App::pickSample(int sx, int sy) const {
  const Floor* f = curFloor();
  int best = -1;
  float bd = 16 * 16;
  for (size_t i = 0; f && i < proj.samples.size(); i++) {
    const Sample& s = proj.samples[i];
    if (s.floor != f->id) continue;
    float px, py;
    view.project(s.x, s.y, field ? 0 : s.z, px, py);
    float d = (px - sx) * (px - sx) + (py - sy) * (py - sy);
    if (d < bd) {
      bd = d;
      best = (int)i;
    }
  }
  return best;
}

void App::mapTap(int sx, int sy) {
  float wx, wy;
  view.unproject((float)sx, (float)sy, 0, wx, wy);
  bool plan = isoT < 0.5f;
  if (mode == M_PLACE) {
    int hit = pickObject(sx, sy);
    if (hit >= 0) {
      selObj = hit;
      addArmed = false;
    } else if (addArmed) {
      if (!plan) {
        say("Switch to 2D to place");
      } else {
        pendX = wx;
        pendY = wy;
        listItems.clear();
        for (int k = 0; k < K_COUNT; k++) listItems.push_back(kindName((uint8_t)k));
        openList(L_KIND, "What is it?");
      }
    } else {
      selObj = -1;
    }
  } else if (mode == M_SURVEY) {
    if (!plan) {
      say("Use 2D to set your position");
    } else {
      meSet = true;
      meX = wx;
      meY = wy;
      say("Position set. ISO = height");
    }
  } else {
    int s = pickSample(sx, sy);
    if (s >= 0) {
      detail = s;
      screen = S_DETAIL;
    }
  }
  dirty = true;
}

void App::touch(bool down, int x, int y) {
  bool inMap = screen == S_MAP;
  if (down && !tDown) {
    tDown = true;
    tDrag = tPole = false;
    tx0 = tx = x;
    ty0 = ty = y;
    tBtn = hitButton(x, y);
    if (tBtn == B_NONE && inMap && isoT > 0.5f && poleHit(x, y)) {
      tPole = true;
      poleSet(y);
    }
  } else if (down && tDown) {
    int dx = x - tx, dy = y - ty;
    if (tBtn == B_NONE && inMap) {
      if (tPole) {
        poleSet(y);
      } else {
        if (!tDrag && (abs(x - tx0) > 8 || abs(y - ty0) > 8)) tDrag = true;
        if (tDrag && (dx || dy)) {
          if (isoT > 0.5f) {
            isoYaw -= dx * 0.012f;
            isoPitch = clampf(isoPitch + dy * 0.008f, 0.25f, 1.35f);
          } else {
            panX -= dx / view.scale;
            panY -= dy / view.scale;
          }
          updateView();
          dirty = true;
        }
      }
    }
    tx = x;
    ty = y;
  } else if (!down && tDown) {
    tDown = false;
    if (tBtn != B_NONE) {
      if (hitButton(tx, ty) == tBtn) press(tBtn);
    } else if (screen == S_DETAIL) {
      screen = S_MAP;
      dirty = true;
    } else if (inMap) {
      if (tPole) {
        if (mode == M_PLACE) saveProject();
      } else if (!tDrag && ty0 >= TOP_H && ty0 < SCR_H - BOT_H) {
        mapTap(tx, ty);
      }
    }
  }
}

// ------------------------------------------------------------------ drawing

void App::line3(Canvas& c, float x1, float y1, float z1, float x2, float y2, float z2, uint16_t col) const {
  float ax, ay, bx, by;
  view.project(x1, y1, z1, ax, ay);
  view.project(x2, y2, z2, bx, by);
  if (std::max(ay, by) < c.bandY0 - 1 || std::min(ay, by) > c.bandY1 + 1) return;
  if (std::max(ax, bx) < 0 || std::min(ax, bx) > SCR_W) return;
  c.line((int)lroundf(ax), (int)lroundf(ay), (int)lroundf(bx), (int)lroundf(by), col);
}

void App::drawButtons(Canvas& c) {
  std::vector<Btn> b;
  buildButtons(b);
  for (const Btn& k : b) {
    if (k.y + k.h < c.bandY0 || k.y > c.bandY1) continue;
    c.fillRect(k.x, k.y, k.w, k.h, k.on ? C_ON : C_BAR);
    c.drawRect(k.x, k.y, k.w, k.h, C_BTN);
    bool row = k.id >= B_ROW;
    int tw = c.textW(k.label.c_str(), 2);
    int txx = row ? k.x + 8 : k.x + (k.w - tw) / 2;
    c.text(txx, k.y + (k.h - 16) / 2 + 1, k.label.c_str(), k.on ? C_AMBER : C_TEXT, 2);
  }
}

void App::drawHeat(Canvas& c) {
  if (heat.v.empty()) return;
  bool plan = isoT < 0.02f;
  float ox, oy, ux, uy, vx, vy;
  view.project(heat.x0, heat.y0, 0, ox, oy);
  view.project(heat.x0 + heat.cell, heat.y0, 0, ux, uy);
  view.project(heat.x0, heat.y0 + heat.cell, 0, vx, vy);
  ux -= ox;
  uy -= oy;
  vx -= ox;
  vy -= oy;
  for (int j = 0; j < heat.ny; j++) {
    for (int i = 0; i < heat.nx; i++) {
      size_t k = (size_t)j * heat.nx + i;
      int8_t val = heat.v[k];
      if (val == HeatGrid::NONE) continue;
      float ax = ox + i * ux + j * vx, ay = oy + i * uy + j * vy;
      float bx = ax + ux, by = ay + uy, dx = ax + vx, dy = ay + vy, ex = bx + vx, ey = by + vy;
      float lo = std::min(std::min(ay, by), std::min(dy, ey)), hi = std::max(std::max(ay, by), std::max(dy, ey));
      if (hi < c.bandY0 || lo > c.bandY1) continue;
      uint16_t col = heat.categorical ? categoryColor(val) : rssiColor(val);
      col = dim565(col) + (heat.far[k] ? 0 : dim565(dim565(col)));  // 75 % normally, 50 % when far from data
      if (plan) {
        c.fillRect((int)floorf(ax), (int)floorf(ay), (int)ceilf(ex) - (int)floorf(ax), (int)ceilf(ey) - (int)floorf(ay), col);
      } else {
        c.fillTri((int)lroundf(ax), (int)lroundf(ay), (int)lroundf(bx), (int)lroundf(by), (int)lroundf(ex), (int)lroundf(ey), col);
        c.fillTri((int)lroundf(ax), (int)lroundf(ay), (int)lroundf(ex), (int)lroundf(ey), (int)lroundf(dx), (int)lroundf(dy), col);
      }
    }
  }
}

void App::drawMap(Canvas& c) {
  c.fillRect(0, TOP_H, SCR_W, MAP_H, C_BG);
  const Floor* f = curFloor();
  bool plan = isoT < 0.02f;
  if (!f) {
    c.text(20, 100, "No floors yet.", C_TEXT, 2);
    c.text(20, 120, "Add them from HOTSPOT SETUP.", C_MUTED, 2);
  } else {
    float H = f->height;
    Bounds b = floorBounds(*f);
    // 1 m reference grid
    for (float gx = floorf(b.x0); gx <= ceilf(b.x1); gx += 1) line3(c, gx, floorf(b.y0), 0, gx, ceilf(b.y1), 0, C_GRID);
    for (float gy = floorf(b.y0); gy <= ceilf(b.y1); gy += 1) line3(c, floorf(b.x0), gy, 0, ceilf(b.x1), gy, 0, C_GRID);

    if (mode == M_VIEW && field) {
      if (heatDirty) {
        computeHeat(proj, f->id, src, heat);
        heatDirty = false;
      }
      drawHeat(c);
    }

    // other floors as ghost outlines, stacked
    if (isoT > 0.5f) {
      for (int j = 0; j < (int)proj.floors.size(); j++) {
        if (j == floorIdx) continue;
        float zo = 0;
        for (int k = std::min(j, floorIdx); k < std::max(j, floorIdx); k++) zo += proj.floors[k].height;
        if (j < floorIdx) zo = -zo;
        const std::vector<Vec2>& o = proj.floors[j].outline;
        for (size_t i = 0; i < o.size(); i++) {
          const Vec2 &p = o[i], &q = o[(i + 1) % o.size()];
          line3(c, p.x, p.y, zo, q.x, q.y, zo, C_GHOST);
        }
      }
    }

    // active floor wireframe
    for (const Seg& s : f->walls) {
      if (!plan) {
        line3(c, s.x1, s.y1, H, s.x2, s.y2, H, C_WALLDIM);
        line3(c, s.x1, s.y1, 0, s.x1, s.y1, H, C_WALLDIM);
        line3(c, s.x2, s.y2, 0, s.x2, s.y2, H, C_WALLDIM);
      }
      line3(c, s.x1, s.y1, 0, s.x2, s.y2, 0, C_WALL2);
    }
    for (size_t i = 0; i < f->outline.size(); i++) {
      const Vec2 &p = f->outline[i], &q = f->outline[(i + 1) % f->outline.size()];
      if (!plan) {
        line3(c, p.x, p.y, H, q.x, q.y, H, C_WALL2);
        line3(c, p.x, p.y, 0, p.x, p.y, H, C_WALL2);
      }
      line3(c, p.x, p.y, 0, q.x, q.y, 0, C_WALL);
    }

    // survey samples
    if (mode != M_PLACE) {
      bool dots = mode == M_VIEW && !field;
      int s2 = mode == M_VIEW ? src : SRC_BEST;
      for (const Sample& s : proj.samples) {
        if (s.floor != f->id) continue;
        float v, px, py;
        bool has = sampleValue(proj, s, s2, v);
        uint16_t col = !has ? C_MUTED : (s2 == SRC_LINK ? categoryColor((int)v) : rssiColor(v));
        if (dots && !plan) line3(c, s.x, s.y, 0, s.x, s.y, s.z, C_WALLDIM);
        view.project(s.x, s.y, dots ? s.z : 0, px, py);
        if (py < c.bandY0 - 8 || py > c.bandY1 + 8) continue;
        if (dots) {
          c.fillCircle((int)px, (int)py, 5, col);
        } else {
          c.fillCircle((int)px, (int)py, 3, C_BG);
          c.fillCircle((int)px, (int)py, 2, mode == M_VIEW ? C_TEXT : col);
        }
      }
    }

    // placed objects
    for (size_t i = 0; i < proj.objects.size(); i++) {
      const MapObject& o = proj.objects[i];
      if (o.floor != f->id) continue;
      bool sel = (int)i == selObj && mode == M_PLACE;
      bool hot = mode == M_VIEW && (int)i == src;
      bool ap = o.kind == K_ROUTER || o.kind == K_MESH;
      float px, py;
      if (!plan) {
        if (sel) {
          line3(c, o.x, o.y, 0, o.x, o.y, H, C_AMBER);
          for (float t = 0.5f; t < H; t += 0.5f) {
            view.project(o.x, o.y, t, px, py);
            c.line((int)px - 3, (int)py, (int)px + 3, (int)py, C_AMBER);
          }
        } else {
          line3(c, o.x, o.y, 0, o.x, o.y, o.z, C_MUTED);
        }
      }
      view.project(o.x, o.y, o.z, px, py);
      if (py < c.bandY0 - 20 || py > c.bandY1 + 20) continue;
      int ix = (int)px, iy = (int)py;
      if (sel || hot) c.fillCircle(ix, iy, 7, C_AMBER);
      c.fillCircle(ix, iy, 5, o.bssids.empty() ? C_BG : C_OK);
      c.fillCircle(ix, iy, 3, ap ? C_TEXT : C_DEV);
      if (mode == M_PLACE || hot) c.text(ix + 8, iy - 4, trunc(o.name, 12).c_str(), sel || hot ? C_AMBER : C_MUTED, 1);
    }

    // "me" marker
    if (mode == M_SURVEY && meSet) {
      float px, py;
      if (!plan) {
        line3(c, meX, meY, 0, meX, meY, H, C_ME);
        for (float t = 0.5f; t < H; t += 0.5f) {
          view.project(meX, meY, t, px, py);
          c.line((int)px - 3, (int)py, (int)px + 3, (int)py, C_ME);
        }
      }
      view.project(meX, meY, meZ, px, py);
      c.line((int)px - 8, (int)py, (int)px + 8, (int)py, C_ME);
      c.line((int)px, (int)py - 8, (int)px, (int)py + 8, C_ME);
      c.fillCircle((int)px, (int)py, 3, C_ME);
    }

    // legend
    if (mode == M_VIEW) {
      if (src == SRC_LINK) {
        int row = 0;
        for (size_t i = 0; i < proj.objects.size() && row < 5; i++) {
          if (proj.objects[i].bssids.empty()) continue;
          c.fillRect(SCR_W - 84, TOP_H + 4 + row * 11, 8, 8, categoryColor((int)i));
          c.text(SCR_W - 73, TOP_H + 4 + row * 11, trunc(proj.objects[i].name, 11).c_str(), C_TEXT, 1);
          row++;
        }
      } else {
        for (int i = 0; i < 44; i++) c.fillRect(SCR_W - 10, TOP_H + 6 + i, 6, 1, rssiColor(-48 - i));
        c.text(SCR_W - 30, TOP_H + 4, "-48", C_TEXT, 1);
        c.text(SCR_W - 30, TOP_H + 44, "-92", C_TEXT, 1);
      }
    }
  }

  // chrome
  c.fillRect(0, 0, SCR_W, TOP_H, C_BAR);
  c.fillRect(0, SCR_H - BOT_H, SCR_W, BOT_H, C_BAR);
  drawButtons(c);
  std::string info;
  uint16_t ic = C_MUTED;
  if (!toast.empty()) {
    info = toast;
    ic = C_AMBER;
  } else if (mode == M_PLACE && selObj >= 0 && selObj < (int)proj.objects.size()) {
    const MapObject& o = proj.objects[selObj];
    info = fmt("%s z%.2f %s", trunc(o.name, 10).c_str(), o.z, o.bssids.empty() ? "" : "bound");
    ic = C_TEXT;
  } else if (mode == M_SURVEY) {
    info = meSet ? fmt("z%.2fm  %d samples", meZ, (int)proj.samples.size()) : fmt("%d samples", (int)proj.samples.size());
  } else {
    info = trunc(proj.name, 22);
  }
  if (toast.size() > 22) {  // long toasts get their own strip under the top bar
    c.fillRect(0, TOP_H, SCR_W, 14, C_BAR);
    c.text(4, TOP_H + 3, toast.c_str(), C_AMBER, 1);
  } else {
    c.text(142, 6, trunc(info, 22).c_str(), ic, 1);
  }
}

void App::drawHome(Canvas& c) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BG);
  c.text(8, 3, "CYD WI-FI MAPPER", C_WALL, 2);
  c.text(200, 8, "scan sets", C_MUTED, 1);
  if (!P.storageOk()) {
    c.text(20, 90, "No SD card found.", C_AMBER, 2);
    c.text(20, 112, "Insert a FAT32 card and reset.", C_MUTED, 2);
  } else if (slugs.empty()) {
    c.text(20, 80, "No scan sets yet.", C_TEXT, 2);
    c.text(20, 104, "Tap HOTSPOT SETUP, join the CYD", C_MUTED, 2);
    c.text(20, 122, "Wi-Fi and add your floor plans.", C_MUTED, 2);
  }
  drawButtons(c);
  if (!toast.empty()) c.text(8, SCR_H - BOT_H - 12, toast.c_str(), C_AMBER, 1);
}

void App::drawHotspot(Canvas& c) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BG);
  c.text(8, 3, "HOTSPOT SETUP", C_WALL, 2);
  for (int i = 0; i < 8; i++) {
    std::string l = P.hotspotLine(i);
    if (l.empty()) break;
    c.text(14, 34 + i * 20, l.c_str(), i % 2 ? C_TEXT : C_MUTED, 2);
  }
  drawButtons(c);
}

void App::drawList(Canvas& c) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BG);
  c.text(8, 3, listTitle.c_str(), C_WALL, 2);
  drawButtons(c);
}

void App::drawDetail(Canvas& c) {
  c.fillRect(0, 0, SCR_W, SCR_H, C_BG);
  if (detail < 0 || detail >= (int)proj.samples.size()) return;
  const Sample& s = proj.samples[detail];
  c.text(8, 3, fmt("SAMPLE %d   %.1f, %.1f  z%.1f m", detail + 1, s.x, s.y, s.z).c_str(), C_WALL, 2);
  int link = linkedObject(proj, s), y = 28, rows = 0;
  auto bar = [&](const std::string& name, float v, bool linked) {
    c.text(8, y, trunc(name, 13).c_str(), linked ? C_AMBER : C_TEXT, 2);
    int w = (int)clampf((v + 95) * 3.0f, 2, 150);
    c.fillRect(112, y + 3, w, 10, rssiColor(v));
    c.text(270, y, v <= RSSI_FLOOR ? "none" : fmt("%d", (int)v).c_str(), C_TEXT, 2);
    y += 20;
    rows++;
  };
  for (size_t i = 0; i < proj.objects.size() && rows < 8; i++) {
    float v;
    if (sampleValue(proj, s, (int)i, v)) bar(proj.objects[i].name, v, (int)i == link);
  }
  if (rows == 0)
    for (size_t i = 0; i < s.aps.size() && rows < 8; i++) bar(macToStr(s.aps[i].mac).substr(6), s.aps[i].rssi, false);
  std::string foot = !s.hasConn ? "Not joined to home Wi-Fi for this sample"
                                : fmt("Joined via %s at %d dBm", link >= 0 ? proj.objects[link].name.c_str() : macToStr(s.conn).c_str(), s.connRssi);
  c.text(8, 204, foot.c_str(), C_AMBER, 1);
  c.text(8, 222, "Tap anywhere to close", C_MUTED, 1);
}

void App::draw(Canvas& c) {
  switch (screen) {
    case S_HOME: drawHome(c); break;
    case S_HOTSPOT: drawHotspot(c); break;
    case S_LIST: drawList(c); break;
    case S_DETAIL: drawDetail(c); break;
    default: drawMap(c); break;
  }
  if (pendingScan) {
    c.fillRect(60, 92, 200, 50, C_BAR);
    c.drawRect(60, 92, 200, 50, C_AMBER);
    c.text(92, 100, "Scanning Wi-Fi...", C_AMBER, 2);
    c.text(104, 122, "hold still (~8 s)", C_MUTED, 1);
    if (c.bandY1 >= SCR_H - 1) overlayShown = true;
  }
}

}  // namespace wm
