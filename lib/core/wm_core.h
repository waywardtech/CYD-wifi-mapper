// CYD Wi-Fi Mapper — portable core: data model, projection, heat field, JSON.
// No Arduino dependencies in this file; it builds on the host for tests and the simulator.
#pragma once
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace wm {

// ---------- basic types ----------
struct Vec2 { float x, y; };
struct Seg { float x1, y1, x2, y2; };
struct Bounds { float x0, y0, x1, y1; };

struct Mac {
  uint8_t b[6] = {0, 0, 0, 0, 0, 0};
  bool operator==(const Mac& o) const;
  bool zero() const;
};
std::string macToStr(const Mac& m);            // "aa:bb:cc:dd:ee:ff"
bool macFromStr(const char* s, Mac& out);      // accepts ':' '-' or no separators

// ---------- project model (all lengths in metres) ----------
struct Floor {
  int id = 0;               // stable id referenced by objects and samples
  std::string name;
  int level = 0;            // -1 basement, 0 ground, 1 first ...
  float height = 2.5f;      // wall height
  std::vector<Vec2> outline;  // closed polygon, may be empty
  std::vector<Seg> walls;     // interior walls
};

enum Kind : uint8_t { K_ROUTER, K_MESH, K_SWITCH, K_SPEAKER, K_TV, K_CONSOLE, K_OTHER, K_COUNT };
const char* kindName(uint8_t k);
float kindDefaultZ(uint8_t k);

struct MapObject {
  std::string name;
  uint8_t kind = K_OTHER;
  int floor = 0;            // Floor::id
  float x = 0, y = 0, z = 1;
  std::vector<Mac> bssids;  // radios this object transmits on (2.4 GHz ones are what the CYD can hear)
};

struct ApReading { Mac mac; int8_t rssi; uint8_t ch; };

struct Sample {
  int floor = 0;            // Floor::id
  float x = 0, y = 0, z = 1.2f;
  uint32_t t = 0;           // unix time if known, else 0
  bool hasConn = false;     // was the CYD joined to the home network for this sample
  Mac conn;                 // BSSID it associated with
  int8_t connRssi = 0;
  std::vector<ApReading> aps;  // strongest first
};

struct ScanEntry { Mac mac; int8_t rssi; uint8_t ch; std::string ssid; };

struct Project {
  std::string name;
  std::vector<Floor> floors;      // kept sorted by level
  std::vector<MapObject> objects;
  std::vector<Sample> samples;
  int floorIndex(int id) const;   // -1 if missing
};

// ---------- JSON ----------
bool projectFromJson(const std::string& json, Project& out);  // does not touch samples
std::string projectToJson(const Project& p);
// One survey sample per line in samples.jsonl. `ssids` (optional) is parallel to s.aps.
std::string sampleToLine(const Sample& s, const std::vector<std::string>* ssids = nullptr);
// Applies one line to the sample list. Handles the {"undo":1} tombstone. Keeps at most maxAps readings.
bool applySampleLine(const std::string& line, std::vector<Sample>& samples, size_t maxAps = 12);
extern const char* const UNDO_LINE;

// ---------- view / projection ----------
// Orthographic "pseudo isometric" camera. pitch = pi/2 is a straight-down plan view.
struct View {
  float yaw = 0, pitch = 1.5707963f, scale = 20;  // scale = pixels per metre
  float cx = 0, cy = 0, cz = 0;                   // world point shown at the screen centre
  float sx0 = 160, sy0 = 115;                     // screen centre
  void project(float x, float y, float z, float& sx, float& sy) const;
  void unproject(float sx, float sy, float z, float& x, float& y) const;  // onto plane at height z
  float zFromScreen(float sy, float baseSy) const;  // height above a floor point drawn at baseSy
};

// ---------- geometry / colour ----------
bool pointInPoly(const std::vector<Vec2>& poly, float x, float y);
Bounds floorBounds(const Floor& f);
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t dim565(uint16_t c);           // half brightness
uint16_t rssiColor(float rssi);        // red (weak) -> yellow -> green (strong)
uint16_t categoryColor(int idx);       // idx < 0 = unknown (grey)

// ---------- signal sources ----------
enum { SRC_BEST = -1, SRC_CONN = -2, SRC_LINK = -3 };  // >= 0 is an object index
const float RSSI_FLOOR = -95.0f;  // value used when a known AP was not heard at all
int linkedObject(const Project& p, const Sample& s);  // object the CYD associated with, or -1
// Signal value of a sample for a source. Returns false if the sample says nothing about it.
bool sampleValue(const Project& p, const Sample& s, int src, float& v);

struct HeatGrid {
  enum { NONE = -128 };
  int nx = 0, ny = 0;
  float x0 = 0, y0 = 0, cell = 0.25f;
  bool categorical = false;        // values are object indexes (100 = unknown) instead of dBm
  std::vector<int8_t> v;           // NONE = outside the floor / no data
  std::vector<uint8_t> far;        // 1 = more than 4 m from the nearest sample (low confidence)
};
// Inverse-distance-weighted estimate across the floor, masked to the floor outline.
void computeHeat(const Project& p, int floorId, int src, HeatGrid& out);

// ---------- platform services the app needs ----------
struct Platform {
  virtual ~Platform() {}
  virtual uint32_t millis() = 0;
  virtual uint32_t epoch() = 0;  // 0 if the clock is not set
  virtual bool storageOk() = 0;
  virtual std::vector<std::string> listProjects() = 0;  // folder names under /wifimap
  virtual bool readFile(const std::string& path, std::string& out) = 0;
  virtual bool writeFile(const std::string& path, const std::string& data) = 0;
  virtual bool appendLine(const std::string& path, const std::string& line) = 0;
  virtual bool readLines(const std::string& path, const std::function<void(const std::string&)>& fn) = 0;
  // Blocking Wi-Fi survey. Results strongest first.
  virtual bool scan(std::vector<ScanEntry>& out, bool& hasConn, Mac& conn, int8_t& connRssi) = 0;
  virtual void hotspot(bool on) = 0;
  virtual std::string hotspotLine(int i) = 0;  // text for the setup screen, "" when done
};

// ---------- drawing surface ----------
struct Canvas {
  virtual ~Canvas() {}
  int bandY0 = 0, bandY1 = 240;  // rows currently being rendered (for culling)
  virtual void fillRect(int x, int y, int w, int h, uint16_t c) = 0;
  virtual void drawRect(int x, int y, int w, int h, uint16_t c) = 0;
  virtual void line(int x0, int y0, int x1, int y1, uint16_t c) = 0;
  virtual void fillTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) = 0;
  virtual void fillCircle(int x, int y, int r, uint16_t c) = 0;
  virtual void text(int x, int y, const char* s, uint16_t c, int size) = 0;  // size 1 = 8 px, 2 = 16 px
  virtual int textW(const char* s, int size) = 0;
};

}  // namespace wm
