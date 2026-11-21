// Host unit tests for the portable core:  pio test -e native
#include <string.h>
#include <unity.h>
#include "wm_host.h"

using namespace wm;

static const char* DEMO =
    "{\"v\":1,\"name\":\"Test House\",\"floors\":["
    "{\"id\":2,\"name\":\"Ground\",\"level\":0,\"height\":2.5,\"outline\":[[0,0],[10,0],[10,8],[0,8]],\"walls\":[[5,0,5,5]]},"
    "{\"id\":1,\"name\":\"Basement\",\"level\":-1,\"height\":2.2,\"outline\":[[0,0],[10,0],[10,8],[0,8]],\"walls\":[]}],"
    "\"objects\":[{\"name\":\"Router 1\",\"kind\":0,\"floor\":2,\"x\":1,\"y\":1,\"z\":0.9,\"bssids\":[\"aa:00:00:00:00:01\"]},"
    "{\"name\":\"Mesh node 1\",\"kind\":1,\"floor\":2,\"x\":9,\"y\":7,\"z\":1,\"bssids\":[\"AA0000000002\"]}]}";

void setUp() {}
void tearDown() {}

static void test_mac() {
  Mac m;
  TEST_ASSERT_TRUE(macFromStr("AA:bb:0c:1D:2e:3f", m));
  TEST_ASSERT_EQUAL_STRING("aa:bb:0c:1d:2e:3f", macToStr(m).c_str());
  TEST_ASSERT_TRUE(macFromStr("aabb0c1d2e3f", m));
  TEST_ASSERT_FALSE(macFromStr("aa:bb:cc", m));
  TEST_ASSERT_FALSE(macFromStr("zz:bb:cc:dd:ee:ff", m));
}

static void test_project_json_roundtrip() {
  Project p;
  TEST_ASSERT_TRUE(projectFromJson(DEMO, p));
  TEST_ASSERT_EQUAL(2, (int)p.floors.size());
  TEST_ASSERT_EQUAL_STRING("Basement", p.floors[0].name.c_str());  // sorted by level
  TEST_ASSERT_EQUAL(1, p.floorIndex(2));
  TEST_ASSERT_EQUAL(2, (int)p.objects.size());
  TEST_ASSERT_EQUAL(1, (int)p.objects[1].bssids.size());
  Project q;
  TEST_ASSERT_TRUE(projectFromJson(projectToJson(p), q));
  TEST_ASSERT_EQUAL_STRING(projectToJson(p).c_str(), projectToJson(q).c_str());
  TEST_ASSERT_FALSE(projectFromJson("{not json", q));
}

static void test_sample_lines_and_undo() {
  Sample s;
  s.floor = 2;
  s.x = 3.25f;
  s.y = 4.5f;
  s.z = 1.2f;
  s.t = 123;
  s.hasConn = true;
  macFromStr("aa:00:00:00:00:02", s.conn);
  s.connRssi = -61;
  for (int i = 0; i < 20; i++) {
    ApReading r;
    r.mac.b[5] = (uint8_t)i;
    r.rssi = (int8_t)(-40 - i);
    r.ch = 6;
    s.aps.push_back(r);
  }
  std::vector<Sample> v;
  TEST_ASSERT_TRUE(applySampleLine(sampleToLine(s), v));
  TEST_ASSERT_TRUE(applySampleLine(sampleToLine(s), v));
  TEST_ASSERT_EQUAL(2, (int)v.size());
  TEST_ASSERT_EQUAL(12, (int)v[0].aps.size());  // capped in RAM
  TEST_ASSERT_EQUAL(-61, v[0].connRssi);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 3.25, v[0].x);
  TEST_ASSERT_TRUE(applySampleLine(UNDO_LINE, v));
  TEST_ASSERT_EQUAL(1, (int)v.size());
  TEST_ASSERT_FALSE(applySampleLine("garbage", v));
}

static void test_view_roundtrip() {
  View v;
  v.yaw = 0.6f;
  v.pitch = 0.62f;
  v.scale = 17;
  v.cx = 4;
  v.cy = 3;
  v.cz = 1.2f;
  float sx, sy, x, y;
  v.project(7.5f, 2.25f, 0, sx, sy);
  v.unproject(sx, sy, 0, x, y);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 7.5, x);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 2.25, y);
  float bx, by, tx, ty;
  v.project(1, 1, 0, bx, by);
  v.project(1, 1, 1.7f, tx, ty);
  TEST_ASSERT_FLOAT_WITHIN(0.01, bx, tx);  // vertical poles stay vertical on screen
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1.7, v.zFromScreen(ty, by));
  View plan;  // straight down
  plan.project(plan.cx + 1, plan.cy, 5, sx, sy);
  TEST_ASSERT_FLOAT_WITHIN(0.01, plan.sx0 + plan.scale, sx);
  TEST_ASSERT_FLOAT_WITHIN(0.01, plan.sy0, sy);  // height has no effect in plan view
}

static void test_point_in_poly() {
  std::vector<Vec2> L = {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
  TEST_ASSERT_TRUE(pointInPoly(L, 1, 3));
  TEST_ASSERT_TRUE(pointInPoly(L, 3, 1));
  TEST_ASSERT_FALSE(pointInPoly(L, 3, 3));
  TEST_ASSERT_FALSE(pointInPoly(L, -1, 1));
}

static Sample mk(int floor, float x, float y, const char* m1, int r1, const char* m2, int r2, const char* conn) {
  Sample s;
  s.floor = floor;
  s.x = x;
  s.y = y;
  ApReading a;
  macFromStr(m1, a.mac);
  a.rssi = (int8_t)r1;
  a.ch = 1;
  s.aps.push_back(a);
  if (m2) {
    macFromStr(m2, a.mac);
    a.rssi = (int8_t)r2;
    s.aps.push_back(a);
  }
  if (conn) {
    s.hasConn = macFromStr(conn, s.conn);
    s.connRssi = -55;
  }
  return s;
}

static void test_sources() {
  Project p;
  projectFromJson(DEMO, p);
  Sample s = mk(2, 2, 2, "aa:00:00:00:00:01", -45, "ff:ff:ff:00:00:09", -30, "aa:00:00:00:00:02");
  float v;
  TEST_ASSERT_TRUE(sampleValue(p, s, SRC_BEST, v));
  TEST_ASSERT_EQUAL(-45, (int)v);  // the neighbour's louder AP is ignored once sources are bound
  TEST_ASSERT_TRUE(sampleValue(p, s, 1, v));
  TEST_ASSERT_EQUAL((int)RSSI_FLOOR, (int)v);  // known AP not heard = dead zone
  TEST_ASSERT_EQUAL(1, linkedObject(p, s));
  TEST_ASSERT_TRUE(sampleValue(p, s, SRC_LINK, v));
  TEST_ASSERT_EQUAL(1, (int)v);
  p.objects[0].bssids.clear();
  p.objects[1].bssids.clear();
  TEST_ASSERT_TRUE(sampleValue(p, s, SRC_BEST, v));
  TEST_ASSERT_EQUAL(-30, (int)v);  // nothing bound yet: strongest of anything
  TEST_ASSERT_FALSE(sampleValue(p, s, 0, v));
}

static void test_heat() {
  Project p;
  projectFromJson(DEMO, p);
  p.floors[1].outline = {{0, 0}, {10, 0}, {10, 4}, {5, 4}, {5, 8}, {0, 8}};  // L-shape
  p.samples.push_back(mk(2, 1, 1, "aa:00:00:00:00:01", -40, nullptr, 0, nullptr));
  p.samples.push_back(mk(2, 9, 1, "aa:00:00:00:00:01", -80, nullptr, 0, nullptr));
  p.samples.push_back(mk(1, 5, 5, "aa:00:00:00:00:01", -20, nullptr, 0, nullptr));  // other floor: ignored
  HeatGrid g;
  computeHeat(p, 2, 0, g);
  TEST_ASSERT_TRUE(g.nx > 0 && g.ny > 0);
  auto at = [&g](float x, float y) { return (int)g.v[(size_t)((y - g.y0) / g.cell) * g.nx + (size_t)((x - g.x0) / g.cell)]; };
  TEST_ASSERT_INT_WITHIN(2, -40, at(1.05f, 1.05f));
  TEST_ASSERT_INT_WITHIN(2, -80, at(8.95f, 1.05f));
  int mid = at(5, 1);
  TEST_ASSERT_TRUE(mid < -45 && mid > -75);
  TEST_ASSERT_EQUAL((int)HeatGrid::NONE, at(8, 6));  // outside the L
  HeatGrid e;
  computeHeat(p, 99, 0, e);
  TEST_ASSERT_EQUAL(0, (int)e.v.size());
}

// Drive the UI end to end with synthetic touches.
static void tap(App& a, HostPlatform& hp, int x, int y) {
  a.touch(true, x, y);
  a.touch(true, x, y);
  a.touch(false, x, y);
  hp.now += 300;
  a.tick();
}

static void test_app_flow() {
  HostPlatform hp;
  hp.files["/wifimap/test-house/project.json"] = DEMO;
  App app(hp);
  hp.scanFn = [&](std::vector<ScanEntry>& out, bool& hc, Mac& c, int8_t& r) {
    for (const MapObject& o : app.proj.objects)
      for (const Mac& m : o.bssids)
        out.push_back({m, (int8_t)fakeRssi(app.proj, o, app.curFloor()->id, app.meX, app.meY, app.meZ), 6, "HomeNet"});
    hc = true;
    macFromStr("aa:00:00:00:00:01", c);
    r = -50;
    return true;
  };
  app.begin();
  TEST_ASSERT_EQUAL(1, (int)app.slugs.size());
  tap(app, hp, 100, 35);  // first project row
  TEST_ASSERT_EQUAL(S_MAP, app.screen);
  TEST_ASSERT_EQUAL_STRING("Ground", app.curFloor()->name.c_str());
  TEST_ASSERT_EQUAL(M_SURVEY, app.mode);  // objects exist, no samples yet

  // go to PLACE (SURVEY -> VIEW -> PLACE), switch to 2D, add a TV
  tap(app, hp, 30, 225);
  tap(app, hp, 30, 225);
  TEST_ASSERT_EQUAL(M_PLACE, app.mode);
  tap(app, hp, 300, 10);
  hp.now += 400;
  app.tick();
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0, app.isoT);
  tap(app, hp, 90, 225);  // ADD
  TEST_ASSERT_TRUE(app.addArmed);
  float sx, sy;
  app.view.project(7.0f, 3.0f, 0, sx, sy);
  tap(app, hp, (int)sx, (int)sy);
  TEST_ASSERT_EQUAL(S_LIST, app.screen);
  tap(app, hp, 100, 24 + 4 * 30 + 10);  // row 4 = TV
  TEST_ASSERT_EQUAL(3, (int)app.proj.objects.size());
  TEST_ASSERT_EQUAL(K_TV, app.proj.objects[2].kind);
  TEST_ASSERT_FLOAT_WITHIN(0.15, 7.0, app.proj.objects[2].x);
  TEST_ASSERT_FLOAT_WITHIN(0.15, 3.0, app.proj.objects[2].y);
  Project disk;
  TEST_ASSERT_TRUE(projectFromJson(hp.files["/wifimap/test-house/project.json"], disk));
  TEST_ASSERT_EQUAL(3, (int)disk.objects.size());  // saved to storage

  // ISO: drag the selected object's pole to change height
  tap(app, hp, 300, 10);
  hp.now += 400;
  app.tick();
  float bx, by, tx, ty;
  app.view.project(app.proj.objects[2].x, app.proj.objects[2].y, 0, bx, by);
  app.view.project(app.proj.objects[2].x, app.proj.objects[2].y, 2.0f, tx, ty);
  tap(app, hp, (int)bx, (int)ty);
  TEST_ASSERT_FLOAT_WITHIN(0.1, 2.0, app.proj.objects[2].z);

  // SURVEY: set position in 2D, scan
  tap(app, hp, 30, 225);
  TEST_ASSERT_EQUAL(M_SURVEY, app.mode);
  tap(app, hp, 300, 10);
  hp.now += 400;
  app.tick();
  app.view.project(2.0f, 2.0f, 0, sx, sy);
  tap(app, hp, (int)sx, (int)sy);
  TEST_ASSERT_TRUE(app.meSet);
  tap(app, hp, 120, 225);  // SCAN HERE
  TEST_ASSERT_EQUAL(1, app.pendingScan);
  RecCanvas rc;
  app.draw(rc);  // overlay must be shown before the blocking scan runs
  app.tick();
  TEST_ASSERT_EQUAL(1, (int)app.proj.samples.size());
  TEST_ASSERT_TRUE(app.proj.samples[0].aps[0].rssi > -60);
  TEST_ASSERT_TRUE(hp.files["/wifimap/test-house/samples.jsonl"].find("HomeNet") != std::string::npos);

  // reopen from storage: sample persists; undo removes it durably
  App again(hp);
  again.begin();
  TEST_ASSERT_TRUE(again.openProject("test-house"));
  TEST_ASSERT_EQUAL(1, (int)again.proj.samples.size());
  TEST_ASSERT_EQUAL(M_VIEW, again.mode);
  again.mode = M_SURVEY;
  again.press(B_UNDO);
  App third(hp);
  TEST_ASSERT_TRUE(third.openProject("test-house"));
  TEST_ASSERT_EQUAL(0, (int)third.proj.samples.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mac);
  RUN_TEST(test_project_json_roundtrip);
  RUN_TEST(test_sample_lines_and_undo);
  RUN_TEST(test_view_roundtrip);
  RUN_TEST(test_point_in_poly);
  RUN_TEST(test_sources);
  RUN_TEST(test_heat);
  RUN_TEST(test_app_flow);
  return UNITY_END();
}
