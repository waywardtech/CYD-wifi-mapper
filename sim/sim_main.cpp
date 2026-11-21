// Desktop simulator: runs the real UI code against a fake house and radio, and dumps
// draw commands that sim/render.py turns into PNG screenshots.  Build + run: sim/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include "wm_host.h"

using namespace wm;

static const char* HOUSE =
    "{\"v\":1,\"name\":\"Demo House\",\"floors\":["
    "{\"id\":1,\"name\":\"Ground floor\",\"level\":0,\"height\":2.5,"
    "\"outline\":[[0,0],[12,0],[12,6],[8,6],[8,9],[0,9]],"
    "\"walls\":[[5,0,5,3],[0,4,3.5,4],[5,4,5,3.9],[8,0,8,2.5],[8,3.5,8,6],[5,5.5,5,9],[5,5.5,6.5,5.5]]},"
    "{\"id\":2,\"name\":\"Basement\",\"level\":-1,\"height\":2.3,"
    "\"outline\":[[0,0],[12,0],[12,6],[0,6]],\"walls\":[[6,0,6,4],[6,5,6,6],[9,3,12,3]]}],"
    "\"objects\":["
    "{\"name\":\"Router 1\",\"kind\":0,\"floor\":1,\"x\":1.0,\"y\":8.3,\"z\":0.5,\"bssids\":[\"aa:00:00:00:00:01\"]},"
    "{\"name\":\"Mesh node 1\",\"kind\":1,\"floor\":1,\"x\":11.2,\"y\":1.0,\"z\":1.0,\"bssids\":[\"aa:00:00:00:00:02\"]},"
    "{\"name\":\"Mesh node 2\",\"kind\":1,\"floor\":2,\"x\":9.0,\"y\":4.5,\"z\":0.8,\"bssids\":[\"aa:00:00:00:00:03\"]},"
    "{\"name\":\"TV 1\",\"kind\":4,\"floor\":1,\"x\":1.6,\"y\":8.6,\"z\":1.1,\"bssids\":[]},"
    "{\"name\":\"Speaker 1\",\"kind\":3,\"floor\":1,\"x\":6.5,\"y\":0.4,\"z\":0.9,\"bssids\":[]}]}";

static HostPlatform hp;
static App app(hp);
static int shotNo = 0;

static void shot(const char* name) {
  char fn[128];
  snprintf(fn, sizeof(fn), "sim/out/%02d_%s.txt", ++shotNo, name);
  RecCanvas rc;
  rc.f = fopen(fn, "w");
  if (!rc.f) { perror(fn); exit(1); }
  for (int b = 0; b < 4; b++) {  // same 4-band pass the firmware does
    rc.bandY0 = b * 60;
    rc.bandY1 = b * 60 + 59;
    fprintf(rc.f, "B %d %d\n", rc.bandY0, rc.bandY1);  // renderer clips to the band, like the sprite does
    app.draw(rc);
  }
  fclose(rc.f);
}

static void settle() { for (int i = 0; i < 4; i++) { hp.now += 200; app.tick(); } }
static void tap(int x, int y) { app.touch(true, x, y); app.touch(true, x, y); app.touch(false, x, y); settle(); }
static void drag(int x0, int y0, int x1, int y1) {
  app.touch(true, x0, y0);
  for (int i = 1; i <= 8; i++) app.touch(true, x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8);
  app.touch(false, x1, y1);
  settle();
}

int main() {
  hp.files["/wifimap/demo-house/project.json"] = HOUSE;
  hp.files["/wifimap/office-router-test/project.json"] =
      "{\"v\":1,\"name\":\"Office\",\"floors\":[{\"id\":1,\"name\":\"Office\",\"level\":0,\"height\":2.5,\"outline\":[[0,0],[4,0],[4,3],[0,3]],\"walls\":[]}],\"objects\":[]}";
  srand(7);
  hp.scanFn = [](std::vector<ScanEntry>& out, bool& hc, Mac& c, int8_t& r) {
    const Floor* f = app.curFloor();
    float best = -200;
    for (const MapObject& o : app.proj.objects)
      for (const Mac& m : o.bssids) {
        float v = fakeRssi(app.proj, o, f->id, app.meX, app.meY, app.meZ) + (rand() % 5 - 2);
        if (v < -93) continue;
        out.push_back({m, (int8_t)v, 6, "HomeNet"});
        if (v > best) { best = v; c = m; }
      }
    for (int i = 0; i < 9; i++) {  // neighbours
      Mac m; m.b[0] = 0x5c; m.b[5] = (uint8_t)i;
      out.push_back({m, (int8_t)(-70 - rand() % 22), (uint8_t)(1 + i), i % 2 ? "NETGEAR-" : "xfinitywifi"});
    }
    std::sort(out.begin(), out.end(), [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
    hc = best > -200;
    r = (int8_t)best;
    return true;
  };

  app.begin();
  shot("home");
  app.press(B_HOTSPOT);
  shot("hotspot");
  app.press(B_DONE);
  tap(100, 35);  // open demo-house
  app.mode = M_PLACE;
  app.iso = false; settle();
  shot("place_2d");
  // add a console through the real touch flow
  tap(90, 225);
  float sx, sy;
  app.view.project(2.6f, 8.5f, 0, sx, sy);
  tap((int)sx, (int)sy);
  shot("place_pick_kind");
  tap(100, 24 + 5 * 30 + 10);
  app.iso = true; settle();
  shot("place_iso_height");
  app.selObj = 1;
  app.press(B_BIND);
  shot("scanning_overlay");
  app.tick();
  shot("bind_list");
  app.press(B_CANCEL);

  // survey both floors on a rough walking grid
  app.mode = M_SURVEY;
  for (int fl = 0; fl < 2; fl++) {
    const Floor* f = app.curFloor();
    Bounds b = floorBounds(*f);
    for (float y = b.y0 + 0.9f; y < b.y1; y += 1.7f)
      for (float x = b.x0 + 0.9f; x < b.x1; x += 1.9f) {
        if (!pointInPoly(f->outline, x, y)) continue;
        app.meSet = true; app.meX = x + (rand() % 7 - 3) * 0.1f; app.meY = y + (rand() % 7 - 3) * 0.1f; app.meZ = 1.2f;
        app.pendingScan = 1; app.overlayShown = true; app.tick();
      }
    if (fl == 0) { settle(); settle(); shot("survey_iso"); app.iso = false; settle(); shot("survey_2d"); app.iso = true; settle(); }
    app.press(B_FLOOR);
  }
  settle(); settle(); settle();

  app.mode = M_VIEW; app.heatDirty = true;
  shot("view_field_iso_best");
  drag(120, 120, 200, 95);
  shot("view_field_iso_rotated");
  app.iso = false; settle();
  shot("view_field_2d_best");
  app.press(B_SRC);  // Router 1
  shot("view_field_2d_router");
  app.field = false; app.iso = true; settle();
  shot("view_dots_iso_router");
  app.field = true;
  app.src = SRC_LINK; app.heatDirty = true;
  shot("view_mesh_split_iso");
  app.iso = false; settle();
  shot("view_mesh_split_2d");
  app.view.project(app.proj.samples[9].x, app.proj.samples[9].y, 0, sx, sy);
  tap((int)sx, (int)sy);
  shot("sample_detail");
  tap(10, 10);
  app.src = SRC_BEST; app.heatDirty = true; app.iso = true; settle();
  app.press(B_FLOOR);
  shot("view_basement_iso");
  // the surveyed demo house, for tools/readme_images.py
  FILE* f = fopen("sim/out/demo_project.json", "w");
  if (f) { fputs(hp.files["/wifimap/demo-house/project.json"].c_str(), f); fclose(f); }
  f = fopen("sim/out/demo_samples.jsonl", "w");
  if (f) { fputs(hp.files["/wifimap/demo-house/samples.jsonl"].c_str(), f); fclose(f); }
  printf("%d screenshots, %d samples\n", shotNo, (int)app.proj.samples.size());
  return 0;
}
