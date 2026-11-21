// CYD Wi-Fi Mapper — touch UI and scene renderer (portable; talks to hardware via Platform/Canvas).
#pragma once
#include "wm_core.h"

namespace wm {

enum Mode : uint8_t { M_PLACE, M_SURVEY, M_VIEW };
enum Screen : uint8_t { S_HOME, S_MAP, S_HOTSPOT, S_LIST, S_DETAIL };
enum ListPurpose : uint8_t { L_NONE, L_KIND, L_BIND };

// Screen geometry (landscape 320x240)
enum { SCR_W = 320, SCR_H = 240, TOP_H = 20, BOT_H = 30, MAP_H = SCR_H - TOP_H - BOT_H };

enum BtnId {
  B_NONE = -1, B_HOME, B_FLOOR, B_ISO, B_MODE, B_ADD, B_BIND, B_DEL, B_ZOUT, B_ZIN, B_SCAN, B_UNDO,
  B_SRC, B_FIELD, B_HOTSPOT, B_DONE, B_PREV, B_NEXT, B_CANCEL, B_ROW  // B_ROW + n = list row n
};

struct Btn {
  int x, y, w, h, id;
  std::string label;
  bool on;
};

struct App {
  explicit App(Platform& p) : P(p) {}
  void begin();
  void touch(bool down, int x, int y);  // call every loop with the current touch state
  void tick();                          // animations, toasts, pending scans
  void draw(Canvas& c);                 // may be called once per band
  bool dirty = true;

  // --- state (public so the simulator and tests can drive and inspect it) ---
  Platform& P;
  Screen screen = S_HOME;
  Mode mode = M_PLACE;
  std::vector<std::string> slugs;
  int page = 0;  // paging for home + list screens
  std::string slug;
  Project proj;
  int floorIdx = 0;

  View view;
  bool iso = true;      // target view; isoT animates towards it
  float isoT = 1;       // 0 = plan, 1 = isometric
  float isoYaw = 0.6f, isoPitch = 0.62f, zoom = 1, panX = 0, panY = 0;

  int selObj = -1;
  bool addArmed = false;
  float pendX = 0, pendY = 0;
  bool meSet = false;
  float meX = 0, meY = 0, meZ = 1.2f;

  int src = SRC_BEST;
  bool field = true;    // VIEW: interpolated field vs. dots only
  HeatGrid heat;
  bool heatDirty = true;

  std::string toast;
  uint32_t toastUntil = 0;

  ListPurpose listPurpose = L_NONE;
  std::string listTitle;
  std::vector<std::string> listItems;
  std::vector<ScanEntry> scanList;
  int pendingScan = 0;  // 1 = survey sample, 2 = bind
  bool overlayShown = false;
  int detail = -1;

  // --- operations ---
  bool openProject(const std::string& s);
  void saveProject();
  const Floor* curFloor() const;
  void updateView();
  void say(const std::string& msg);
  void buildButtons(std::vector<Btn>& out) const;
  void press(int id);
  std::vector<int> sources() const;
  std::string sourceName(int s) const;
  void doScan();

 private:
  bool tDown = false, tDrag = false, tPole = false;
  int tx0 = 0, ty0 = 0, tx = 0, ty = 0, tBtn = B_NONE;
  uint32_t lastTick = 0;

  int hitButton(int x, int y) const;
  void mapTap(int x, int y);
  int pickObject(int x, int y) const;
  int pickSample(int x, int y) const;
  bool poleTarget(float& x, float& y, float** z) const;
  bool poleHit(int x, int y) const;
  void poleSet(int y);
  void openList(ListPurpose p, const std::string& title);
  void listPick(int idx);
  std::string path(const char* file) const;

  void drawMap(Canvas& c);
  void drawHeat(Canvas& c);
  void drawButtons(Canvas& c);
  void drawHome(Canvas& c);
  void drawHotspot(Canvas& c);
  void drawList(Canvas& c);
  void drawDetail(Canvas& c);
  void line3(Canvas& c, float x1, float y1, float z1, float x2, float y2, float z2, uint16_t col) const;
};

}  // namespace wm
