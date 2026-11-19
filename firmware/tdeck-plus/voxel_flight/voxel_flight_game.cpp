// SPDX-License-Identifier: MIT
// Port of lewtheegg/voxel-flight for wadamesh (RGB565 fb at panel size, no OLED/DMA).
#include "voxel_host.h"
#include "flight_types.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "font5x7.h"

// Host-injected button (trackball click / on-screen) and manual steer.
static volatile bool s_btn = false;
static volatile int8_t s_steer = 0;  // -1 left, 0 autopilot, +1 right
void voxelHostSetButton(bool pressed) { s_btn = pressed; }
void voxelHostSetSteer(int dir)
{
  if (dir < 0) s_steer = -1;
  else if (dir > 0) s_steer = 1;
  else s_steer = 0;
}

// ---------- pins and settings ----------
// Display pins unused on the T-Deck port (framebuffer only).
// Max matches the T-Deck panel; runtime W/H follow the slim AppPage content.
constexpr int FB_MAX_W = 320;
constexpr int FB_MAX_H = 240;
int W = 64, H = 64;
uint16_t* fb = nullptr;
uint8_t* zb = nullptr;          // depth per pixel (2 per cell, 255 = sky), for bridges
uint8_t  sin8[256];
uint8_t  smooth32[32];

const uint32_t FRAME_MS = 33;      // ~30 fps target

// Camera / projection. Horizontal: 1 map cell = 1 unit. Vertical: heights 0..255, 2 per cell.
const float TAN_HALF_FOV = 0.75f;              // horizontal half field of view (about 74 deg total)
float KX = 32.0f / TAN_HALF_FOV;               // pixels per cell at distance 1 (set with W)
float KY = 16.0f / TAN_HALF_FOV;               // pixels per height unit at distance 1
int   HORIZON_Y = 20;                          // screen row of the horizon (camera looks slightly down)
const float ZFAR = 100;                        // view distance in cells
const float FOG_START = 0.45f;                 // fraction of ZFAR where the haze begins
const float CAM_ROLL_SHARE = 0.4f;             // how much of the bank the camera follows
const uint32_t CITY_SEARCH_FRAMES = 1800;      // look for a city to fly to about once a minute

// Selection screen
const int SHOW_FRAMES = 105;                   // each plane is shown for 3.5 s
const bool CAPTURE_FRAMES = false;
const int FORCE_PLANE = 0;                   // skip OLED selection UI on the T-Deck

// Day and night
const float DAY_SECONDS = 300;                 // one full day; nights pass twice as fast
const float START_TIME = 0.29f;                // time of day at boot (0 midnight, 0.25 dawn, 0.5 noon)
const int NIGHT_HAZE[3] = { 12, 16, 34 };      // night: dark navy at the horizon; distant ground fades into it
const int NIGHT_SKY[3] = { 3, 5, 16 };         // night sky overhead
const int NIGHT_CLOUD[3] = { 46, 52, 74 };     // moonlit clouds
const int NIGHT_LIGHT[3] = { 100, 110, 150 };  // moonlit ground multiplier /256 (raise if too dark,
                                               // but dim areas show faint scan lines on this OLED)

// World
const int SEA = 40;                            // water level (height units)
const int MAP_N = 256;                         // ring-buffer map size (cells); view must stay < MAP_N/2
const int GEN_LINES_PER_FRAME = 2;             // map rows/columns generated per frame at most
const int BIOME_BLEND = 112;                   // width of the soft border between biomes (of 256; max 128)

const char *BIOME_NAME[NUM_BIOMES] = { "lakeland", "mountains", "canyon", "farmland", "coast" };

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float smoothstepf(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static inline int lerpi(int a, int b, int t) { return a + (((b - a) * t) >> 8); }   // t 0..256
static inline float wrapPi(float a) {
  while (a > PI) a -= TWO_PI;
  while (a < -PI) a += TWO_PI;
  return a;
}

// 4x4 ordered-dither thresholds (0..15)
const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

// ---------- colour + drawing helpers ----------
static inline uint16_t rgb(int r, int g, int b) {
  r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t dim4(uint16_t c) { return (c >> 2) & 0x39E7; }               // x 1/4
static inline uint16_t dim34(uint16_t c) { return c - dim4(c); }                    // x 3/4
// Blend RGB565 colour c towards (r, g, b) by a (0..256)
static inline uint16_t blend565(uint16_t c, int r, int g, int b, int a) {
  int cr = (c >> 11) << 3, cg = ((c >> 5) & 63) << 2, cb = (c & 31) << 3;
  return rgb(lerpi(cr, r, a), lerpi(cg, g, a), lerpi(cb, b, a));
}

inline void px(int x, int y, uint16_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c; }

// 5x7 text from the OLED library's font (6 px per character)
void drawText(int x, int y, const char *s, uint16_t c) {
  for (; *s; s++, x += 6) {
    const unsigned char *g = font5x7 + 6 + (uint8_t)*s * 5;
    for (int col = 0; col < 5; col++) {
      uint8_t bits = pgm_read_byte(g + col);
      for (int row = 0; row < 7; row++) if (bits & (1 << row)) px(x + col, y + row, c);
    }
  }
}
int textWidth(const char *s) { return strlen(s) * 6 - 1; }

// No OLED: blit is a no-op; the AppPage reads fb[] each tick.
void blit() {}

// Forward declarations (Arduino .ino auto-prototypes)
static inline float wrapPi(float a);
static inline uint16_t rgb(int r, int g, int b);
static inline uint16_t blend565(uint16_t c, int r, int g, int b, int a);
void buildMatLit();
static inline uint32_t hash32(int x, int y);
static inline int field(int x, int y, int shift, int k);
static inline int bigField(int x, int y, int shift, int k, int scale);
static inline void force(TerrainInfo &t, bool primary, uint8_t m);
static inline bool treeAt(int x, int y, int density, int &h);
int biomeHeight(int biome, int x, int y, const Octaves &o, TerrainInfo &t, bool primary);
bool cityParams(int sgx, int sgy, int &cx, int &cy, int &R);
bool villageParams(int gx, int gy, int &cx, int &cy, int &R);
static inline int flatGround(int x, int y, const TerrainInfo &t, const Octaves &o);
bool settleGround(int &h, TerrainInfo &t, int flat, int d2, int R, int edge, int limit);
bool applyCity(int x, int y, int &h, TerrainInfo &t, const Octaves &o);
void applyVillage(int x, int y, int &h, TerrainInfo &t, const Octaves &o);
void applyTown(int x, int y, int &h, TerrainInfo &t, const Octaves &o);
void gradeRoad(int x, int y, int &h, TerrainInfo &t, const Octaves &o);
void biomeAt(int x, int y, int &band, int &other, int &w);
int terrain(int x, int y, TerrainInfo *info);
uint8_t materialFor(int h, int slope, const TerrainInfo &t);
void genCell(int x, int y, bool nbInMap);
void streamMap(int targetOX, int targetOY, int maxLines);
static inline int groundAt(int32_t x88, int32_t y88);
void updateDayLight();
void buildFogLevel(int f);
uint16_t worldCol(int r, int g, int b, float z);
int groundAhead(float ca, float sa, float dist);
bool findCity(int x, int y, int radius, bool aheadOnly, float dir, int &outX, int &outY);
void updateCamera();
bool roomAhead(float dist, float margin);
void updateTrick();
void updateFlight();
void mapTarget(int &ox, int &oy);
bool project(int32_t wx, int32_t wy, float h, float &sx, float &sy, float &zc);
static inline void skyPx(int x, int y, uint16_t c);
bool skyPos(float az, float elPx, float &sx, float &sy);
void drawMoon(float cx, float cy);
void skyBlob(int cx16, int cy16, int rx16, int ry16, const int *top, const int *bot, int maxA);
void drawSky();
static int cloudNoise(int x, int y, int shift, int salt);
void makeClouds();
static void cloudSpan(int i, int32_t a, int32_t b, int r, int g, int bl, int alpha);
void drawCloudLayer();
void buildRenderTables();
static inline bool inShadow(int32_t X, int32_t Y);
void renderTerrain();
float bridgeTop(const Bridge &b);
int bridgeAhead(float px, float py, float ch, float sh, float look);
bool bridgeWaypoint(int cx, int cy, int px, int py, float hd, float &wx, float &wy);
int checkedSlot(int gx, int gy);
static inline bool waterCell(int x, int y, int &h);
void addBridge(int x0, int y0, int x1, int y1, int deck, uint8_t style, int rampA, int landA, int rampB, int landB);
void scanBridges();
void zTri(ScreenPtZ a, ScreenPtZ b, ScreenPtZ c, uint16_t col);
void zLine(ScreenPtZ a, ScreenPtZ b, uint16_t col);
void zDot(ScreenPtZ a, uint16_t col);
void zPost(float x, float y, float h0, float h1, float w, uint16_t col);
void drawBridges();
void updateBalloon();
bool balloonSteer(float &wantTurn);
int balloonAhead(float px, float py, float ch, float sh, float look);
void drawBalloon();
void planeBasis();
void planePoint(float f, float r, float u, int32_t &wx, int32_t &wy, float &wh);
ScreenPt ssProject(float f, float r, float u);
static inline void ssPut(int x, int y, int z, uint16_t c);
void ssTri(ScreenPt a, ScreenPt b, ScreenPt c, uint16_t col);
void ssLine(ScreenPt a, ScreenPt b, uint16_t col);
void ssResolve(int bx0, int by0, int bx1, int by1);
void drawPlane();
void updateTrails();
void blendLine(float x0, float y0, float x1, float y1, int r, int g, int b, int a);
void drawTrails();
void setupShadow();
void drawStatBar(int y, const char *label, int value, uint16_t col);
void showcaseFrame(int idx, int f, bool chosen);
int runSelection();
void buildTables();
void generateWorld();
bool voxelHostBegin(void);
void voxelHostEnd(void);
bool voxelHostTick(void);



// ---------- materials ----------
// Daylight colour of each material. Some also glow at night (town lights).
const uint8_t MAT_RGB[NUM_MATERIALS][3] = {
  {  25,  65, 120 },   // M_DEEP
  {  45, 110, 150 },   // M_SHALLOW
  { 205, 190, 140 },   // M_SAND
  {  85, 145,  60 },   // M_GRASS
  {  40,  90,  45 },   // M_FOREST
  { 125, 118, 108 },   // M_ROCK
  { 235, 238, 245 },   // M_SNOW
  { 180, 105,  70 },   // M_STRATA_A  sandstone layers
  { 205, 150, 105 },   // M_STRATA_B
  { 150,  85,  65 },   // M_STRATA_C
  { 160, 150, 100 },   // M_DRY       scrub, dry grass
  { 215, 185,  90 },   // M_WHEAT
  { 120,  90,  60 },   // M_PLOUGHED
  { 110, 165,  55 },   // M_CROP
  {  45,  95,  40 },   // M_HEDGE
  {  80,  80,  86 },   // M_ROAD
  {  85,  85,  90 },   // M_LAMP      a road cell with a street lamp
  { 165, 160, 150 },   // M_PLAZA     paved city square
  {  30,  27,  25 },   // M_TUNNEL    the dark mouth of a road tunnel
  { 165,  80,  60 },   // M_ROOF      tiled roof
  { 140, 135, 130 },   // M_ROOF_LIT  slate roof, windows lit at night
  { 180, 175, 165 },   // M_CONCRETE  pale stone / concrete
  { 195, 185, 165 },   // M_CONCRETE_LIT
  {  90, 125, 160 },   // M_GLASS     blue office glass
  { 110, 145, 170 },   // M_GLASS_LIT
  {  55,  70,  85 },   // M_DARKGLASS smoked glass
  {  70,  88, 105 },   // M_DARKGLASS_LIT
  { 150,  85,  65 },   // M_BRICK
  { 165,  98,  72 },   // M_BRICK_LIT
  { 150, 150, 155 },   // M_ANTENNA   rooftop mast (red warning light at night)
  {  95,  95, 100 },   // M_ROOFTOP   flat roof
};
const uint8_t MAT_GLOW[NUM_MATERIALS][3] = {   // night glow (0 = none)
  {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
  {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
  { 255, 150,  40 },   // M_LAMP
  {   0,   0,   0 },
  {   0,   0,   0 },
  {   0,   0,   0 },
  { 150, 110,  50 },   // M_ROOF_LIT  (a cottage's lit windows: warm, not too bright)
  {   0,   0,   0 },
  { 255, 200, 110 },   // M_CONCRETE_LIT
  {   0,   0,   0 },
  { 170, 200, 255 },   // M_GLASS_LIT
  {   0,   0,   0 },
  { 150, 190, 255 },   // M_DARKGLASS_LIT
  {   0,   0,   0 },
  { 255, 185,  95 },   // M_BRICK_LIT
  {   0,   0,   0 },
  {   0,   0,   0 },
};
const bool MAT_WATER[NUM_MATERIALS] = { 1, 1 };
static inline bool isTower(int m) { return m >= M_CONCRETE && m <= M_BRICK_LIT; }   // floors / windows

uint8_t matLit[256][3];    // material colour x slope light, for every colour index
uint16_t fogPal[16][256];  // [fog level 15 = near][material << 3 | light], rebuilt as the day turns

void buildMatLit() {
  for (int i = 0; i < 256; i++) {
    int m = i >> 3, l = i & 7;
    if (m >= NUM_MATERIALS) { matLit[i][0] = matLit[i][1] = matLit[i][2] = 0; continue; }
    float k = 0.45f + 0.55f * l / 7;
    for (int ch = 0; ch < 3; ch++) matLit[i][ch] = (uint8_t)min(255.0f, MAT_RGB[m][ch] * k);
  }
}

// ---------- noise ----------
// Everything grows from worldSeed, picked fresh at power-on, so every boot is a new world.
uint32_t worldSeed;
int seedOX[8], seedOY[8];          // per-field offsets so the noise fields are independent
uint8_t biomeOrder[NUM_BIOMES];    // which biome sits in which band of the biome field

static inline uint32_t hash32(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + worldSeed;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
static inline uint8_t hash2(int x, int y) { return hash32(x, y) >> 24; }

// Smooth 2D value noise: random lattice values blended with smoothstep. 0..255.
static inline int valueNoise(int x, int y, int shift) {   // lattice spacing = 1 << shift (shift <= 5)
  int ix = x >> shift, iy = y >> shift;
  int fx = smooth32[(x & ((1 << shift) - 1)) << (5 - shift)];
  int fy = smooth32[(y & ((1 << shift) - 1)) << (5 - shift)];
  int a = hash2(ix, iy), b = hash2(ix + 1, iy);
  int c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
  int top = a + (((b - a) * fx) >> 8);
  int bot = c + (((d - c) * fx) >> 8);
  return top + (((bot - top) * fy) >> 8);
}
// A noise field with its own seed offset. Neighbouring cells usually share the same four
// lattice corners, so each field remembers the last corners it hashed and reuses them.
int cacheIX[8] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN }, cacheIY[8];
uint8_t cacheV[8][4];
static inline int field(int x, int y, int shift, int k) {
  x += seedOX[k]; y += seedOY[k];
  int ix = x >> shift, iy = y >> shift;
  if (ix != cacheIX[k] || iy != cacheIY[k]) {
    cacheIX[k] = ix; cacheIY[k] = iy;
    cacheV[k][0] = hash2(ix, iy);     cacheV[k][1] = hash2(ix + 1, iy);
    cacheV[k][2] = hash2(ix, iy + 1); cacheV[k][3] = hash2(ix + 1, iy + 1);
  }
  int fx = smooth32[(x & ((1 << shift) - 1)) << (5 - shift)];
  int fy = smooth32[(y & ((1 << shift) - 1)) << (5 - shift)];
  const uint8_t *v = cacheV[k];
  int top = v[0] + (((v[1] - v[0]) * fx) >> 8);
  int bot = v[2] + (((v[3] - v[2]) * fx) >> 8);
  return top + (((bot - top) * fy) >> 8);
}

// The same noise for big features (lattice 1 << shift cells, any shift up to 9), evaluated at
// every cell. (The broad hills and the biome map used to be read at coarse coordinates, x >> 1
// and x >> 4, which made them change in steps: every 16 cells the biome blend jumped, and in a
// border zone that showed as a wall.) `scale` keeps the old feature positions.
static inline int bigField(int x, int y, int shift, int k, int scale) {
  x += seedOX[k] * scale; y += seedOY[k] * scale;
  int ix = x >> shift, iy = y >> shift, m = (1 << shift) - 1;
  if (ix != cacheIX[k] || iy != cacheIY[k]) {
    cacheIX[k] = ix; cacheIY[k] = iy;
    cacheV[k][0] = hash2(ix, iy);     cacheV[k][1] = hash2(ix + 1, iy);
    cacheV[k][2] = hash2(ix, iy + 1); cacheV[k][3] = hash2(ix + 1, iy + 1);
  }
  // smoothstep of the position inside the lattice cell, 0..256 (integer)
  int fx = (x & m) << (9 - shift), fy = (y & m) << (9 - shift);   // 0..511
  fx = (fx * fx * (1536 - 2 * fx)) >> 19;
  fy = (fy * fy * (1536 - 2 * fy)) >> 19;
  const uint8_t *v = cacheV[k];
  int top = v[0] + (((v[1] - v[0]) * fx) >> 8);
  int bot = v[2] + (((v[3] - v[2]) * fx) >> 8);
  return top + (((bot - top) * fy) >> 8);
}

// ---------- terrain ----------
// One height formula per biome, all built from the same five noise octaves (64, 32, 16, 8 and
// 4 cell features) so blending two biomes at a border costs little. All integer maths.
static inline int ridge(int n) { return 255 - abs(2 * n - 255); }   // 255 along the noise's mid-line

static inline void force(TerrainInfo &t, bool primary, uint8_t m) {
  if (primary) { t.flags |= F_FORCED; t.forced = m; }
}

// A tree on this cell? (density 0..255)
static inline bool treeAt(int x, int y, int density, int &h) {
  uint32_t r = hash32(x, y);
  if ((int)(r & 255) >= density) return false;
  h += 2 + ((r >> 8) & 3);
  return true;
}

// Set by terrain() before each biomeHeight call: how strongly the canyon terraces show (0..256),
// and the neighbouring biome blended in at this cell (for the flat ground under towns)
int terraceK = 256, blendOther = -1, blendW = 0;

int biomeHeight(int biome, int x, int y, const Octaves &o, TerrainInfo &t, bool primary) {
  int base = (o.a * 4 + o.b * 2 + o.c) / 7;
  switch (biome) {
    case B_LAKES: {      // low rolling hills, lakes, sandy shores, woods
      int h = (base >> 1) - 22 + (o.d >> 4) + (o.e >> 5);
      if (o.c > 150 && h > SEA + 4 && treeAt(x, y, (o.c - 150) * 3, h) && primary) t.flags |= F_TREE;
      return h;
    }
    case B_MOUNTAINS: {  // broad ridged ranges (64-cell ridges, 32-cell spurs) so slopes stay climbable
      int r1 = ridge(o.a), r2 = ridge(o.b);
      int h = 34 + (o.a >> 2) + ((r1 * r1) >> 10) + ((r2 * r2) >> 11) + (o.c >> 4) + (o.e >> 6);
      if (h < 95 && o.c > 140 && treeAt(x, y, (o.c - 140) * 2, h) && primary) t.flags |= F_TREE;
      return h;
    }
    case B_CANYON: {     // stepped sandstone plateaus cut by river canyons
      int raw = 30 + (base >> 1) + (o.b >> 2);
      int q = raw / 18, rem = raw % 18;
      int h = q * 18 + (rem > 12 ? (rem - 12) * 3 : 0);
      h = raw + (((h - raw) * terraceK) >> 8);           // terraces fade out towards other biomes
      int rv = abs(2 * o.c - 255);                       // 0 along the river line
      if (rv < 40) h = (h * rv + (SEA - 6) * (40 - rv)) / 40;
      return h;
    }
    case B_FARMLAND: {   // gentle hills, a patchwork of fields with hedges, lanes, copses
      int h = 44 + (base >> 2) + (o.d >> 5);
      if (o.c > 205) {                                   // a copse
        if (treeAt(x, y, 150, h)) force(t, primary, M_FOREST); else force(t, primary, M_GRASS);
        return h;
      }
      if (((x - 5) & 63) == 0 || ((y - 5) & 63) == 0) { force(t, primary, M_ROAD); if (primary) t.flags |= F_LANE; return h; }
      int wx = x + (o.c >> 3), wy = y + (o.b >> 3);       // wobbly field edges
      uint32_t f = hash32((wx >> 3) * 5 + 1, (wy / 7) * 3 + 2);
      if ((wx & 7) == 0 || (wy % 7) == 0) {               // field boundary
        if ((f & 3) != 0) {
          h += 1;
          if ((hash32(x, y) & 15) == 0) { h += 3; force(t, primary, M_FOREST); }   // hedgerow tree
          else force(t, primary, M_HEDGE);
        } else force(t, primary, M_GRASS);
        return h;
      }
      static const uint8_t FIELD[6] = { M_WHEAT, M_PLOUGHED, M_CROP, M_GRASS, M_DRY, M_WHEAT };
      force(t, primary, FIELD[(f >> 4) % 6]);
      return h;
    }
    case B_COAST: {      // sea with islands, beaches and cliffs
      int h = (base >> 1) - 30 + (o.c >> 3) + (o.e >> 5);
      if (o.c > 170 && h > SEA + 5 && treeAt(x, y, (o.c - 170) * 3, h) && primary) t.flags |= F_TREE;
      return h;
    }
  }
  return SEA;
}

// Settlements, sized to match the planes (a wingspan is 6 cells):
//  - Cities: at most one per 256x256 super-block, chosen by the biome at its centre. Radius
//    64..86 cells, 24-cell blocks with 3-cell streets, 7-cell avenues through the middle (carried
//    on over water by bridges). Each block holds one, two or four lots and each lot one separate
//    building set back from its edges, so there's open ground between them. Every building gets
//    its own height, wall material (stone, blue glass, smoked glass, brick) and shape (box,
//    setback, tower on a podium, the odd mast), with a flat roof that's a different colour from
//    its walls. Tall towers downtown, mid-rise around them, low-rise at the edge.
//  - Villages: at most one per 64x64 block away from cities: street grid, houses, gardens.
const uint8_t CITY_CHANCE[NUM_BIOMES] = { 120, 0, 0, 150, 150 };   // out of 256 per super-block
const uint8_t VILLAGE_CHANCE[NUM_BIOMES] = { 90, 0, 0, 150, 80 };  // out of 256 per 64-block
const int CITY_BLOCK = 24, CITY_STREET = 2, AVENUE_HALF = 1;   // avenue: 3 cells wide
const int BOULEVARD_HALF = 9;                                  // open corridor along the avenues: 19 cells
void biomeAt(int x, int y, int &band, int &other, int &w);

// (both remember their last answer: neighbouring cells nearly always ask about the same block)
bool cityParams(int sgx, int sgy, int &cx, int &cy, int &R) {
  static int lx = INT32_MIN, ly, lcx, lcy, lR; static bool lok; static uint32_t lseed;
  if (sgx != lx || sgy != ly || lseed != worldSeed) {
    lx = sgx; ly = sgy; lseed = worldSeed;
    uint32_t r = hash32(sgx * 53 + 11, sgy * 29 + 7);
    lcx = (sgx << 8) + 96 + ((r >> 8) & 63);
    lcy = (sgy << 8) + 96 + ((r >> 14) & 63);
    int band, other, w;
    biomeAt(lcx, lcy, band, other, w);
    lok = (int)(r & 255) < CITY_CHANCE[biomeOrder[band]];
    lR = 64 + ((r >> 20) & 15) * 3 / 2;                  // 64..86: stays inside the super-block
  }
  cx = lcx; cy = lcy; R = lR;
  return lok;
}

bool villageParams(int gx, int gy, int &cx, int &cy, int &R) {
  static int lx = INT32_MIN, ly, lcx, lcy, lR; static bool lok; static uint32_t lseed;
  if (gx != lx || gy != ly || lseed != worldSeed) {
    lx = gx; ly = gy; lseed = worldSeed;
    uint32_t r = hash32(gx * 31 + 7, gy * 17 + 3);
    lcx = (gx << 6) + 29 + ((r >> 19) & 3);
    lcy = (gy << 6) + 29 + ((r >> 22) & 3);
    int band, other, w;
    biomeAt(lcx, lcy, band, other, w);
    lok = (int)(r & 255) < VILLAGE_CHANCE[biomeOrder[band]];
    lR = 10 + ((r >> 16) & 7);
  }
  cx = lcx; cy = lcy; R = lR;
  return lok;
}

// Flat ground for a settlement: the cell's biome formula with only the broadest octave
// (blended with the neighbouring biome like the land around it, so no step at the town's edge)
static inline int flatGround(int x, int y, const TerrainInfo &t, const Octaves &o) {
  Octaves flatO = { o.a, 128, 128, 128, 128 };
  TerrainInfo tmp = t;
  terraceK = 0;
  int h = biomeHeight(t.biome, x, y, flatO, tmp, false);
  if (blendOther >= 0) h = (h * (256 - blendW) + biomeHeight(blendOther, x, y, flatO, tmp, false) * blendW) >> 8;
  return h;
}

// The ground under a settlement (radius R, blended over the outer `edge` cells). Returns true
// if the cell is fully flattened (somewhere to build); otherwise it has blended the natural
// height towards the flat ground and the caller leaves it as countryside. Everything fades:
// - towards the edge of the settlement,
// - where the flat ground is high (between `limit` and limit + 24), so towns don't climb hills,
// - where it's water, which is carved down to the water level so the shore is a smooth beach
//   rather than a hill cut off next to a flattened street.
bool settleGround(int &h, TerrainInfo &t, int flat, int d2, int R, int edge, int limit) {
  bool water = flat < SEA - 2;
  int target = water ? SEA - 3 : max(flat, SEA + 2);
  int core = R - edge, w = 256;
  if (d2 >= core * core) { w = (R * R - d2) * 256 / (R * R - core * core); w = (w * w) >> 8; }
  w = (w * clampi((limit + 24 - flat) * 256 / 24, 0, 256)) >> 8;
  if (w >= 256 && !water) return true;
  h = (h * (256 - w) + target * w) >> 8;
  if (h < t.raw) t.raw = h;                              // carved below the water level: water
  return false;
}

// Returns true if the cell is part of (or right next to) a city, so no village goes there
bool applyCity(int x, int y, int &h, TerrainInfo &t, const Octaves &o) {
  int sgx = x >> 8, sgy = y >> 8, cx, cy, R;
  if (!cityParams(sgx, sgy, cx, cy, R)) return false;
  int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
  bool avenue = abs(dx) <= AVENUE_HALF || abs(dy) <= AVENUE_HALF;
  if (d2 >= R * R) {                                     // outside: the avenue carries on as a one-cell lane
    if ((dx == 0 || dy == 0) && h > SEA) { t.flags = (t.flags & ~F_TREE) | F_FORCED | F_LANE; t.forced = M_ROAD; }
    return d2 < (R + 24) * (R + 24);
  }
  int flat = flatGround(x, y, t, o);
  if (!settleGround(h, t, flat, d2, R, 12, 120)) {       // outskirts, waterfront or high ground
    if (avenue && h > SEA) { t.flags = (t.flags & ~F_TREE) | F_FORCED; t.forced = M_ROAD; }
    return true;
  }
  h = max(flat, SEA + 2);
  t.flags = (t.flags & ~F_TREE) | F_FORCED;
  t.raw = h;

  int lx = dx + 7 * CITY_BLOCK, ly = dy + 7 * CITY_BLOCK;   // a whole number of blocks: avenues on block lines
  int bx = lx % CITY_BLOCK, by = ly % CITY_BLOCK;
  if (avenue) {                                          // avenue with lamps along both kerbs
    bool kerb = abs(dx) == AVENUE_HALF || abs(dy) == AVENUE_HALF;
    t.forced = (kerb && ((dx + dy) % 5 == 0)) ? M_LAMP : M_ROAD;
    return true;
  }
  if (bx < CITY_STREET || by < CITY_STREET) {            // street
    bool kerb = bx == 0 || bx == CITY_STREET - 1 || by == 0 || by == CITY_STREET - 1;
    t.forced = (kerb && ((lx + ly) % 6 == 0)) ? M_LAMP : M_ROAD;
    return true;
  }
  // Boulevards: buildings stand well back from the two main avenues, leaving an open corridor
  // (wider than a wingspan with room to spare) that the plane can fly down between the towers.
  // Paving with lawns and a row of trees each side.
  int ad = min(abs(dx), abs(dy));
  if (ad <= BOULEVARD_HALF) {
    t.forced = ad <= AVENUE_HALF + 2 ? M_PLAZA : M_GRASS;
    if (ad == AVENUE_HALF + 3 && ((dx + dy) & 3) == 0) { h += 4; t.forced = M_FOREST; }
    return true;
  }
  int ix = bx - CITY_STREET, iy = by - CITY_STREET, n = CITY_BLOCK - CITY_STREET;   // 0..20 inside the block
  uint32_t b = hash32(lx / CITY_BLOCK + sgx * 101, ly / CITY_BLOCK + sgy * 37);
  int q = d2 * 256 / (R * R);                            // 0 at the centre .. 256 at the edge
  t.forced = M_PLAZA;                                    // pavement unless a building or garden says otherwise

  if (((b >> 12) & 15) < (q < 100 ? 1 : 2)) {            // a park (rarer downtown)
    t.forced = M_GRASS;
    if ((hash32(x, y) & 7) == 0) { h += 4; t.forced = M_FOREST; }
    return true;
  }
  // The block holds one, two or four lots (downtown always splits, so towers stay slender).
  // Each lot has one building standing back from the lot edges, so there's open ground between.
  int split = (b >> 4) & 3;
  if (q < 100 && split == 0) split = 3;
  int lot0x = 0, lot1x = n, lot0y = 0, lot1y = n, part = 0, mid = n / 2;
  if (split & 1) { if (ix < mid) lot1x = mid; else { lot0x = mid; part |= 1; } }
  if (split & 2) { if (iy < mid) lot1y = mid; else { lot0y = mid; part |= 2; } }
  uint32_t bt = hash32(b & 0xFFFF, (b >> 16) + part * 977);
  if (((bt >> 20) & 15) < 2) {                           // an empty lot (1 in 8): square or garden
    if ((bt >> 24) & 1) { t.forced = M_GRASS; if ((hash32(x, y) & 7) == 0) { h += 4; t.forced = M_FOREST; } }
    return true;
  }
  // footprint: 2..4 cells back from each lot edge (varied per side), at least 4 cells wide
  int fx0 = lot0x + 2 + (bt & 3) % 3, fx1 = lot1x - 3 - ((bt >> 2) & 3) % 3;
  int fy0 = lot0y + 2 + ((bt >> 4) & 3) % 3, fy1 = lot1y - 3 - ((bt >> 6) & 3) % 3;
  if (fx1 - fx0 < 3) { fx0 = (lot0x + lot1x) / 2 - 2; fx1 = fx0 + 3; }
  if (fy1 - fy0 < 3) { fy0 = (lot0y + lot1y) / 2 - 2; fy1 = fy0 + 3; }
  if (ix < fx0 || ix > fx1 || iy < fy0 || iy > fy1) {    // the open ground around it
    if (((bt >> 25) & 3) == 0) t.forced = M_GRASS;       // some lots have lawns
    return true;
  }
  int inset = min(min(ix - fx0, fx1 - ix), min(iy - fy0, fy1 - iy));   // 0 = the building's outer wall
  inset = min(inset, ad - BOULEVARD_HALF - 1);           // a building cut back by a boulevard gets a wall there
  int small = min(fx1 - fx0, fy1 - fy0) + 1;            // narrowest side, cells

  // height: random within the ring's range, skewed so most are modest and a few soar
  int r8 = (bt >> 8) & 255, rr = ((r8 * r8 >> 8) + r8) >> 1;
  int lo, hi;
  if (q < 100)      { lo = 28; hi = 70 + (100 - q) * 7 / 10; }   // downtown: up to ~140
  else if (q < 180) { lo = 16; hi = 55; }                        // midtown
  else              { lo = 10; hi = 20; }                        // low-rise edge of town
  int tall = lo + ((hi - lo) * rr >> 8);

  // design: wall material (brick only away from downtown), lit windows, shape
  int kind = (bt >> 16) & 3;
  if (kind == 3 && q < 100) kind = (bt >> 18) & 1;
  bool lit = ((bt >> 26) & 3) != 0;
  uint8_t wall = M_CONCRETE + kind * 2 + lit;
  uint8_t roof = q >= 180 && kind == 3 ? (lit ? M_ROOF_LIT : M_ROOF) : M_ROOFTOP;
  int shape = (bt >> 28) & 3;                           // 0,1 box  2 setback  3 tower on a podium
  int coreIn = 0, base = tall;
  if (shape == 2 && small >= 5 && tall >= 30) { coreIn = max(1, small / 4); base = tall * 3 / 5; }
  if (shape == 3 && small >= 7 && tall >= 30) { coreIn = small / 3; base = max(10, tall / 4); }

  if (coreIn && inset >= coreIn) {                       // the upper tower
    h += tall;
    t.forced = inset == coreIn ? wall : roof;
    int ccx = (fx0 + fx1) / 2, ccy = (fy0 + fy1) / 2;
    if (tall > 90 && ((bt >> 30) & 1) && ix == ccx && iy == ccy) { h += 18; t.forced = M_ANTENNA; }
    return true;
  }
  h += base;
  t.forced = inset == 0 ? wall : roof;
  if (!coreIn && tall > 90 && ((bt >> 30) & 1) && ix == (fx0 + fx1) / 2 && iy == (fy0 + fy1) / 2) { h += 18; t.forced = M_ANTENNA; }
  return true;
}

void applyVillage(int x, int y, int &h, TerrainInfo &t, const Octaves &o) {
  int gx = x >> 6, gy = y >> 6, cx, cy, R;
  if (!villageParams(gx, gy, cx, cy, R)) return;
  int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
  if (d2 >= R * R) return;
  int flat = flatGround(x, y, t, o);
  if (!settleGround(h, t, flat, d2, R, 6, 110)) return;
  h = max(flat, SEA + 2);
  t.flags = (t.flags & ~F_TREE) | F_FORCED;
  t.raw = h;
  // Lanes every 8 cells with the odd street lamp; each 7x7 square between them has room for up
  // to two small cottages (about one on average) in big gardens, so houses stand well apart.
  int lx = dx + 64, ly = dy + 64;
  if ((lx & 7) == 0 || (ly & 7) == 0) { t.forced = ((lx * 3 + ly * 5) % 23 == 0) ? M_LAMP : M_ROAD; return; }
  int ix = (lx & 7) - 1, iy = (ly & 7) - 1;              // 0..6 inside the square
  uint32_t sq = hash32((lx >> 3) * 7 + gx * 131, (ly >> 3) * 11 + gy * 71);
  t.forced = M_GRASS;
  for (int k = 0; k < 2; k++) {
    uint32_t hk = sq >> (k * 12);
    if ((hk & 1) == 0) continue;                         // half the plots are empty
    int hx = (hk >> 1) % 5, hy = (hk >> 4) % 5;          // a 2x2 cottage at (hx..hx+1, hy..hy+1)
    if (ix >= hx && ix <= hx + 1 && iy >= hy && iy <= hy + 1) {
      h += 2 + ((hk >> 7) & 1);
      t.forced = ((hk >> 8) % 3) == 0 ? M_ROOF_LIT : M_ROOF;   // a third have lit windows at night
      return;
    }
  }
  if ((hash32(x, y) % 14) == 0) { h += 2; t.forced = M_FOREST; }   // garden trees
}

void applyTown(int x, int y, int &h, TerrainInfo &t, const Octaves &o) {
  if (!applyCity(x, y, h, t, o)) applyVillage(x, y, h, t, o);
}

// Country roads keep to a smooth level of their own (the cell's own biome without its small
// bumps or the neighbouring biome blended in) instead of riding over every lump: a little
// ground above that level is cut away (a cutting), a lot means the road goes into a tunnel (the
// hill stays, a dark mouth where it starts), and small dips are filled in (an embankment).
void gradeRoad(int x, int y, int &h, TerrainInfo &t, const Octaves &o) {
  Octaves smoothO = { o.a, o.b, o.c, 128, 128 };
  TerrainInfo tmp = t;
  terraceK = 0;
  int grade = max(biomeHeight(t.biome, x, y, smoothO, tmp, false), SEA + 2);
  int excess = h - grade;
  if (excess > 22) { t.flags &= ~(F_FORCED | F_LANE); return; }   // inside the tunnel: just hillside
  if (excess > 16) { t.forced = M_TUNNEL; return; }                // the tunnel mouth (a dark face)
  if (excess > -12 && h > SEA) { h = grade; t.raw = h; }           // cutting or embankment
}

// Which biome band a cell is in, and how much of the neighbouring band blends in (0..128)
void biomeAt(int x, int y, int &band, int &other, int &w) {
  // 512-cell features, contrast stretched, cut into five bands with wide soft borders
  int s = clampi(128 + (bigField(x, y, 9, 5, 16) - 128) * 2, 0, 255);
  int pos = s * NUM_BIOMES, frac = pos & 255;
  band = min(pos >> 8, (int)NUM_BIOMES - 1);
  other = -1; w = 0;
  int nb = -1, dist = 0;                                 // the nearer neighbouring band, and how far its border is
  if (frac < 128 && band > 0) { nb = band - 1; dist = frac; }
  else if (frac >= 128 && band < NUM_BIOMES - 1) { nb = band + 1; dist = 255 - frac; }
  if (nb < 0) return;
  if (biomeOrder[band] == B_FARMLAND) return;            // farmland stays pure farmland right to its edge
  if (biomeOrder[nb] == B_FARMLAND) {
    // next to farmland the whole change happens on this side, over the full half band: this
    // biome eases down (or up) until it's exactly farmland height at the border
    other = nb;
    int p = (128 - dist) * 2;                            // 0 .. 256 approaching the border
    w = (p * p * (768 - 2 * p)) >> 16;                   // smoothstep, 0 .. 256
    return;
  }
  if (dist >= BIOME_BLEND) return;
  other = nb;
  w = (BIOME_BLEND - dist) * 128 / BIOME_BLEND;          // 0 .. 128 at the border
  w = (w * w * (768 - 2 * w)) >> 16;                     // smoothstep across the whole border zone
}

// Height (0..250) of world cell (x, y), water already levelled to SEA. Fills info if given.
int terrain(int x, int y, TerrainInfo *info) {
  Octaves o;
  o.a = bigField(x, y, 6, 0, 2);
  o.b = field(x, y, 5, 1);
  o.c = field(x, y, 4, 2);
  o.d = field(x, y, 3, 3);
  o.e = field(x, y, 2, 4);

  int band, other, w;
  biomeAt(x, y, band, other, w);
  TerrainInfo t = { biomeOrder[band], 0, 0, 0 };
  terraceK = clampi(2 * (256 - w) - 256, 0, 256);        // full terraces only well inside a canyon
  int h = biomeHeight(t.biome, x, y, o, t, true);
  if (other >= 0 && w > 0) {
    TerrainInfo t2 = t;
    terraceK = clampi(2 * w - 256, 0, 256);
    h = (h * (256 - w) + biomeHeight(biomeOrder[other], x, y, o, t2, false) * w) >> 8;
  }
  blendOther = other >= 0 && w > 0 ? biomeOrder[other] : -1;
  blendW = w;
  t.raw = h;
  applyTown(x, y, h, t, o);
  if (t.flags & F_LANE) gradeRoad(x, y, h, t, o);
  h = clampi(h, 0, 250);
  if (!(t.flags & F_FORCED)) t.raw = h;
  if (h < SEA) h = SEA;
  if (info) *info = t;
  return h;
}

uint8_t materialFor(int h, int slope, const TerrainInfo &t) {
  if (t.raw < SEA) return SEA - t.raw > 8 ? M_DEEP : M_SHALLOW;
  if (t.flags & F_FORCED) {
    if (h < SEA + 2 && t.forced >= M_WHEAT && t.forced <= M_HEDGE) return M_SAND;   // fields don't reach the water
    return t.forced;
  }
  if (t.flags & F_TREE) return M_FOREST;
  switch (t.biome) {
    case B_LAKES:
      if (h < SEA + 3) return M_SAND;
      if (slope > 7) return M_ROCK;
      return h > 90 ? M_DRY : M_GRASS;
    case B_MOUNTAINS:
      if (h > 140 && slope < 8) return M_SNOW;
      if (h < SEA + 3) return M_SAND;
      if (slope > 5 || h > 105) return M_ROCK;
      return M_GRASS;
    case B_CANYON:
      if (h < SEA + 3) return M_SAND;
      if (slope < 2 && (h % 18) < 2) return M_DRY;         // plateau tops
      return M_STRATA_A + (h / 6) % 3;
    case B_FARMLAND:
      return M_GRASS;
    case B_COAST:
      if (slope > 5) return M_ROCK;                        // cliffs
      if (h < SEA + 4) return M_SAND;
      return M_GRASS;
  }
  return M_ROCK;
}

// ---------- streaming map ----------
// A 256x256 ring buffer: world cell (x, y) lives at [y & 255][x & 255]. The window
// [mapOX, mapOX+256) x [mapOY, mapOY+256) follows a point ahead of the camera; as it moves,
// the rows/columns falling off the back are regenerated as new ground in front.
// 256×256 maps are 128 KB — keep them in PSRAM so internal DRAM still fits Wi‑Fi/LVGL.
uint8_t *hmap = nullptr;         // height
uint8_t *cmap = nullptr;         // colour index (material << 3 | light)
int mapOX, mapOY;

// Light comes from the north-west, so each cell needs the height of (x-1, y-1). When that
// cell is already in the map (growing towards +x or +y) it's read back instead of recomputed,
// which halves the cost of those lines.
void genCell(int x, int y, bool nbInMap) {
  TerrainInfo t;
  int h = terrain(x, y, &t);
  int hn = nbInMap ? hmap[(((y - 1) & 255) << 8) | ((x - 1) & 255)] : terrain(x - 1, y - 1, nullptr);
  int light = t.raw < SEA ? 4 : clampi(4 + (h - hn) * 3 / 4, 0, 7);
  int idx = ((y & 255) << 8) | (x & 255);
  hmap[idx] = h;
  cmap[idx] = (materialFor(h, abs(h - hn), t) << 3) | light;
}
// grow = the line is next to the newest row/column on its -1 side, which is already in the map
void genRow(int y, bool grow) { for (int x = mapOX; x < mapOX + MAP_N; x++) genCell(x, y, grow && x > mapOX); }
void genCol(int x, bool grow) { for (int y = mapOY; y < mapOY + MAP_N; y++) genCell(x, y, grow && y > mapOY); }

// Move the window towards (targetOX, targetOY), a few lines per call
void streamMap(int targetOX, int targetOY, int maxLines) {
  int n = 0;
  while (mapOX < targetOX && n < maxLines) { genCol(mapOX + MAP_N, true); mapOX++; n++; }
  while (mapOX > targetOX && n < maxLines) { genCol(mapOX - 1, false); mapOX--; n++; }
  while (mapOY < targetOY && n < maxLines) { genRow(mapOY + MAP_N, true); mapOY++; n++; }
  while (mapOY > targetOY && n < maxLines) { genRow(mapOY - 1, false); mapOY--; n++; }
}

static inline int groundAt(int32_t x88, int32_t y88) {
  return hmap[(((y88 >> 8) & 255) << 8) | ((x88 >> 8) & 255)];
}
static inline bool inMap(int x, int y) { return x >= mapOX && x < mapOX + MAP_N && y >= mapOY && y < mapOY + MAP_N; }

// ---------- day and night ----------
// timeOfDay 0..1 (0 = midnight, 0.5 = noon). The sun's height drives everything: sky colours,
// the haze that distant ground fades into, how lit the ground is, and when town lights come on.
float timeOfDay = START_TIME;
int dayCount = 0;               // days flown so far
float moonPhase = 0.5f, moonPhase0 = 0;   // moon phase now (0 new .. 0.5 full .. 1) and at boot
float sunE = 0;                 // sun elevation, -1..1
float sunAz0;                   // where the sun rises
DayLight dl;
int fogLevelNext = 0;           // fog palette levels are rebuilt one per frame
const int SKY_STEPS = 192;      // sky gradient, in quarter rows above the horizon (48 rows)
const float SKY_GRAD_ROWS = 36; // rows over which the sky goes from the horizon colour to overhead
uint8_t skyGrad[SKY_STEPS][3];  // sky colour (8-bit RGB) by quarter rows above the horizon
int planeShade = 256;           // light on the plane

void updateDayLight() {
  float rate = 1.0f / (DAY_SECONDS * 1000.0f / FRAME_MS);
  if (sunE < -0.15f) rate *= 2;                          // short nights
  timeOfDay += rate;
  if (timeOfDay >= 1) { timeOfDay -= 1; dayCount++; }
  // the moon goes round its phases once every 8 days (a step every 3 hours of game time)
  moonPhase = moonPhase0 + (dayCount + timeOfDay) / 8.0f;
  moonPhase -= floorf(moonPhase);
  sunE = -cosf(timeOfDay * TWO_PI);

  int day  = (int)(256 * smoothstepf(-0.12f, 0.2f, sunE));
  int warm = (int)(256 * clampf(1 - fabsf(sunE - 0.03f) / 0.2f, 0, 1));
  dl.day = day;
  dl.warm = warm;
  dl.glow = (int)(256 * (1 - smoothstepf(-0.05f, 0.15f, sunE)));
  dl.stars = (int)(256 * (1 - smoothstepf(-0.2f, -0.02f, sunE)));

  const int DAY_H[3] = { 150, 190, 230 }, DAY_Z[3] = { 35, 95, 210 };
  const int DUSK_H[3] = { 240, 125, 55 }, DUSK_Z[3] = { 60, 50, 115 };
  const int DUSK_G[3] = { 250, 180, 135 };
  for (int c = 0; c < 3; c++) {
    dl.skyH[c] = lerpi(lerpi(NIGHT_HAZE[c], DAY_H[c], day), lerpi(NIGHT_HAZE[c], DUSK_H[c], max(day, 80)), warm * 3 / 4);
    dl.skyZ[c] = lerpi(lerpi(NIGHT_SKY[c], DAY_Z[c], day), lerpi(NIGHT_SKY[c], DUSK_Z[c], max(day, 80)), warm / 2);
    dl.ground[c] = lerpi(lerpi(NIGHT_LIGHT[c], 256, day), DUSK_G[c], warm / 2);
    dl.cloud[c] = lerpi(lerpi(NIGHT_CLOUD[c], 238, day), lerpi(NIGHT_CLOUD[c], c == 0 ? 255 : c == 1 ? 165 : 135, max(day, 96)), warm);
  }
  // Horizon -> zenith blend per quarter row: an S-curve over SKY_GRAD_ROWS, so it starts and
  // ends gently and has no sudden change of pace (the old curve rose fastest right at the
  // horizon, which made the sky look like a pale band with a dark one on top)
  static uint16_t skyT[SKY_STEPS];
  if (!skyT[SKY_STEPS - 1])
    for (int k = 0; k < SKY_STEPS; k++) skyT[k] = (uint16_t)(256 * smoothstepf(0, SKY_GRAD_ROWS, k / 4.0f));
  for (int k = 0; k < SKY_STEPS; k++) {
    int t = skyT[k];
    for (int c = 0; c < 3; c++) skyGrad[k][c] = lerpi(dl.skyH[c], dl.skyZ[c], t);
  }
  planeShade = max(dl.ground[1], 64);
}

// One fog level of the palette: lit ground colour faded towards the haze colour, plus town
// lights, which fade more slowly with distance so far-off cities twinkle at night.
uint8_t litNow[256][3];   // matLit x the current ground light (refreshed once per palette cycle)

void buildFogLevel(int f) {
  if (f == 0)
    for (int i = 0; i < 256; i++)
      for (int c = 0; c < 3; c++) litNow[i][c] = (matLit[i][c] * dl.ground[c]) >> 8;
  int glowF = f == 0 ? 0 : (dl.glow * (4 + f)) / 19;
  // How much of the ground colour survives at this distance (0..65535). Straight-line fading
  // hazes the middle distance a lot, so the curve is eased: middle distances stay clear and only
  // the far edge goes fully into the haze. Night uses the eased curve fully, day half of it.
  int kfL = f * 4369;                                    // /15 as a multiply (4369 = 65536 / 15)
  int kfE = 65535 - (15 - f) * (15 - f) * 291;           // 291 = 65535 / 225
  int kf = kfE + (((kfL - kfE) >> 8) * (dl.day >> 1)), kh = 65535 - kf;
  int hr = dl.skyH[0] * kh, hg = dl.skyH[1] * kh, hb = dl.skyH[2] * kh;
  uint16_t *out = fogPal[f];
  for (int i = 0; i < 256; i++) {
    int r = (litNow[i][0] * kf + hr) >> 16;
    int g = (litNow[i][1] * kf + hg) >> 16;
    int b = (litNow[i][2] * kf + hb) >> 16;
    int m = i >> 3;
    if (m < NUM_MATERIALS && MAT_GLOW[m][0] && glowF) {
      r += (MAT_GLOW[m][0] * glowF) >> 8;
      g += (MAT_GLOW[m][1] * glowF) >> 8;
      b += (MAT_GLOW[m][2] * glowF) >> 8;
    }
    out[i] = rgb(r, g, b);
  }
}

// Colour of a 3D object (bridge) part: lit by the time of day, faded by distance like the ground
uint16_t worldCol(int r, int g, int b, float z) {
  r = (r * dl.ground[0]) >> 8; g = (g * dl.ground[1]) >> 8; b = (b * dl.ground[2]) >> 8;
  int f = (int)(256 * clampf((1 - z / ZFAR) / (1 - FOG_START), 0, 1));
  return rgb(lerpi(dl.skyH[0], r, f), lerpi(dl.skyH[1], g, f), lerpi(dl.skyH[2], b, f));
}

// ---------- planes ----------
// Three planes, each a list of 3D points (forward, right, up, in cells), a list of primitives
// drawn back to front as seen from the chase camera, a few colours, and its handling.

// CUB: yellow high-wing taildragger. Slow, gentle, flies low.
const float CUB_V[][3] = {
  {  1.75f,  0.0f,  0.15f }, {  1.35f, -0.38f, 0.05f }, {  1.35f,  0.38f, 0.05f },   //  0 nose, 1-2 cowling
  {  0.8f,  -0.38f, 0.45f }, {  0.8f,   0.38f, 0.45f },                             //  3-4 cabin top front
  { -0.3f,  -0.34f, 0.40f }, { -0.3f,   0.34f, 0.40f }, { -2.4f, 0.0f, 0.30f },     //  5-6 cabin top back, 7 tail
  {  1.0f,  -3.0f,  0.70f }, {  0.1f,  -3.0f,  0.70f },                             //  8-9 left wingtip LE, TE
  {  1.0f,   3.0f,  0.70f }, {  0.1f,   3.0f,  0.70f },                             // 10-11 right wingtip
  {  1.0f,   0.0f,  0.62f }, {  0.1f,   0.0f,  0.62f },                             // 12-13 wing centre
  { -1.85f, -1.1f,  0.30f }, { -2.3f,  -1.1f,  0.30f },                             // 14-15 tailplane left
  { -1.85f,  1.1f,  0.30f }, { -2.3f,   1.1f,  0.30f }, { -1.6f, 0.0f, 0.30f },     // 16-17 right, 18 root
  { -2.3f,   0.0f,  1.05f }, { -1.8f,   0.0f,  0.35f },                             // 19 fin top, 20 fin front
  {  0.4f,  -0.38f, 0.0f  }, {  0.5f,  -1.7f,  0.66f },                             // 21-22 left strut
  {  0.4f,   0.38f, 0.0f  }, {  0.5f,   1.7f,  0.66f },                             // 23-24 right strut
  {  0.95f, -0.3f, -0.05f }, {  1.0f,  -0.65f,-0.55f },                             // 25-26 left gear, wheel
  {  0.95f,  0.3f, -0.05f }, {  1.0f,   0.65f,-0.55f },                             // 27-28 right gear, wheel
  {  1.3f,  -0.40f, 0.12f }, { -1.5f,  -0.20f, 0.24f },                             // 29-30 stripe left
  {  1.3f,   0.40f, 0.12f }, { -1.5f,   0.20f, 0.24f },                             // 31-32 stripe right
  {  0.55f, -3.0f,  0.70f }, {  0.55f,  3.0f,  0.70f },                             // 33-34 wingtip lights
  {  1.8f,   0.0f,  0.15f }, {  1.8f,   0.85f, 0.15f },                             // 35-36 prop hub, rim
};
const uint8_t CUB_C[][3] = { {245,195,35}, {195,145,20}, {30,25,20}, {120,120,125}, {70,90,110}, {140,140,145} };
const Prim CUB_P[] = {
  {PR_PROP,35,36,0,0,5},
  {PR_LINE,25,26,0,0,3}, {PR_LINE,27,28,0,0,3}, {PR_DOT,26,0,0,0,2}, {PR_DOT,28,0,0,0,2},
  {PR_TRI,1,2,7,0,1}, {PR_TRI,3,4,7,0,0}, {PR_TRI,0,1,2,0,0}, {PR_QUAD,3,4,6,5,4},
  {PR_LINE,29,30,0,0,2}, {PR_LINE,31,32,0,0,2},
  {PR_LINE,21,22,0,0,3}, {PR_LINE,23,24,0,0,3},
  {PR_WING_L,8,12,13,9,0}, {PR_WING_R,12,10,11,13,0}, {PR_LINE,9,11,0,0,1},
  {PR_TRI,18,14,15,0,0}, {PR_TRI,18,15,7,0,0}, {PR_TRI,18,16,17,0,0}, {PR_TRI,18,17,7,0,0},
  {PR_TRI,20,19,7,0,1}, {PR_LINE,20,19,0,0,0},
};

// RACER: red and white low-wing aerobatic plane. Quick, banks hard, hugs the ground.
const float RACER_V[][3] = {
  {  2.0f,  0.0f,  0.05f }, {  1.5f, -0.33f, 0.0f }, {  1.5f,  0.33f, 0.0f },       //  0 nose, 1-2 cowling
  {  0.7f,  0.0f,  0.42f }, { -0.5f,  0.0f,  0.34f },                               //  3 canopy front, 4 back
  {  0.1f, -0.22f, 0.26f }, {  0.1f,  0.22f, 0.26f }, { -2.3f, 0.0f, 0.15f },       //  5-6 canopy sides, 7 tail
  {  0.3f, -0.36f, 0.05f }, {  0.3f,  0.36f, 0.05f },                               //  8-9 body sides
  {  0.55f,-2.7f,  0.10f }, {  0.0f, -2.7f,  0.10f },                               // 10-11 left tip LE, TE
  {  0.55f, 2.7f,  0.10f }, {  0.0f,  2.7f,  0.10f },                               // 12-13 right tip
  {  1.05f, 0.0f, -0.15f }, { -0.1f,  0.0f, -0.15f },                               // 14-15 wing root
  { -1.9f, -0.95f, 0.15f }, { -2.3f, -0.95f, 0.15f },                               // 16-17 tailplane left
  { -1.9f,  0.95f, 0.15f }, { -2.3f,  0.95f, 0.15f }, { -1.6f, 0.0f, 0.15f },       // 18-19 right, 20 root
  { -2.35f, 0.0f,  0.95f }, { -1.7f,  0.0f,  0.25f },                               // 21 fin top, 22 fin front
  {  0.7f, -0.8f, -0.15f }, {  0.75f,-0.85f,-0.55f },                               // 23-24 left gear, spat
  {  0.7f,  0.8f, -0.15f }, {  0.75f, 0.85f,-0.55f },                               // 25-26 right gear, spat
  {  0.3f, -2.7f,  0.10f }, {  0.3f,  2.7f,  0.10f },                               // 27-28 wingtip lights
  {  2.05f, 0.0f,  0.05f }, {  2.05f, 0.8f,  0.05f },                               // 29-30 prop hub, rim
  {  0.62f,-2.1f,  0.08f }, {  0.02f,-2.1f,  0.08f },                               // 31-32 red tip panel left
  {  0.62f, 2.1f,  0.08f }, {  0.02f, 2.1f,  0.08f },                               // 33-34 right
};
const uint8_t RACER_C[][3] = { {225,40,35}, {165,25,25}, {240,240,245}, {80,110,150}, {120,120,125}, {150,150,155} };
const Prim RACER_P[] = {
  {PR_PROP,29,30,0,0,5},
  {PR_LINE,23,24,0,0,2}, {PR_LINE,25,26,0,0,2}, {PR_DOT,24,0,0,0,0}, {PR_DOT,26,0,0,0,0},
  {PR_WING_L,10,14,15,11,2}, {PR_WING_R,14,12,13,15,2},
  {PR_QUAD,10,31,32,11,0}, {PR_QUAD,33,12,13,34,0},
  {PR_TRI,1,2,7,0,1}, {PR_TRI,8,9,7,0,0}, {PR_TRI,0,1,2,0,0},
  {PR_LINE,8,7,0,0,2}, {PR_LINE,9,7,0,0,2},
  {PR_QUAD,3,6,4,5,3},
  {PR_TRI,20,16,17,0,2}, {PR_TRI,20,17,7,0,2}, {PR_TRI,20,18,19,0,2}, {PR_TRI,20,19,7,0,2},
  {PR_TRI,22,21,7,0,0}, {PR_LINE,22,21,0,0,2},
};

// JET: white business jet with swept wings, rear engines and a T-tail. Fastest, wide turns, flies high.
const float JET_V[][3] = {
  {  3.0f,  0.0f,  0.20f }, {  2.2f, -0.36f, 0.12f }, {  2.2f,  0.36f, 0.12f },     //  0 nose, 1-2 nose sides
  {  1.0f, -0.42f, 0.38f }, {  1.0f,  0.42f, 0.38f }, { -2.7f, 0.0f, 0.45f },       //  3-4 cabin top, 5 tail
  {  0.9f,  0.0f, -0.05f }, { -0.6f,  0.0f, -0.05f },                               //  6-7 wing root LE, TE
  { -0.9f, -3.3f,  0.20f }, { -1.4f, -3.3f,  0.20f },                               //  8-9 left tip LE, TE
  { -0.9f,  3.3f,  0.20f }, { -1.4f,  3.3f,  0.20f },                               // 10-11 right tip
  { -1.35f,-3.35f, 0.70f }, { -1.35f, 3.35f, 0.70f },                               // 12-13 winglet tops
  { -0.8f, -0.5f,  0.55f }, { -0.8f, -1.0f,  0.55f }, { -2.0f, -1.0f, 0.55f }, { -2.0f, -0.5f, 0.55f },   // 14-17 left engine
  { -0.8f,  0.5f,  0.55f }, { -0.8f,  1.0f,  0.55f }, { -2.0f,  1.0f, 0.55f }, { -2.0f,  0.5f, 0.55f },   // 18-21 right engine
  { -2.1f, -0.75f, 0.55f }, { -2.1f,  0.75f, 0.55f },                               // 22-23 exhaust glow
  { -1.7f,  0.0f,  0.50f }, { -2.55f, 0.0f,  1.70f },                               // 24 fin front, 25 fin top
  { -2.35f,-1.3f,  1.70f }, { -2.75f,-1.3f,  1.70f },                               // 26-27 T-tail left
  { -2.35f, 1.3f,  1.70f }, { -2.75f, 1.3f,  1.70f }, { -2.25f, 0.0f, 1.70f },      // 28-29 right, 30 root
  { -1.15f,-3.3f,  0.20f }, { -1.15f, 3.3f,  0.20f },                               // 31-32 wingtip lights
  {  1.9f, -0.40f, 0.25f }, { -1.2f, -0.42f, 0.38f },                               // 33-34 blue stripe left
  {  1.9f,  0.40f, 0.25f }, { -1.2f,  0.42f, 0.38f },                               // 35-36 right
  {  2.45f,-0.22f, 0.30f }, {  2.45f, 0.22f, 0.30f }, { 1.95f, 0.34f, 0.42f }, { 1.95f, -0.34f, 0.42f },   // 37-40 cockpit
};
const uint8_t JET_C[][3] = { {240,242,248}, {185,190,200}, {40,80,175}, {55,75,105}, {165,170,180}, {255,170,80} };
const Prim JET_P[] = {
  {PR_WING_L,8,6,7,9,1}, {PR_WING_R,6,10,11,7,1},
  {PR_LINE,8,12,0,0,2}, {PR_LINE,10,13,0,0,2},
  {PR_TRI,1,2,5,0,1}, {PR_TRI,3,4,5,0,0}, {PR_TRI,0,1,2,0,0},
  {PR_QUAD,37,38,39,40,3},
  {PR_LINE,33,34,0,0,2}, {PR_LINE,35,36,0,0,2},
  {PR_QUAD,14,15,16,17,4}, {PR_QUAD,18,19,20,21,4},
  {PR_GLOW,22,0,0,0,5}, {PR_GLOW,23,0,0,0,5},
  {PR_TRI,24,25,5,0,0}, {PR_LINE,24,25,0,0,2},
  {PR_TRI,30,26,27,0,0}, {PR_TRI,30,27,25,0,0}, {PR_TRI,30,28,29,0,0}, {PR_TRI,30,29,25,0,0},
};

#define NPTS(a) (uint8_t)(sizeof(a) / sizeof(a[0]))
const PlaneDef PLANES[3] = {
  //  name    model                         prims                 cols     tipL tipR strobe
  //  speed  maxTurn bank/turn maxBank clrMid clrMin camBack camUp  spd agi alt
  { "CUB",   CUB_V,   NPTS(CUB_V),   CUB_P,   NPTS(CUB_P),   CUB_C,   33, 34, 19,
    0.45f, 0.020f, 30, 0.6f,  24, 10, 14, 12,  2, 3, 2, TRICK_NONE },
  { "RACER", RACER_V, NPTS(RACER_V), RACER_P, NPTS(RACER_P), RACER_C, 27, 28, 21,
    0.62f, 0.030f, 42, 0.95f, 16,  8, 14, 10,  4, 5, 1, TRICK_LOOP },
  { "JET",   JET_V,   NPTS(JET_V),   JET_P,   NPTS(JET_P),   JET_C,   31, 32, 25,
    0.75f, 0.014f, 38, 0.5f,  50, 26, 17, 15,  5, 2, 5, TRICK_ROLL },
};
const PlaneDef *PL = &PLANES[0];

// ---------- autopilot ----------
// The plane follows the terrain: it climbs to clear the highest ground ahead, leans towards
// whichever side is lower (so it drifts into valleys and canyons), wanders gently, and now
// and then sets course for a city. Every input is low-pass filtered so the motion is smooth.
// Positions are fixed point 8.8 cells so precision never degrades on long flights.
int32_t planeX, planeY;          // 8.8 cells
float planeAlt;                  // height units
float altTarget;                 // filtered target height
float heading;                   // radians, 0 = +x, counter-clockwise
float turnRate = 0, bank = 0, climb = 0;
float wanderPhase[4];
uint32_t tick = 0;

// City visits
bool cityTarget = false;
int cityX, cityY;                // world cells
uint32_t nextCitySearch = 0;
bool viaTried = false, viaActive = false;   // going in over one of the city's bridges
float viaX, viaY;                           // ...via this point beyond its far end

// Camera (follows the plane with a little lag)
int32_t camX, camY;
float camH, camA, camRoll;
float camFX, camFY, camRX, camRY;    // forward and right unit vectors
int clearanceNow = 0;
float camBackX = 0, camLiftX = 0;    // extra camera distance / height (eased), for aerobatics
float flybyAlt = 0;                 // height to fly at during a balloon flyby (0 = none)

// Aerobatics state (see updateTrick)
enum TrickState : uint8_t { TS_IDLE, TS_LINEUP, TS_FLYING };
TrickState trickState = TS_IDLE;
int trickFrame = 0;
uint32_t nextTrick = 450;            // first one after ~15 s
float offF = 0, offR = 0, offU = 0;  // plane offset from its path: forward, right, up (cells)
float trickPitch = 0, trickRoll = 0; // extra attitude (radians)
float trickGlow = 0;                 // 0..1, brightens the wingtip trails
float trickCamBack = 0, trickCamLift = 0;

int groundAhead(float ca, float sa, float dist) {
  return groundAt(planeX + (int32_t)(ca * dist * 256), planeY + (int32_t)(sa * dist * 256));
}

// Nearest city centre within `radius` super-blocks of world cell (x, y), optionally only
// ones at least 120 cells ahead of `dir`
bool findCity(int x, int y, int radius, bool aheadOnly, float dir, int &outX, int &outY) {
  int gx0 = x >> 8, gy0 = y >> 8;
  long best = -1;
  float cd = cosf(dir), sd = sinf(dir);
  for (int gy = gy0 - radius; gy <= gy0 + radius; gy++)
    for (int gx = gx0 - radius; gx <= gx0 + radius; gx++) {
      int cx, cy, R;
      if (!cityParams(gx, gy, cx, cy, R)) continue;
      long dx = cx - x, dy = cy - y;
      if (aheadOnly && dx * cd + dy * sd < 120) continue;
      long d2 = dx * dx + dy * dy;
      if (best < 0 || d2 < best) { best = d2; outX = cx; outY = cy; }
    }
  return best >= 0;
}

void updateCamera() {
  camFX = cosf(camA); camFY = sinf(camA);
  camRX = sinf(camA); camRY = -cosf(camA);
  camX = planeX - (int32_t)(camFX * (PL->camBack + camBackX) * 256);
  camY = planeY - (int32_t)(camFY * (PL->camBack + camBackX) * 256);
}

// Aerobatics. The Racer loops the loop, the Jet does barrel rolls, every 20-40 s when there's
// room. First it lines up (stops turning, wings level), then flies the figure as an offset
// from its straight path: the path (planeX/Y, planeAlt) is what the camera and the terrain
// following use, so the camera stays steady and the plane visibly flies round on screen.
// Both figures only ever go up from the path, so the terrain clearance still holds.
const int LOOP_FRAMES = 100, ROLL_FRAMES = 72;
const float LOOP_R = 8, ROLL_R = 3;                      // cells
const int LINEUP_MAX = 150;                              // give up lining up after 5 s

void scheduleTrick(int minF, int maxF) { nextTrick = tick + minF + esp_random() % (maxF - minF); }

// True if the next `dist` cells straight ahead are clear by `margin` below the plane
bool roomAhead(float dist, float margin) {
  float ch = cosf(heading), sh = sinf(heading);
  int top = 0;
  for (float d = -LOOP_R - 2; d <= dist; d += 3)
    for (int side = -1; side <= 1; side++)
      top = max(top, groundAt(planeX + (int32_t)((ch * d + sh * side * 5) * 256), planeY + (int32_t)((sh * d - ch * side * 5) * 256)));
  top = max(top, bridgeAhead(planeX / 256.0f, planeY / 256.0f, ch, sh, dist));
  return planeAlt - top > margin;
}

void updateTrick() {
  if (PL->trick == TRICK_NONE) return;
  trickCamBack = trickCamLift = 0;
  if (trickState == TS_IDLE) {
    if (tick >= nextTrick && flybyAlt == 0) { trickState = TS_LINEUP; trickFrame = 0; }
    return;
  }
  bool isLoop = PL->trick == TRICK_LOOP;
  int len = isLoop ? LOOP_FRAMES : ROLL_FRAMES;
  if (isLoop) { trickCamBack = 8; trickCamLift = 16; }     // pull back and up to frame the loop
  else      { trickCamBack = 3; trickCamLift = 3; }
  trickFrame++;
  if (trickState == TS_LINEUP) {
    if (trickFrame > LINEUP_MAX) { trickState = TS_IDLE; scheduleTrick(150, 450); return; }
    bool level = fabsf(bank) < 0.04f && fabsf(turnRate) < 0.002f;
    if (level && trickFrame > 40 && roomAhead(PL->speed * len + 20, PL->clrMin)) {
      trickState = TS_FLYING; trickFrame = 0;
      Serial.println(isLoop ? ">> loop the loop" : ">> barrel roll");
    }
    return;
  }
  // flying the figure: eased from 0 to one full turn, so it starts and ends smoothly
  float u = (float)trickFrame / len;
  float e = u * u * (3 - 2 * u);
  float a = e * 6.2831853f;
  float prevU = offU;
  if (isLoop) {                                          // a vertical circle in the flight direction
    offF = LOOP_R * sinf(a);
    offU = LOOP_R * (1 - cosf(a));
    trickPitch = a;                                      // nose follows the circle
  } else {                                               // corkscrew: roll right, swing round the path
    offR = -ROLL_R * sinf(a);
    offU = ROLL_R * (1 - cosf(a));
    trickRoll = a;
    trickPitch = atanf((offU - prevU) / PL->speed);      // nose follows the up/down motion
  }
  trickGlow = sinf(u * 3.1415927f);                      // wingtip trails strongest mid-figure
  if (trickFrame >= len) {
    trickState = TS_IDLE;
    offF = offR = offU = trickPitch = trickRoll = trickGlow = 0;
    scheduleTrick(600, 1200);
  }
}

void updateFlight() {
  tick++;
  float t = tick;
  float ch = cosf(heading), sh = sinf(heading);
  float turnScale = PL->maxTurn / 0.02f;

  // Height: clear everything on the path ahead by a clearance that swells and shrinks over time.
  // The path is followed round the current turn, across a swath wider than the wings, a long
  // way out (towers can be 150 units tall). Something far away only needs the height we could
  // climb by the time we get there, so a skyscraper 120 cells ahead starts a gentle climb early
  // instead of a scramble up its wall at the last moment.
  const float PLAN_CLIMB = 0.9f;                         // height units per frame the plan assumes
  float perCell = PLAN_CLIMB / PL->speed;
  float need = 0;                                        // ground height that has to be cleared now
  int32_t needRate = 0;                                  // climb rate that clears everything ahead, 8.8
  {
    const int STEP = 2, LOOK = 120;                      // cells (the map reaches ~135+ ahead even when lagging)
    float dturn = -turnRate * STEP / PL->speed;          // heading change per step along the turn
    int32_t cd = (int32_t)(cosf(dturn) * 16384), sd = (int32_t)(sinf(dturn) * 16384);
    int32_t dx = (int32_t)(ch * 16384), dy = (int32_t)(sh * 16384);   // direction, 2.14
    int32_t px = planeX, py = planeY;                    // 8.8 cells
    int32_t per = (int32_t)(perCell * STEP * 256), drop = 0, best = 0;   // 8.8 height units
    int32_t altLim = (int32_t)((planeAlt - PL->clrMin) * 256), spd = (int32_t)(PL->speed * 256);
    for (int d = 0; d <= LOOK; d += STEP, drop += per) {
      int32_t wx = (dy * 7) >> 7, wy = (-dx * 7) >> 7;   // 3.5 cells to the right, 8.8
      for (int side = -2; side <= 2; side++) {           // centre, wingtips (3.5), wider (7): narrow
                                                         // enough to fly down a boulevard between towers
        int32_t g = groundAt(px + wx * side, py + wy * side) << 8;
        int32_t n = g - drop;
        if (n > best) best = n;
        if (g > altLim) {                                // climb rate needed to clear it in time
          int32_t rate = (g - altLim) * spd / (max(d, 4) << 8);
          if (rate > needRate) needRate = rate;
        }
      }
      px += (dx * STEP) >> 6; py += (dy * STEP) >> 6;
      if (d < 60) {                                      // follow the current turn for a while
        int32_t nx = (dx * cd - dy * sd) >> 14;
        dy = (dx * sd + dy * cd) >> 14; dx = nx;
      }
    }
    need = best / 256.0f;
  }
  float look = 60;
  need = max(need, (float)bridgeAhead(planeX / 256.0f, planeY / 256.0f, ch, sh, look));
  need = max(need, (float)balloonAhead(planeX / 256.0f, planeY / 256.0f, ch, sh, look));
  float clr = PL->clrMid + PL->clrMid * 0.5f * sinf(t * 0.0021f + wanderPhase[0]) + PL->clrMid * 0.25f * sinf(t * 0.0057f + wanderPhase[1]);
  clr = clr < PL->clrMin ? PL->clrMin : clr;
  clearanceNow = (int)clr;
  float want = need + clr;
  if (flybyAlt > want) want = flybyAlt;                 // up to a balloon's height for a flyby
  altTarget += (want - altTarget) * (want > altTarget ? 0.15f : 0.02f);   // rise sooner than sink
  float maxClimb = 0.8f + PL->speed * 0.5f;
  float wantClimb = clampf((altTarget - planeAlt) * 0.05f, -0.35f * turnScale - 0.1f, maxClimb);
  // Feed-forward: never climb slower than what clears the worst obstacle ahead (with a margin),
  // so the plane is above a tower's roof well before it gets there rather than lagging behind.
  float ff = needRate / 256.0f * 1.5f;
  if (needRate > 0 && ff > wantClimb) wantClimb = min(ff, maxClimb);
  climb += (wantClimb - climb) * (ff > 0.15f ? 0.12f : 0.06f);
  planeAlt += climb;
  int under = groundAt(planeX, planeY);
  if (planeAlt < under + 6) planeAlt += (under + 6 - planeAlt) * 0.3f;   // soft floor (last resort)

  // Steering: wander, lean away from the higher side ahead, and head for a city if we have one
  int hl = groundAhead(cosf(heading + 0.5f), sinf(heading + 0.5f), 30);
  int hr = groundAhead(cosf(heading - 0.5f), sinf(heading - 0.5f), 30);
  float wander = (0.010f * sinf(t * 0.0031f + wanderPhase[2]) + 0.006f * sinf(t * 0.0013f + wanderPhase[3])) * turnScale;
  float valley = clampf((hl - hr) * 0.0005f * turnScale, -0.6f * PL->maxTurn, 0.6f * PL->maxTurn);   // > 0: left is higher
  float wantTurn = wander + valley;
  if (cityTarget) {
    // If the city has a bridge, go in over it: first to a point just beyond its far end, then
    // along the avenue (across the bridge) to the centre
    int px = planeX >> 8, py = planeY >> 8;
    if (!viaTried && tick % 15 == 0) {
      float wx, wy;
      if (bridgeWaypoint(cityX, cityY, px, py, heading, wx, wy)) {
        viaTried = true; viaActive = true; viaX = wx; viaY = wy;
        Serial.println(">> going in over the bridge");
      } else {
        // no bridge (yet): line up with whichever avenue faces us and fly in down its boulevard
        float cdx = cityX - px, cdy = cityY - py, cd = sqrtf(cdx * cdx + cdy * cdy);
        if (cd > 160 && cd < 400) {
          const int AX[4] = { 1, -1, 0, 0 }, AY[4] = { 0, 0, 1, -1 };
          float best = 1e9f;
          for (int k = 0; k < 4; k++) {
            float qx = cityX + AX[k] * 130.0f, qy = cityY + AY[k] * 130.0f;
            float ex = qx - px, ey = qy - py, e2 = ex * ex + ey * ey;
            if (e2 < 50.0f * 50 || ex * cosf(heading) + ey * sinf(heading) < 0.3f * sqrtf(e2)) continue;
            if (e2 < best) { best = e2; wx = qx; wy = qy; }
          }
          viaTried = true;
          if (best < 1e9f) { viaActive = true; viaX = wx; viaY = wy; Serial.println(">> lining up with a boulevard"); }
        }
      }
    }
    if (viaActive) {
      float vx = viaX - px, vy = viaY - py;
      if (vx * vx + vy * vy < 25 * 25 || vx * cosf(heading) + vy * sinf(heading) < 0) viaActive = false;
    }
    float tx = viaActive ? viaX : cityX, ty = viaActive ? viaY : cityY;
    float dx = tx - px, dy = ty - py;
    float diff = wrapPi(atan2f(dy, dx) - heading);
    wantTurn = clampf(-diff * 0.03f, -0.8f * PL->maxTurn, 0.8f * PL->maxTurn) + valley * 0.5f;
    float cdx = cityX - px, cdy = cityY - py;
    if (!viaActive && cdx * cdx + cdy * cdy < 30 * 30) {
      cityTarget = false;
      nextCitySearch = tick + CITY_SEARCH_FRAMES;
      Serial.println(">> over the city");
    }
  } else if (tick >= nextCitySearch) {
    nextCitySearch = tick + CITY_SEARCH_FRAMES;
    if (findCity(planeX >> 8, planeY >> 8, 2, true, heading, cityX, cityY)) {
      cityTarget = true;
      viaTried = viaActive = false;
      float dx = cityX - (planeX >> 8), dy = cityY - (planeY >> 8);
      Serial.printf(">> heading for a city %d cells away\n", (int)sqrtf(dx * dx + dy * dy));
    }
  }
  balloonSteer(wantTurn);                                 // a balloon flyby takes over the steering
  if (trickState != TS_IDLE) wantTurn = 0;               // fly straight for aerobatics
  // Upstream OLED build is autopilot-only (BOOT picks a plane). On the T-Deck,
  // holding left/right overrides the wander so the player can bank.
  if (s_steer) wantTurn = (float)s_steer * PL->maxTurn;
  turnRate += (wantTurn - turnRate) * (s_steer ? 0.12f : 0.03f);
  turnRate = clampf(turnRate, -PL->maxTurn, PL->maxTurn);
  heading -= turnRate;                                     // positive turnRate = turning right
  bank += (clampf(turnRate * PL->bankPerTurn, -PL->maxBank, PL->maxBank) - bank) * 0.06f;

  planeX += (int32_t)(cosf(heading) * PL->speed * 256);
  planeY += (int32_t)(sinf(heading) * PL->speed * 256);
  updateTrick();

  // Camera: behind and above, lagging the heading and height a little, so the plane swings
  // and bobs gently on screen. For a loop it eases back and up first, so the whole loop fits.
  camA += (heading - camA) * 0.06f;
  camBackX += (trickCamBack - camBackX) * 0.04f;
  camLiftX += (trickCamLift - camLiftX) * 0.04f;
  updateCamera();
  camH += (planeAlt + PL->camUp + camLiftX - camH) * 0.1f;
  int camGround = groundAt(camX, camY);
  if (camH < camGround + 6) camH += (camGround + 6 - camH) * 0.3f;   // soft floor
  camRoll = bank * CAM_ROLL_SHARE;
}

// Where the map window should be: centred about half the view distance ahead of the camera
// (measured from the plane's usual camera spot, so the camera pulling back for a loop doesn't
// make the map regenerate lines it already has)
void mapTarget(int &ox, int &oy) {
  float back = PL->camBack - ZFAR / 2;
  ox = (int)(planeX >> 8) - (int)(camFX * back) - MAP_N / 2;
  oy = (int)(planeY >> 8) - (int)(camFY * back) - MAP_N / 2;
}

// Project a world point (8.8 cells, height units) to the screen, with camera roll
float shear;   // horizon slope from camera roll (pixels per pixel)
bool project(int32_t wx, int32_t wy, float h, float &sx, float &sy, float &zc) {
  float rx = (wx - camX) / 256.0f, ry = (wy - camY) / 256.0f;
  zc = rx * camFX + ry * camFY;
  if (zc < 1.0f) return false;
  float xc = rx * camRX + ry * camRY;
  const float cx = W * 0.5f;   // was hardcoded 32 for the 64×64 OLED
  sx = cx + xc * KX / zc;
  sy = HORIZON_Y + (camH - h) * KY / zc + (sx - cx) * shear;
  return true;
}

// ---------- sky ----------
// Sky gradient above the terrain, a soft sun and soft clouds (sub-pixel positioned, so they glide
// rather than hop), the moon, and stars at night. Everything is drawn only on sky pixels, so
// hills hide it properly.
const int NUM_STARS = 90;
float starAz[NUM_STARS];
uint8_t starEl[NUM_STARS], starTw[NUM_STARS], starHue[NUM_STARS];
int16_t ybuf[FB_MAX_W];          // top of the terrain per column (filled by renderTerrain)
int32_t hz[FB_MAX_W];            // horizon row per column, 1/256 px

static inline void skyPx(int x, int y, uint16_t c) {
  if ((unsigned)x < W && y >= 0 && y < ybuf[x]) fb[y * W + x] = c;
}
// Screen position (float) of a sky direction (azimuth, pixels above the horizon)
bool skyPos(float az, float elPx, float &sx, float &sy) {
  float d = wrapPi(az - camA);
  if (fabsf(d) > 1.0f) return false;
  const float cx = W * 0.5f;
  sx = cx - d * KX;
  sy = HORIZON_Y - elPx + (sx - cx) * shear;
  return true;
}

// The moon: a white disc lit according to moonPhase (0 new, 0.25 first quarter, 0.5 full, 0.75
// last quarter). For each pixel, the lit part is the side of the terminator (an ellipse across
// the disc) facing the sun; the dark part shows the sky. Edges are anti-aliased, with a faint
// glow that grows as the moon fills.
void drawMoon(float cx, float cy) {
  const float R = 3.6f;                                // pixels
  float cp = cosf(moonPhase * 6.2831853f);
  bool waxing = moonPhase < 0.5f;
  float lit = 0.5f - 0.5f * cp;                        // 0 new .. 1 full
  if (lit > 0.05f) {                                   // glow
    int g[3] = { 150, 160, 190 };
    skyBlob((int)(cx * 16), (int)(cy * 16), (int)(R * 16 * 2.2f), (int)(R * 16 * 2.2f), g, g, (int)(70 * lit));
  }
  for (int y = (int)(cy - R) - 1; y <= (int)(cy + R) + 1; y++)
    for (int x = (int)(cx - R) - 1; x <= (int)(cx + R) + 1; x++) {
      if ((unsigned)x >= W || y < 0 || y >= ybuf[x]) continue;
      float u = (x + 0.5f - cx) / R, v = (y + 0.5f - cy) / R;
      float d = sqrtf(u * u + v * v);
      float aEdge = clampf((1 - d) * R + 0.5f, 0, 1);    // disc edge, ~1 px soft
      if (aEdge <= 0) continue;
      float half = sqrtf(max(0.0f, 1 - v * v));
      float s = waxing ? (u - cp * half) : (-cp * half - u);   // > 0 on the lit side
      float aLit = clampf(s * R + 0.5f, 0, 1);
      int a = (int)(256 * aEdge * aLit);
      if (a <= 0) continue;
      fb[y * W + x] = blend565(fb[y * W + x], 240, 238, 225, a);
    }
}

// Optional settings for skyBlob (clouds use them): nothing is drawn below blobFloor16, and if
// blobGradY1 > blobGradY0 the top->bottom colour runs over that range (all in 1/16 px) instead
// of over each blob, so several puffs share one smooth shading.
int blobFloor16 = 1 << 20, blobGradY0 = 0, blobGradY1 = 0;

// Soft-edged ellipse blended into the sky. Centre and radii in 1/16 px; colour goes from
// (top) to (bottom) down the blob; maxA is the opacity in the middle (0..256).
void skyBlob(int cx16, int cy16, int rx16, int ry16, const int *top, const int *bot, int maxA) {
  if (rx16 < 24) rx16 = 24;
  if (ry16 < 24) ry16 = 24;
  int kx = (256 << 16) / (rx16 * rx16), ky = (256 << 16) / (ry16 * ry16);
  int x0 = max(0, (cx16 - rx16) >> 4), x1 = min(W - 1, (cx16 + rx16) >> 4);
  int y0 = max(0, (cy16 - ry16) >> 4), y1 = min(H - 1, (cy16 + ry16) >> 4);
  for (int y = y0; y <= y1; y++) {
    int dy = y * 16 + 8 - cy16;
    int qy = (dy * dy * ky) >> 16;                     // fits 32 bits: radii are 24..450
    if (qy >= 256 || y * 16 + 8 > blobFloor16) continue;
    int t = blobGradY1 > blobGradY0 ? clampi((y * 16 + 8 - blobGradY0) * 256 / (blobGradY1 - blobGradY0), 0, 256)
                                    : clampi(128 + dy * 128 / ry16, 0, 256);
    int r = lerpi(top[0], bot[0], t), g = lerpi(top[1], bot[1], t), b = lerpi(top[2], bot[2], t);
    for (int x = x0; x <= x1; x++) {
      if (y >= ybuf[x]) continue;
      int dx = x * 16 + 8 - cx16;
      int q = qy + ((dx * dx * kx) >> 16);
      if (q >= 256) continue;
      int a = min(256, (256 - q) * 3) * maxA >> 8;     // soft outer third
      uint16_t &c = fb[y * W + x];
      c = blend565(c, r, g, b, a);
    }
  }
}

void drawSky() {
  // gradient: looked up to a quarter of a row (follows the tilted horizon smoothly) and dithered
  // between neighbouring 16-bit colours, so it shows no bands
  for (int i = 0; i < W; i++) {
    for (int y = 0; y < ybuf[i]; y++) {
      int k = clampi((hz[i] - (y << 8) - 128) >> 6, 0, SKY_STEPS - 1);
      const uint8_t *s = skyGrad[k];
      int d = BAYER[y & 3][i & 3];                       // 0..15
      fb[y * W + i] = rgb(min(255, s[0] + (d >> 1)), min(255, s[1] + (d >> 2)), min(255, s[2] + (d >> 1)));
    }
  }
  // stars
  if (dl.stars > 0)
    for (int i = 0; i < NUM_STARS; i++) {
      float sx, sy;
      if (!skyPos(starAz[i], 3 + starEl[i], sx, sy)) continue;
      // blended over the sky (fading towards the sky colour, never darker than it, so no black
      // specks at dawn)
      int b = ((110 + (sin8[(uint8_t)(tick + starTw[i])] >> 2) + (starTw[i] & 63)) * dl.stars) >> 8;
      int x = (int)floorf(sx + 0.5f), y = (int)floorf(sy + 0.5f);
      if ((unsigned)x >= W || y < 0 || y >= ybuf[x]) continue;
      uint16_t &c = fb[y * W + x];
      if (starHue[i] == 0) c = blend565(c, 255, 255, 255, b);
      else if (starHue[i] == 1) c = blend565(c, 190, 200, 255, b);
      else c = blend565(c, 255, 225, 170, b);
    }
  // Sun and moon travel all the way round the sky once a day, opposite each other: each rises
  // at sunAz0, climbs to its highest (near the top of the screen), and sets on the far side,
  // sinking behind the hills (the sky is only drawn above the terrain). Their height is scaled
  // to the visible sky so the whole arc can be seen, moving smoothly (sub-pixel).
  const float ARC_PX = 17;                             // highest point, pixels above the horizon
  float sunAz = sunAz0 + TWO_PI * (timeOfDay - 0.25f);
  float sx, sy;
  if (sunE > -0.15f && skyPos(sunAz, sunE * ARC_PX, sx, sy)) {
    int glowC[3] = { 255, 170 + dl.day / 4, 80 + dl.day / 2 }, core[3] = { 255, 248, 225 };
    skyBlob((int)(sx * 16), (int)(sy * 16), 80, 80, glowC, glowC, 200);
    skyBlob((int)(sx * 16), (int)(sy * 16), 40, 40, core, core, 256);
  }
  // moon, opposite the sun, in its current phase
  if (sunE < 0.15f && skyPos(sunAz + PI, -sunE * ARC_PX, sx, sy)) drawMoon(sx, sy);
  drawCloudLayer();
}

// Clouds: a 3D layer of blocky clouds overhead, like Minecraft's. A repeating 64x64 map of
// blocks (each 8x8 cells, CLOUD_BOT..CLOUD_TOP high) drifts slowly; it's drawn with an upside
// down version of the terrain ray march: stepping outwards from the camera, each column draws
// the underside of every block it passes under, and the side face of a block it reaches across a
// gap, so near clouds show depth and far ones shrink towards the horizon. Span ends are blended
// by their fraction of a pixel so the edges glide instead of hopping. Far away (where a step is
// longer than a block) a blurred copy of the map gives soft coverage instead of flickering bits.
const int CLOUD_N = 64, CLOUD_SHIFT = 3;               // 64x64 blocks of 8x8 cells
const int CLOUD_BOT = 292, CLOUD_TOP = 300;            // height units (above anything the camera reaches)
const float CLOUD_NEAR = 200, CLOUD_FAR = 640;         // crisp blocks out to here; layer fades out by here
const int CLOUD_MAX_STEPS = 120;
uint64_t cloudBits[CLOUD_N];
uint8_t cloudSoft[CLOUD_N * CLOUD_N];
float cloudZ[CLOUD_MAX_STEPS];
int32_t cloudInv[CLOUD_MAX_STEPS];
int cloudSteps = 0;

// Tiling value noise on the cloud map (period CLOUD_N)
static int cloudNoise(int x, int y, int shift, int salt) {
  int P = CLOUD_N >> shift, ix = x >> shift, iy = y >> shift, m = (1 << shift) - 1;
  int fx = smooth32[(x & m) << (5 - shift)], fy = smooth32[(y & m) << (5 - shift)];
  int a = hash2(ix % P + salt, iy % P), b = hash2((ix + 1) % P + salt, iy % P);
  int c = hash2(ix % P + salt, (iy + 1) % P), d = hash2((ix + 1) % P + salt, (iy + 1) % P);
  int top = a + (((b - a) * fx) >> 8), bot = c + (((d - c) * fx) >> 8);
  return top + (((bot - top) * fy) >> 8);
}

void makeClouds() {
  for (int y = 0; y < CLOUD_N; y++) {
    cloudBits[y] = 0;
    for (int x = 0; x < CLOUD_N; x++) {
      int n = (cloudNoise(x, y, 4, 911) * 5 + cloudNoise(x, y, 3, 517) * 3 + cloudNoise(x, y, 2, 233) * 2) / 10;
      if (n > 150) cloudBits[y] |= 1ULL << x;
    }
  }
  for (int y = 0; y < CLOUD_N; y++)                    // 3x3 blur for the far distance
    for (int x = 0; x < CLOUD_N; x++) {
      int s = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) s += (cloudBits[(y + dy) & 63] >> ((x + dx) & 63)) & 1;
      cloudSoft[y * CLOUD_N + x] = s * 28;
    }
  cloudSteps = 0;                                      // distance steps: fine near, coarser far
  for (float z = 10; z < CLOUD_FAR && cloudSteps < CLOUD_MAX_STEPS; z = z * (z < CLOUD_NEAR ? 1.04f : 1.07f) + 0.4f) {
    cloudZ[cloudSteps] = z;
    cloudInv[cloudSteps] = (int32_t)(KY * 256 / z);
    cloudSteps++;
  }
}

// Blend rows a..b (1/256 px) of column i, ends weighted by how much of the pixel they cover
static void cloudSpan(int i, int32_t a, int32_t b, int r, int g, int bl, int alpha) {
  if (b <= a || alpha <= 0) return;
  int y0 = a >> 8, y1 = b >> 8, lim = ybuf[i];
  for (int y = max(0, y0); y <= y1 && y < lim; y++) {
    int cov = 256;
    if (y == y0) cov -= a & 255;
    if (y == y1) cov -= 256 - (b & 255);
    if (cov <= 0) continue;
    uint16_t &d = fb[y * W + i];
    d = blend565(d, r, g, bl, (alpha * cov) >> 8);
  }
}

void drawCloudLayer() {
  if (camH > CLOUD_BOT - 4 || !cloudSteps) return;
  int under[3] = { dl.cloud[0] * 3 / 4, dl.cloud[1] * 3 / 4, dl.cloud[2] * 4 / 5 };
  int sideX[3] = { dl.cloud[0], dl.cloud[1], dl.cloud[2] };
  int sideY[3] = { dl.cloud[0] * 7 / 8, dl.cloud[1] * 7 / 8, dl.cloud[2] * 9 / 10 };
  int opacity = 170 + dl.day * 70 / 256;
  int32_t ct[FB_MAX_W];                                // lowest cloud row drawn so far, 1/256 px
  int16_t prevBx[FB_MAX_W];
  bool prevOcc[FB_MAX_W], done[FB_MAX_W];
  for (int i = 0; i < W; i++) { ct[i] = INT32_MIN / 2; prevBx[i] = -1; prevOcc[i] = false; done[i] = ybuf[i] <= 0; }
  int32_t drift = tick * 6;                            // 8.8 cells: the layer drifts slowly
  int32_t camH16 = (int32_t)(camH * 16);
  int32_t hzMax = INT32_MIN;
  for (int i = 0; i < W; i++) hzMax = max(hzMax, hz[i]);
  const int halfW = W > 1 ? W / 2 : 1;
  for (int s = 0; s < cloudSteps; s++) {
    float z = cloudZ[s];
    int32_t inv = cloudInv[s];
    int32_t yB = ((camH16 - CLOUD_BOT * 16) * inv) >> 4;   // relative to the horizon, 1/256 px
    int32_t yT = ((camH16 - CLOUD_TOP * 16) * inv) >> 4;
    if (hzMax + yB < 0) continue;                      // still above the top of the screen
    bool near = z < CLOUD_NEAR;
    int fade = z < 300 ? 256 : (int)(256 * (CLOUD_FAR - z) / (CLOUD_FAR - 300));
    int alpha = (opacity * fade) >> 8;
    float lw = z * TAN_HALF_FOV;
    int32_t ox = camX + (int32_t)(camFX * z * 256) + drift, oy = camY + (int32_t)(camFY * z * 256);
    int32_t hx = (int32_t)(camRX * lw * 256), hy = (int32_t)(camRY * lw * 256);
    int32_t dX = hx / halfW, dY = hy / halfW;
    int32_t X = ox - hx + dX / 2, Y = oy - hy + dY / 2;
    for (int i = 0; i < W; i++, X += dX, Y += dY) {
      if (done[i]) continue;
      int bx = (X >> (8 + CLOUD_SHIFT)) & 63, by = (Y >> (8 + CLOUD_SHIFT)) & 63;
      int32_t b = hz[i] + yB, t = hz[i] + yT;
      if (near) {
        bool occ = (cloudBits[by] >> bx) & 1;
        if (occ) {
          if (prevOcc[i]) cloudSpan(i, ct[i], b, under[0], under[1], under[2], alpha);
          else {                                       // reached across a gap: its side face, then the underside
            const int *sc = bx != prevBx[i] ? sideX : sideY;
            cloudSpan(i, max(ct[i], t), b, sc[0], sc[1], sc[2], alpha);
          }
          if (b > ct[i]) ct[i] = b;
        }
        prevOcc[i] = occ;
        prevBx[i] = bx;
      } else {                                         // far: soft coverage, undersides only
        int cov = cloudSoft[by * CLOUD_N + bx];
        if (cov) cloudSpan(i, max(ct[i], t), b, under[0], under[1], under[2], (alpha * cov) >> 8);
        if (b > ct[i]) ct[i] = b;
      }
      if ((ct[i] >> 8) >= ybuf[i]) done[i] = true;
    }
  }
}

// ---------- terrain renderer ----------
// Voxel-space ray march: step outwards from the camera; at each distance take one map
// sample per screen column and draw only the part of the column that pokes above everything
// nearer (front-to-back, tracked in ybuf). Distance steps, perspective and fog are tables.
// To avoid blocky ground, heights and slope shading are interpolated between cells and the
// shading is dithered between its 8 levels; the material itself is picked by dithering between
// the four nearest cells, so field and shore edges dissolve instead of showing square steps.
// Sharp steps (buildings, cliff edges) are left crisp. Tower blocks get floors by day and lit
// windows at night. The plane's shadow is drawn here, on the exact ground surface.
const int MAX_STEPS = 200;
int numSteps = 0;
int32_t zTab[MAX_STEPS];      // distance, 8.8 cells
int32_t invTab[MAX_STEPS];    // KY * 256 / z
uint8_t fogTab[MAX_STEPS];    // 0..15
uint8_t zbTab[MAX_STEPS];     // depth for the z-buffer (2 per cell)
int revealSteps = 0;          // grows at boot so the world fades in from the plane outwards
const int CRISP_STEP = 16;    // height steps between neighbouring cells bigger than this stay sharp
const int SMOOTH_DIST = 45;   // smooth heights / shading / material edges out to this many cells

void buildRenderTables() {
  float z = 2.0f;
  numSteps = 0;
  // Wider frames cost one ray per column; step a little coarser above 160 px.
  const float zMul = W > 160 ? 0.028f : 0.02f;
  while (z < ZFAR && numSteps < MAX_STEPS) {
    zTab[numSteps] = (int32_t)(z * 256);
    invTab[numSteps] = (int32_t)(KY * 256 / z);
    float t = z / ZFAR;
    fogTab[numSteps] = (uint8_t)clampi((int)((1 - t) / (1 - FOG_START) * 15 + 0.5f), 0, 15);
    zbTab[numSteps] = (uint8_t)min(254, (int)(z * 2));
    numSteps++;
    z += 0.12f + z * zMul;
  }
}

// Shadow footprint, set up once per frame (plane position and axes, 8.8 cells / 2.14)
bool shadowOn = false;
int shadowK = 0;              // darkness 0..256
int32_t shX, shY;             // plane position, 16.16 wrapped to the map
int32_t shFX, shFY, shSpan, shTail;
float pbCos = 1, pbSin = 0, pPitch = 0, pFX = 1, pFY = 0;   // plane attitude, once per frame

static inline bool inShadow(int32_t X, int32_t Y) {
  int32_t dx = ((X - shX) << 8) >> 8, dy = ((Y - shY) << 8) >> 8;    // wrap to +-128 cells
  if (dx > (5 << 16) || dx < -(5 << 16) || dy > (5 << 16) || dy < -(5 << 16)) return false;
  int32_t rx = dx >> 8, ry = dy >> 8;                                 // 8.8 cells
  int32_t f = (rx * shFX + ry * shFY) >> 14;                          // along the plane
  int32_t r = (rx * shFY - ry * shFX) >> 14;                          // across (right +)
  if (r < 0) r = -r;
  if (f >= -38 && f <= 256 && r <= shSpan) return true;              // wing (-0.15 .. 1.0 cells)
  if (f >= -660 && f <= 512 && r <= 100) return true;                // fuselage
  if (f >= -660 && f <= -474 && r <= shTail) return true;            // tailplane
  return false;
}

void renderTerrain() {
  const float mid = (W - 1) * 0.5f;
  for (int i = 0; i < W; i++) {
    ybuf[i] = H;
    hz[i] = HORIZON_Y * 256 + (int32_t)((i - mid) * shear * 256);
  }
  memset(zb, 255, (size_t)W * (size_t)H);
  int32_t fX = (int32_t)(camFX * 16384), fY = (int32_t)(camFY * 16384);
  int32_t rX = (int32_t)(camRX * 16384), rY = (int32_t)(camRY * 16384);
  int32_t cX = (camX & 0xFFFF) << 8, cY = (camY & 0xFFFF) << 8;   // 16.16, wrapped to the map
  int32_t camH16 = (int32_t)(camH * 16);
  bool night = dl.glow > 128;
  bool beacon = (tick % 40) < 16;
  int done = 0;
  int steps = min(numSteps, revealSteps);
  const int halfW = W > 1 ? W / 2 : 1;

  for (int s = 0; s < steps && done < W; s++) {
    int32_t z = zTab[s], inv = invTab[s];
    const uint16_t *pal = fogPal[fogTab[s]];
    uint8_t zbv = zbTab[s];
    const uint8_t *bay = BAYER[s & 3];
    bool smoothStep = z < (SMOOTH_DIST << 8);       // beyond this a cell is under a pixel wide
    int32_t ox = cX + ((fX * z) >> 6), oy = cY + ((fY * z) >> 6);
    int32_t lw = (z * 3) >> 2;                      // half width = z * TAN_HALF_FOV
    int32_t hx = (rX * lw) >> 6, hy = (rY * lw) >> 6;
    int32_t X = ox - hx, Y = oy - hy;
    int32_t dX = hx / halfW, dY = hy / halfW;       // full width / W columns
    X += dX >> 1; Y += dY >> 1;                     // sample at column centres
    for (int i = 0; i < W; i++, X += dX, Y += dY) {
      int yb = ybuf[i];
      if (yb <= 0) continue;
      int mx = (X >> 16) & 255, my = (Y >> 16) & 255;
      int my0 = my << 8;
      int h00 = hmap[my0 | mx];
      // early out: the smoothed height is at most h00 + CRISP_STEP, so if even that is hidden, skip
      if (((hz[i] + (((camH16 - ((h00 + CRISP_STEP) << 4)) * inv) >> 4)) >> 8) >= yb) continue;
      int mx1 = (mx + 1) & 255, my1 = ((my + 1) & 255) << 8;
      int h10 = hmap[my0 | mx1], h01 = hmap[my1 | mx], h11 = hmap[my1 | mx1];
      int hi = max(max(h00, h10), max(h01, h11)), lo = min(min(h00, h10), min(h01, h11));
      int h16;
      uint8_t c;
      // (a cell next to a building is a step of 10+ height units, so it's crisp too; natural
      // slopes up to CRISP_STEP per cell stay smooth ramps rather than turning into stair-step walls)
      if (hi - lo > CRISP_STEP || !smoothStep || cmap[my0 | mx] >= (M_ROOF << 3)) {   // sharp step, building (square
                                                     // edges), or far away: no smoothing
        h16 = h00 << 4;
        c = cmap[my0 | mx];
      } else {
        int fx = (X >> 8) & 255, fy = (Y >> 8) & 255;
        int top = (h00 << 8) + (h10 - h00) * fx, bot = (h01 << 8) + (h11 - h01) * fx;
        h16 = ((top << 8) + (bot - top) * fy) >> 12;
        uint8_t c00 = cmap[my0 | mx], c10 = cmap[my0 | mx1], c01 = cmap[my1 | mx], c11 = cmap[my1 | mx1];
        // shading: bilinear light, dithered between its levels
        int lt = ((c00 & 7) << 8) + ((c10 & 7) - (c00 & 7)) * fx;
        int lb = ((c01 & 7) << 8) + ((c11 & 7) - (c01 & 7)) * fx;
        int d = bay[i & 3];
        int l = min(7, ((lt << 8) + (lb - lt) * fy + (d << 12)) >> 16);
        // material: pick one of the four cells, dithered by how close the sample is to each
        int dm = bay[(i + 2) & 3] << 4;
        uint8_t pick = (fy > dm) ? ((fx > dm) ? c11 : c01) : ((fx > dm) ? c10 : c00);
        c = (pick & 0xF8) | l;
      }
      int y = (hz[i] + (((camH16 - h16) * inv) >> 4)) >> 8;
      if (y >= yb) continue;
      int m = c >> 3;
      // Town lights come on (and go off at dawn) one by one: each cell has its own moment
      // during dusk. Until then it shows its unlit twin (always the material just before it).
      uint32_t lh = 0;
      bool lightsOn = false;
      if (MAT_GLOW[m][0] && dl.glow > 0) {
        lh = hash32(mx * 7 + 3, my * 13 + 5);
        lightsOn = dl.glow > 24 + (int)(lh & 255) * 7 / 8;
        if (!lightsOn) { c -= 8; m--; }
      }
      if (MAT_WATER[m]) {                            // water: slow ripples and the odd glint
        uint8_t w = sin8[(uint8_t)(mx * 13 + my * 7 + tick * 2)];
        int l = 2 + (w >> 6);
        if ((((mx * 29) ^ (my * 71)) + (tick >> 1) & 127) == 0) l = 7;
        c = (c & 0xF8) | l;
      }
      uint16_t col = pal[c];
      if (shadowOn && inShadow(X, Y)) col = blend565(col, 0, 0, 0, shadowK);
      int y0 = y < 0 ? 0 : y;
      uint16_t *p = fb + y0 * W + i;
      uint8_t *zp = zb + y0 * W + i;
      if (isTower(m) && fogTab[s] > 2) {             // facades: floors by day, windows by night
        uint16_t alt;
        if (night || lightsOn) {                     // some windows lit, the rest dark
          alt = dim4(col);
          // which windows are lit changes now and then, at a different moment for each cell
          uint8_t win = lightsOn ? hash2(mx * 3 + 1, my * 5 + 2 + ((tick + (lh >> 16)) >> 9) * 977) | 0x11 : 0;
          for (int yy = y0; yy < yb; yy++, p += W, zp += W) {
            int row = yy - y;
            *p = ((row & 1) && ((win >> ((row >> 1) & 7)) & 1)) ? col : alt;
            *zp = zbv;
          }
        } else {                                     // a darker line every third row: floors
          alt = dim34(col);
          for (int yy = y0; yy < yb; yy++, p += W, zp += W) { *p = ((yy - y) % 3 == 2) ? alt : col; *zp = zbv; }
        }
      } else if (m == M_ANTENNA && night) {          // mast with a blinking red light on top
        for (int yy = y0; yy < yb; yy++, p += W, zp += W) { *p = (yy == y && beacon) ? rgb(255, 30, 20) : dim4(col); *zp = zbv; }
      } else {
        for (int yy = y0; yy < yb; yy++, p += W, zp += W) { *p = col; *zp = zbv; }
      }
      ybuf[i] = y0;
      if (y0 == 0) done++;
    }
  }
}

// ---------- bridges ----------
// Where a city's main avenue runs into water, a bridge carries it across. Bridges are found by
// walking out along the avenues in the map once the city is fully loaded, and drawn as real 3D
// objects (deck, towers, cables, hangers, lamps) with a per-pixel depth test against the
// terrain, so hills and buildings in front hide them and the water shows underneath.
const int MAX_BRIDGES = 10, NUM_CHECKED = 64;
Bridge bridges[MAX_BRIDGES];
int checkedGX[NUM_CHECKED], checkedGY[NUM_CHECKED], checkedN = 0;
uint8_t checkedMask[NUM_CHECKED];                  // which of the 4 avenues have been traced (15 = done)

const float BRIDGE_HW = 2.0f;                        // deck half-width: a 3-cell avenue plus walkways
const float SUSPENSION_MIN = 30;                     // longer than this: towers and cables

static inline float bridgeLength(const Bridge &b) { return (float)(abs(b.x1 - b.x0) + abs(b.y1 - b.y0)); }
// Highest point of a bridge (tower tops), in height units
float bridgeTop(const Bridge &b) {
  float L = bridgeLength(b);
  return L >= SUSPENSION_MIN ? b.deck + 40 + L * 0.45f : b.deck + 3;
}

// Height the plane must clear for any bridge on its path in the next `look` cells (0 if none):
// the whole bridge counts as tall as its towers, so the plane climbs over cables and all.
int bridgeAhead(float px, float py, float ch, float sh, float look) {
  int need = 0;
  for (int i = 0; i < MAX_BRIDGES; i++) {
    const Bridge &b = bridges[i];
    if (!b.used) continue;
    float L = bridgeLength(b), ux = (b.x1 - b.x0) / L, uy = (b.y1 - b.y0) / L;
    float mx = (b.x0 + b.x1) * 0.5f - px, my = (b.y0 + b.y1) * 0.5f - py, reach = look + L * 0.5f + 20;
    if (mx * mx + my * my > reach * reach) continue;     // nowhere near
    for (float t = 0; t <= L; t += 3) {
      float dx = b.x0 + ux * t - px, dy = b.y0 + uy * t - py;
      float along = dx * ch + dy * sh, across = dx * sh - dy * ch;
      if (along > -6 && along < look + 6 && fabsf(across) < BRIDGE_HW + 8) { need = max(need, (int)bridgeTop(b)); break; }
    }
  }
  return need;
}

// A point 35 cells beyond the far end of one of city (cx, cy)'s bridges, if there's one the
// plane at (px, py) heading `hd` can reach on the way in: ahead of it, and closer than the city.
bool bridgeWaypoint(int cx, int cy, int px, int py, float hd, float &wx, float &wy) {
  float best = 1e9f;
  for (int i = 0; i < MAX_BRIDGES; i++) {
    const Bridge &b = bridges[i];
    if (!b.used) continue;
    float sx = b.x0 - cx, sy = b.y0 - cy;
    if (sx * sx + sy * sy > 150.0f * 150) continue;   // not this city's
    float L = bridgeLength(b), ux = (b.x1 - b.x0) / L, uy = (b.y1 - b.y0) / L;
    float qx = b.x1 + ux * 35, qy = b.y1 + uy * 35;
    float dx = qx - px, dy = qy - py, d2 = dx * dx + dy * dy;
    float cdx = cx - px, cdy = cy - py;
    if (d2 < 50.0f * 50 || d2 > cdx * cdx + cdy * cdy) continue;
    if (dx * cosf(hd) + dy * sinf(hd) < 0.3f * sqrtf(d2)) continue;   // well ahead
    if (d2 < best) { best = d2; wx = qx; wy = qy; }
  }
  return best < 1e9f;
}

int checkedSlot(int gx, int gy) {
  for (int i = 0; i < NUM_CHECKED; i++) if (checkedGX[i] == gx && checkedGY[i] == gy) return i;
  return -1;
}

static inline bool waterCell(int x, int y, int &h) {
  TerrainInfo ti;
  h = terrain(x, y, &ti);
  return ti.raw < SEA;
}

void addBridge(int x0, int y0, int x1, int y1, int deck, uint8_t style, int rampA, int landA, int rampB, int landB) {
  for (int i = 0; i < MAX_BRIDGES; i++)              // already known?
    if (bridges[i].used && bridges[i].x0 == x0 && bridges[i].y0 == y0 && bridges[i].x1 == x1 && bridges[i].y1 == y1) return;
  int slot = -1;
  for (int i = 0; i < MAX_BRIDGES; i++) if (!bridges[i].used) { slot = i; break; }
  if (slot < 0) {                                    // replace the one furthest from the camera
    long far = -1;
    for (int i = 0; i < MAX_BRIDGES; i++) {
      long dx = bridges[i].x0 - (camX >> 8), dy = bridges[i].y0 - (camY >> 8), d = dx * dx + dy * dy;
      if (d > far) { far = d; slot = i; }
    }
  }
  bridges[slot] = { x0, y0, x1, y1, deck, true, style, (uint8_t)rampA, (uint8_t)rampB, (int16_t)landA, (int16_t)landB };
  Serial.printf(">> bridge, %d cells long, deck %d, ramps %d (to %d) and %d (to %d)\n", abs(x1 - x0) + abs(y1 - y0), deck, rampA, landA, rampB, landB);
}

// Called every frame; does at most one city avenue (or a few cheap "not a city" checks) per
// call, using the terrain function directly, so bridges are known long before they're in view.
void scanBridges() {
  int cgx = (mapOX + MAP_N / 2) >> 8, cgy = (mapOY + MAP_N / 2) >> 8;
  for (int gy = cgy - 1; gy <= cgy + 1; gy++)
    for (int gx = cgx - 1; gx <= cgx + 1; gx++) {
      int slot = checkedSlot(gx, gy);
      if (slot >= 0 && checkedMask[slot] == 15) continue;
      int cx, cy, R;
      bool isCity = cityParams(gx, gy, cx, cy, R);
      if (slot < 0) {                                   // first look at this super-block
        slot = checkedN;
        checkedN = (checkedN + 1) % NUM_CHECKED;
        checkedGX[slot] = gx; checkedGY[slot] = gy;
        checkedMask[slot] = isCity ? 0 : 15;
        continue;
      }
      // a city with avenues still to trace: do one, along its centre line
      int k = 0;
      while (checkedMask[slot] & (1 << k)) k++;
      checkedMask[slot] |= 1 << k;
      const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
      int d, start = -1, end = -1, h, hs = -1, he = 0;
      for (d = R / 2; d < R + 50; d++) {                // land first, then the first water
        bool wet = waterCell(cx + DX[k] * d, cy + DY[k] * d, h);
        if (!wet) hs = h;
        else if (hs >= 0) { start = d - 1; break; }
      }
      if (start < 0) return;
      for (d = start + 1; d < start + 110; d++) {
        if (waterCell(cx + DX[k] * d, cy + DY[k] * d, h)) continue;
        bool solid = true;                              // real land, not a sandbar with water beyond
        for (int j = 1; j <= 6 && solid; j++) { int hh; if (waterCell(cx + DX[k] * (d + j), cy + DY[k] * (d + j), hh)) solid = false; }
        if (solid) { end = d; he = h; break; }
      }
      if (end < 0 || end - start < 14) return;         // short gaps just stay water
      int deck = max(max(hs, he) + 4, SEA + 16);
      // approach ramps: from each end, go inland until a gentle slope down from the deck meets
      // the ground (at most 40 cells; if the ground is still far below, the ramp just gets steeper)
      int ramp[2], land[2];
      for (int e = 0; e < 2; e++) {
        int base = e ? end : start, dir = e ? 1 : -1, n, g = deck;
        for (n = 2; n <= 40; n += 2) {
          g = terrain(cx + DX[k] * (base + dir * n), cy + DY[k] * (base + dir * n), nullptr);
          if (g >= deck - n * 0.6f) break;
        }
        ramp[e] = min(n, 40);
        land[e] = min(g, deck);
      }
      addBridge(cx + DX[k] * start, cy + DY[k] * start, cx + DX[k] * end, cy + DY[k] * end,
                deck, hash2(gx, gy) & 1, ramp[0], land[0], ramp[1], land[1]);
      return;
    }
}

ScreenPtZ zProject(float wx, float wy, float h) {     // world cells (float), height units
  ScreenPtZ p;
  float sx, sy, zc;
  p.ok = project((int32_t)(wx * 256), (int32_t)(wy * 256), h, sx, sy, zc) && zc < ZFAR;
  p.x = sx; p.y = sy; p.z = zc * 2;
  return p;
}

// Depth-tested filled triangle (depth interpolated across it)
void zTri(ScreenPtZ a, ScreenPtZ b, ScreenPtZ c, uint16_t col) {
  if (!a.ok || !b.ok || !c.ok) return;
  if (a.y > b.y) { ScreenPtZ t = a; a = b; b = t; }
  if (b.y > c.y) { ScreenPtZ t = b; b = c; c = t; }
  if (a.y > b.y) { ScreenPtZ t = a; a = b; b = t; }
  if (c.y < 0 || a.y >= H || c.y - a.y > 200 || c.y - a.y < 0.01f) return;
  if (fabsf(a.x) > 4000 || fabsf(b.x) > 4000 || fabsf(c.x) > 4000) return;
  int y0 = max(0, (int)ceilf(a.y - 0.5f)), y1 = min(H - 1, (int)floorf(c.y - 0.5f));
  if (y1 < y0) return;
  // edge slopes per row, in 16.16 (worked out once per triangle; the rows are all integer)
  float iac = 1.0f / (c.y - a.y);
  int32_t sAX = (int32_t)((c.x - a.x) * iac * 65536), sAZ = (int32_t)((c.z - a.z) * iac * 65536);
  float fy0 = y0 + 0.5f;
  int32_t xL = (int32_t)((a.x + (c.x - a.x) * (fy0 - a.y) * iac) * 65536);
  int32_t zL = (int32_t)((a.z + (c.z - a.z) * (fy0 - a.y) * iac) * 65536);
  bool upper = fy0 < b.y;
  int32_t sBX, sBZ, xR, zR;
  // set up the short edge for the half we start in; switch at b
  auto setEdge = [&](bool up, float fy) {
    const ScreenPtZ &p = up ? a : b, &q = up ? b : c;
    float d = q.y - p.y, inv = d > 0.01f ? 1.0f / d : 0;
    sBX = (int32_t)((q.x - p.x) * inv * 65536); sBZ = (int32_t)((q.z - p.z) * inv * 65536);
    xR = (int32_t)((p.x + (q.x - p.x) * (fy - p.y) * inv) * 65536);
    zR = (int32_t)((p.z + (q.z - p.z) * (fy - p.y) * inv) * 65536);
  };
  setEdge(upper, fy0);
  for (int y = y0; y <= y1; y++) {
    if (upper && y + 0.5f >= b.y) { upper = false; setEdge(false, y + 0.5f); }
    int32_t l = xL, r = xR, zl = zL, zr = zR;
    if (l > r) { int32_t t = l; l = r; r = t; t = zl; zl = zr; zr = t; }
    int x0 = (l + 32767) >> 16, x1 = (r - 32768) >> 16;    // pixel centres inside [l, r]
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x1 >= x0) {
      int32_t span = (r - l) >> 8;
      int32_t dz = span > 0 ? (int32_t)(((int64_t)(zr - zl) << 8) / span) : 0;
      int32_t z = zl + (int32_t)(((int64_t)(((x0 << 16) + 32768) - l) * dz) >> 16);
      uint16_t *fp = fb + y * W;
      uint8_t *zp = zb + y * W;
      for (int x = x0; x <= x1; x++, z += dz) {
        int zz = z >> 16;
        if (zz < zp[x]) { fp[x] = col; zp[x] = zz; }
      }
    }
    xL += sAX; zL += sAZ; xR += sBX; zR += sBZ;
  }
}
void zQuad(ScreenPtZ a, ScreenPtZ b, ScreenPtZ c, ScreenPtZ d, uint16_t col) { zTri(a, b, c, col); zTri(a, c, d, col); }

// Depth-tested line, stepped in 16.16 fixed point (no floats per pixel)
void zLine(ScreenPtZ a, ScreenPtZ b, uint16_t col) {
  if (!a.ok || !b.ok) return;
  if ((a.x < 0 && b.x < 0) || (a.x >= W && b.x >= W) || (a.y < 0 && b.y < 0) || (a.y >= H && b.y >= H)) return;
  int n = (int)max(fabsf(b.x - a.x), fabsf(b.y - a.y)) + 1;
  if (n > 160) return;
  int32_t x = (int32_t)(a.x * 65536), y = (int32_t)(a.y * 65536), z = (int32_t)(a.z * 65536);
  int32_t dx = (int32_t)((b.x - a.x) * 65536) / n, dy = (int32_t)((b.y - a.y) * 65536) / n, dz = (int32_t)((b.z - a.z) * 65536) / n;
  for (int k = 0; k <= n; k++, x += dx, y += dy, z += dz) {
    int sx = x >> 16, sy = y >> 16;
    if ((unsigned)sx >= W || (unsigned)sy >= H) continue;
    int i = sy * W + sx;
    if ((z >> 16) <= zb[i]) { fb[i] = col; zb[i] = z >> 16; }
  }
}
void zDot(ScreenPtZ a, uint16_t col) {
  if (!a.ok) return;
  int x = (int)a.x, y = (int)a.y;
  if ((unsigned)x >= W || (unsigned)y >= H) return;
  int i = y * W + x;
  if ((int)a.z <= zb[i]) fb[i] = col;
}

// A vertical post from height h0 to h1 at (x, y), as a camera-facing quad of half-width w cells
void zPost(float x, float y, float h0, float h1, float w, uint16_t col) {
  zQuad(zProject(x - camRX * w, y - camRY * w, h1), zProject(x + camRX * w, y + camRY * w, h1),
        zProject(x + camRX * w, y + camRY * w, h0), zProject(x - camRX * w, y - camRY * w, h0), col);
}

void drawBridges() {
  float cxw = (camX >> 8), cyw = (camY >> 8);
  bool night = dl.glow > 128;
  for (int b = 0; b < MAX_BRIDGES; b++) {
    Bridge &br = bridges[b];
    if (!br.used) continue;
    float x0 = br.x0 + 0.5f, y0 = br.y0 + 0.5f, x1 = br.x1 + 0.5f, y1 = br.y1 + 0.5f;
    float mx = (x0 + x1) / 2 - cxw, my = (y0 + y1) / 2 - cyw;
    // (out of range: skip drawing, but keep it - bridges are found up to ~380 cells away and each
    // city is only scanned once; when the list is full the furthest one makes room)
    if (mx * mx + my * my > 250.0f * 250.0f) continue;
    float L = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    float ux = (x1 - x0) / L, uy = (y1 - y0) / L, px = -uy, py = ux;       // along, across
    float dist = sqrtf(mx * mx + my * my);
    if (dist > ZFAR + L * 0.5f + 40) continue;
    if (mx * camFX + my * camFY < -(L * 0.5f + 45)) continue;   // wholly behind the camera (ramps included)
    float deck = br.deck;
    uint16_t deckCol = worldCol(85, 85, 92, dist), edgeCol = worldCol(160, 160, 165, dist);
    uint16_t towerCol = br.style ? worldCol(200, 70, 45, dist) : worldCol(170, 175, 185, dist);
    uint16_t cableCol = br.style ? worldCol(200, 90, 65, dist) : worldCol(180, 182, 190, dist);
    uint16_t hangerCol = br.style ? worldCol(150, 80, 65, dist) : worldCol(130, 132, 140, dist);   // thinner-looking: dimmer
    const float HW = BRIDGE_HW;                         // deck half-width (cells)

    // deck, in 4-cell segments so a partly visible bridge still draws
    int nseg = (int)ceilf(L / 4);
    for (int k = 0; k < nseg; k++) {
      float t0 = L * k / nseg, t1 = L * (k + 1) / nseg;
      float ax = x0 + ux * t0, ay = y0 + uy * t0, bx = x0 + ux * t1, by = y0 + uy * t1;
      ScreenPtZ a1 = zProject(ax - px * HW, ay - py * HW, deck), a2 = zProject(ax + px * HW, ay + py * HW, deck);
      ScreenPtZ b1 = zProject(bx - px * HW, by - py * HW, deck), b2 = zProject(bx + px * HW, by + py * HW, deck);
      zQuad(a1, a2, b2, b1, deckCol);
      zLine(a1, b1, edgeCol); zLine(a2, b2, edgeCol);
    }
    // approach ramps sloping down to the ground at each end, so the road runs up onto the bridge
    for (int e = 0; e < 2; e++) {
      int rl = e ? br.rampB : br.rampA;
      float land = e ? br.landB : br.landA;
      if (rl <= 0 || land >= deck) continue;
      float ex = e ? x1 : x0, ey = e ? y1 : y0, dir = e ? 1.0f : -1.0f;
      int rs = (rl + 3) / 4;
      for (int k = 0; k < rs; k++) {
        float t0 = (float)rl * k / rs, t1 = (float)rl * (k + 1) / rs;
        float h0 = deck - (deck - land) * t0 / rl, h1 = deck - (deck - land) * t1 / rl;
        float ax = ex + ux * t0 * dir, ay = ey + uy * t0 * dir, bx = ex + ux * t1 * dir, by = ey + uy * t1 * dir;
        ScreenPtZ a1 = zProject(ax - px * HW, ay - py * HW, h0), a2 = zProject(ax + px * HW, ay + py * HW, h0);
        ScreenPtZ b1 = zProject(bx - px * HW, by - py * HW, h1), b2 = zProject(bx + px * HW, by + py * HW, h1);
        zQuad(a1, a2, b2, b1, deckCol);
        zLine(a1, b1, edgeCol); zLine(a2, b2, edgeCol);
        if (night && dist < 90 && (k & 1)) zDot(zProject(ax + px * HW, ay + py * HW, h0 + 2), rgb(255, 170, 60));
      }
    }
    if (L >= SUSPENSION_MIN) {                          // suspension bridge: two towers and cables
      float top = bridgeTop(br);
      float ta = L * 0.2f, tb = L * 0.8f;
      for (int side = -1; side <= 1; side += 2) {
        float sx = px * HW * side, sy = py * HW * side;
        for (int tw = 0; tw < 2; tw++) {
          float t = tw ? tb : ta;
          zPost(x0 + ux * t + sx, y0 + uy * t + sy, SEA, top, 0.8f, towerCol);
        }
        // main cable: deck end -> tower top -> sag -> tower top -> deck end
        const int NC = 10;
        ScreenPtZ prev = zProject(x0 + sx, y0 + sy, deck + 1);
        for (int k = 0; k <= NC + 1; k++) {
          float t, h;
          if (k == 0) { t = ta; h = top; }
          else if (k == NC + 1) { t = L; h = deck + 1; }
          else { float u = (float)k / NC; t = ta + (tb - ta) * u; h = top - (top - deck - 3) * (1 - (2 * u - 1) * (2 * u - 1)); }
          ScreenPtZ cur = zProject(x0 + ux * t + sx, y0 + uy * t + sy, h);
          zLine(prev, cur, cableCol);
          if (k >= 2 && k <= NC - 2 && (k & 1) && dist > 25)   // hangers (left out right up close, where
            zLine(cur, zProject(x0 + ux * t + sx, y0 + uy * t + sy, deck), hangerCol);   // they'd be a tangle)
          prev = cur;
        }
      }
      for (int tw = 0; tw < 2; tw++) {                  // cross beams on the towers
        float t = tw ? tb : ta;
        zLine(zProject(x0 + ux * t - px * HW, y0 + uy * t - py * HW, top), zProject(x0 + ux * t + px * HW, y0 + uy * t + py * HW, top), towerCol);
        zLine(zProject(x0 + ux * t - px * HW, y0 + uy * t - py * HW, deck + 10), zProject(x0 + ux * t + px * HW, y0 + uy * t + py * HW, deck + 10), towerCol);
        if (night && (tick % 40) < 16) zDot(zProject(x0 + ux * t, y0 + uy * t, top + 1), rgb(255, 30, 20));
      }
    } else {                                            // short bridge: piers
      for (float t = 8; t < L - 4; t += 10) zPost(x0 + ux * t, y0 + uy * t, SEA, deck, 1.4f, towerCol);
    }
    if (night && dist < 90)                             // deck lamps along both edges
      for (float t = 2; t < L; t += 6)
        for (int side = -1; side <= 1; side += 2)
          zDot(zProject(x0 + ux * t + px * HW * side, y0 + uy * t + py * HW * side, deck + 2), rgb(255, 170, 60));
  }
}

// ---------- hot air balloon ----------
// Now and then (every 2.5-5 minutes, by day) a hot air balloon appears a long way ahead, off to
// one side of the route, drifting slowly on the wind, high above the ground. It's drawn as a
// striped, shaded pear-shaped envelope with ropes and a basket, depth-tested against the terrain
// so hills hide it, hazed with distance, with soft edges so it moves smoothly.
Balloon balloon = {};
uint32_t nextBalloon = 1800;                             // first chance after ~1 minute
float windDir = 0;
// balloon flyby (see balloonSteer)
int8_t flyby = 0;                                        // 0 none, 1 passing alongside, 2 circling
bool flybyOrbit = false, flybyAhead = false;
float passSide = 1, passRX = 0, passRY = 0;
int flybyFrames = 0;
float orbitR = 70, extX = 0, extY = 0;               // the turning point for a second pass
const float BALLOON_R = 4.5f;                            // envelope radius, cells
const uint8_t BALLOON_COLS[4][2][3] = {
  { { 215, 45, 40 }, { 245, 200, 55 } },                 // red and yellow
  { { 40, 95, 200 }, { 240, 240, 240 } },                // blue and white
  { { 55, 160, 75 }, { 245, 145, 40 } },                 // green and orange
  { { 145, 60, 170 }, { 250, 220, 90 } },                // purple and gold
};

void updateBalloon() {
  if (balloon.active) {
    balloon.x += balloon.vx; balloon.y += balloon.vy;
    float dx = balloon.x - camX / 256.0f, dy = balloon.y - camY / 256.0f;
    if (!flyby && (dx * camFX + dy * camFY < -20 || dx * dx + dy * dy > 420.0f * 420.0f)) {
      balloon.active = false;
      nextBalloon = tick + 4500 + esp_random() % 4500;
    }
    return;
  }
  if (tick < nextBalloon || dl.day < 160 || trickState != TS_IDLE) return;
  nextBalloon = tick + 300;                              // (try again in 10 s if it can't be placed)
  float ch = cosf(heading), sh = sinf(heading);
  float side = (esp_random() & 1) ? 1.0f : -1.0f, off = side * (20 + esp_random() % 30);
  float bx = planeX / 256.0f + ch * 190 + sh * off, by = planeY / 256.0f + sh * 190 - ch * off;
  int top = 0;                                           // highest ground it might drift over
  for (int j = -2; j <= 2; j++)
    for (int i = -2; i <= 2; i++) top = max(top, terrain((int)bx + i * 20, (int)by + j * 20, nullptr));
  if (top > 150) return;                                 // over high ground: wait for lower ground
  float h = min(240.0f, top + 45.0f + esp_random() % 30);
  balloon = { bx, by, h, 0.05f * cosf(windDir), 0.05f * sinf(windDir), true, (uint8_t)(esp_random() % 4) };
  Serial.println(">> hot air balloon");
  // About half the time the plane goes over for a closer look: a pass alongside at the balloon's
  // height, and (about a third of those) a full circle round it for a second pass
  flyby = 0;
  if (esp_random() % 100 < 55) {
    flyby = 1; flybyFrames = 0; flybyAhead = false;
    flybyOrbit = esp_random() % 100 < 33;
    passSide = side;                                     // pass it on the side away from where it is
    passRX = sh; passRY = -ch;                           // "right" when the balloon was spotted
    Serial.println(flybyOrbit ? ">> going for a closer look (and a second pass)" : ">> going for a closer look");
  }
}

// Steering for a balloon flyby (see updateBalloon). Sets wantTurn and the height to fly at;
// returns false when there's no flyby going on.
bool balloonSteer(float &wantTurn) {
  flybyAlt = 0;
  if (!flyby || !balloon.active || ++flybyFrames > 3600) { flyby = 0; return false; }
  float px = planeX / 256.0f, py = planeY / 256.0f, ch = cosf(heading), sh = sinf(heading);
  float cx = balloon.x, cy = balloon.y, tx, ty;
  if (flyby == 1) {
    // a point 16 cells beside the balloon, on the far side from where it started out
    tx = cx - passSide * passRX * 16; ty = cy - passSide * passRY * 16;
    float along = (cx - px) * ch + (cy - py) * sh;
    if (along > 25) flybyAhead = true;                   // it's been in front of us
    float qx = tx - px, qy = ty - py;
    if (qx * qx + qy * qy < 35 * 35) { tx = px + ch * 50; ty = py + sh * 50; }   // close: hold course past it
    if (flybyAhead && along < -6) {                      // gone past it
      if (!flybyOrbit) { flyby = 0; return false; }
      // second pass: carry on out a way, then turn round and come back at it head on (circling
      // it would keep the balloon off to the side, out of the camera's view)
      flyby = 2;
      orbitR = 70 + 0.8f * PL->speed / PL->maxTurn;      // further out for planes that turn wide
      extX = cx + ch * orbitR; extY = cy + sh * orbitR;
      Serial.println(">> turning back for another pass");
    }
  }
  if (flyby == 2) {
    tx = extX; ty = extY;
    float ex = extX - px, ey = extY - py;
    if (ex * ex + ey * ey < 25 * 25 || ex * ch + ey * sh < 0) { flyby = 1; flybyOrbit = false; flybyAhead = false; }
    tx = flyby == 2 ? extX : cx - passSide * passRX * 16;
    ty = flyby == 2 ? extY : cy - passSide * passRY * 16;
  }
  float diff = wrapPi(atan2f(ty - py, tx - px) - heading);
  wantTurn = clampf(-diff * 0.05f, -PL->maxTurn, PL->maxTurn);
  flybyAlt = balloon.h - 4;                              // at the balloon's height
  return true;
}

// Height the plane must clear for the balloon if it's on the path ahead (0 if not)
int balloonAhead(float px, float py, float ch, float sh, float look) {
  if (!balloon.active) return 0;
  float dx = balloon.x - px, dy = balloon.y - py;
  float along = dx * ch + dy * sh, across = dx * sh - dy * ch;
  if (along < -10 || along > look + 10 || fabsf(across) > 9) return 0;   // (a flyby passes 16 cells off)
  return (int)(balloon.h + BALLOON_R * 2.4f);
}

void drawBalloon() {
  if (!balloon.active) return;
  float hC = balloon.h + 2 * sinf(tick * 0.01f);         // a gentle rise and fall
  float sx, sy, zc;
  if (!project((int32_t)(balloon.x * 256), (int32_t)(balloon.y * 256), hC, sx, sy, zc) || zc > 160 || zc < 7) return;
  float rx = BALLOON_R * KX / zc, ry = BALLOON_R * 1.15f * KX / zc;   // 1 cell = 2 height units = KX px at z 1
  int fog = clampi((int)(256 * (160 - zc) / (160 - ZFAR * FOG_START)), 0, 256);
  int z8 = min(254, (int)(zc * 2));
  const uint8_t *c0 = BALLOON_COLS[balloon.look][0], *c1 = BALLOON_COLS[balloon.look][1];
  int lightR = dl.ground[0], lightG = dl.ground[1], lightB = dl.ground[2];
  // ropes and basket first (the envelope covers their tops)
  float bsx, bsy, bz;
  float hB = hC - BALLOON_R * 2 * 1.15f - 4;
  if (project((int32_t)(balloon.x * 256), (int32_t)(balloon.y * 256), hB, bsx, bsy, bz)) {
    int ra = (180 * fog) >> 8;
    blendLine(sx - rx * 0.4f, sy + ry * 0.85f, bsx - 0.3f, bsy, 90, 70, 50, ra);
    blendLine(sx + rx * 0.4f, sy + ry * 0.85f, bsx + 0.3f, bsy, 90, 70, 50, ra);
    float bw = max(0.6f, 0.8f * KX / bz);
    for (int y = (int)floorf(bsy - bw * 0.4f); y <= (int)floorf(bsy + bw * 0.6f); y++)
      for (int x = (int)floorf(bsx - bw * 0.5f); x <= (int)floorf(bsx + bw * 0.5f); x++)
        if ((unsigned)x < W && (unsigned)y < H && z8 < zb[y * W + x])
          fb[y * W + x] = blend565(fb[y * W + x], (120 * lightR) >> 8, (85 * lightG) >> 8, (50 * lightB) >> 8, fog);
  }
  // envelope: pear-shaped (narrower towards the bottom), vertical stripes (gores), lit from the
  // upper left, soft-edged
  // (all integer per pixel - the C3 has no floating point unit; Q8 means 256 = 1.0)
  static uint8_t halfTab[257];                           // sqrt(1 - v*v) by |v|, Q8
  if (!halfTab[0]) for (int i = 0; i <= 256; i++) halfTab[i] = (uint8_t)min(255.0f, 256 * sqrtf(max(0.0f, 1 - (i / 256.0f) * (i / 256.0f))));
  int rminQ8 = (int)(min(rx, ry) * 256);
  int ex0 = max(0, (int)floorf(sx - rx - 2)), ex1 = min(W - 1, (int)ceilf(sx + rx + 2));
  int ey0 = max(0, (int)floorf(sy - ry - 2)), ey1 = min(H - 1, (int)ceilf(sy + ry + 2));
  int32_t du = (int32_t)(65536 / rx);                    // u per pixel, Q16
  int32_t dvx = (int32_t)(-shear * 65536 / ry);          // v per pixel across (horizon tilt), Q16
  int32_t dvy = (int32_t)(65536 / ry);                   // v per pixel down, Q16
  int32_t u0 = (int32_t)((ex0 + 0.5f - sx) * 65536 / rx);
  int32_t v00 = (int32_t)((ey0 + 0.5f - sy - (ex0 + 0.5f - sx) * shear) * 65536 / ry);
  int fr = lerpi(dl.skyH[0], 0, fog), fg = lerpi(dl.skyH[1], 0, fog), fbl = lerpi(dl.skyH[2], 0, fog);   // haze part
  for (int y = ey0; y <= ey1; y++) {
    int32_t uQ = u0, vQ = v00 + (y - ey0) * dvy;
    for (int x = ex0; x <= ex1; x++, uQ += du, vQ += dvx) {
      if (z8 >= zb[y * W + x]) continue;
      int u = uQ >> 8, v = vQ >> 8;                      // Q8
      if (v < -300 || v > 300 || u < -300 || u > 300) continue;
      int wv = v > 0 ? 256 - ((115 * v * v) >> 16) : 256;   // taper towards the mouth
      if (wv < 64) continue;
      int uu = u * 256 / wv;
      int d2 = uu * uu + v * v;                          // Q16
      int edge = (65536 - d2) >> 1;                      // ~ (1 - d), Q16
      int a = ((edge >> 8) * rminQ8 >> 8) + 128;         // soft edge about a pixel wide
      if (a <= 0) continue;
      if (a > 256) a = 256;
      int half = halfTab[min(256, abs(v))];
      int gore = ((uu * 256 / max(half, 16)) + 256) * 3 >> 8;   // 6 stripes across, following the curve
      const uint8_t *c = (gore & 1) ? c1 : c0;
      int k = clampi(218 - ((77 * uu) >> 8) - ((64 * v) >> 8), 115, 282);   // lit from the upper left
      int r = min(255, c[0] * k >> 8) * lightR >> 8, g = min(255, c[1] * k >> 8) * lightG >> 8, b = min(255, c[2] * k >> 8) * lightB >> 8;
      r = fr + (r * fog >> 8); g = fg + (g * fog >> 8); b = fbl + (b * fog >> 8);
      fb[y * W + x] = blend565(fb[y * W + x], r, g, b, a);
    }
  }
}

// ---------- plane drawing ----------
// The chosen plane is projected with the same camera as the terrain, drawn at double resolution
// into a small buffer around the plane and averaged down, so its edges are anti-aliased and it
// moves smoothly by fractions of a pixel instead of hopping.
float ppCos = 1, ppSin = 0;   // pitch
void planeBasis() {
  float roll = bank + trickRoll;
  pbCos = cosf(roll); pbSin = sinf(roll);
  pPitch = clampf(climb * 0.3f, -0.3f, 0.3f) + trickPitch;
  ppCos = cosf(pPitch); ppSin = sinf(pPitch);
  pFX = cosf(heading); pFY = sinf(heading);
}

// World position of a model point (f, r, u in cells) for the current attitude:
// roll about the plane's own axis, then pitch, then the aerobatic offset, then heading.
void planePoint(float f, float r, float u, int32_t &wx, int32_t &wy, float &wh) {
  float r2 = r * pbCos + u * pbSin;         // bank: right wing goes down when bank > 0
  float u2 = u * pbCos - r * pbSin;
  float f3 = f * ppCos - u2 * ppSin + offF; // pitch: nose up when pPitch > 0
  float u3 = f * ppSin + u2 * ppCos + offU;
  r2 += offR;
  wx = planeX + (int32_t)((pFX * f3 + pFY * r2) * 256);   // right vector is (pFY, -pFX)
  wy = planeY + (int32_t)((pFY * f3 - pFX * r2) * 256);
  wh = planeAlt + u3 * 2;                   // 2 height units per cell
}

// Supersampled drawing buffer around the plane: SS_N x SS_N samples = SS_N/2 screen pixels
// Each sample also keeps a depth, so the parts hide each other correctly whatever the attitude
// (upside down at the top of a loop, rolling, or seen from any side on the turntable). The
// primitives are still listed back to front: a later one wins over an earlier one it lies
// within SS_BIAS of (stripes, panels and canopies sit on the surfaces they're painted on).
const int SS_N = 96;
const uint16_t SS_EMPTY = 0xFFFF, SS_BIAS = 80;   // depth in 1/256 cell
uint16_t ssCol[SS_N * SS_N];
uint16_t ssZ[SS_N * SS_N];
int ssOX, ssOY;                              // screen position of the buffer's top-left
bool ssStipple = false;                      // only every other sample, both ways (see-through)

ScreenPt ssProject(float f, float r, float u) {
  int32_t wx, wy; float wh, sx, sy, zc;
  planePoint(f, r, u, wx, wy, wh);
  ScreenPt p;
  p.ok = project(wx, wy, wh, sx, sy, zc);
  p.x = (int)floorf((sx - ssOX) * 2 + 0.5f);
  p.y = (int)floorf((sy - ssOY) * 2 + 0.5f);
  p.z = p.ok ? min(60000, (int)(zc * 256)) : 0;
  return p;
}

static inline void ssPut(int x, int y, int z, uint16_t c) {
  if ((unsigned)x >= SS_N || (unsigned)y >= SS_N) return;
  if (ssStipple && ((x | y) & 1)) return;
  int i = y * SS_N + x;
  if (ssZ[i] != SS_EMPTY && z > ssZ[i] + SS_BIAS) return;
  ssCol[i] = c; ssZ[i] = z;
}

void ssTri(ScreenPt a, ScreenPt b, ScreenPt c, uint16_t col) {
  if (!a.ok || !b.ok || !c.ok) return;
  if (a.y > b.y) { ScreenPt t = a; a = b; b = t; }
  if (b.y > c.y) { ScreenPt t = b; b = c; c = t; }
  if (a.y > b.y) { ScreenPt t = a; a = b; b = t; }
  if (c.y < 0 || a.y >= SS_N || c.y - a.y > 2 * SS_N) return;
  for (int y = max(a.y, 0); y <= min(c.y, SS_N - 1); y++) {
    int xl, zl, xr, zr;
    if (c.y == a.y) { xl = a.x; zl = a.z; }
    else { xl = a.x + (c.x - a.x) * (y - a.y) / (c.y - a.y); zl = a.z + (c.z - a.z) * (y - a.y) / (c.y - a.y); }
    if (y < b.y) {
      if (b.y == a.y) { xr = b.x; zr = b.z; }
      else { xr = a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y); zr = a.z + (b.z - a.z) * (y - a.y) / (b.y - a.y); }
    } else {
      if (c.y == b.y) { xr = b.x; zr = b.z; }
      else { xr = b.x + (c.x - b.x) * (y - b.y) / (c.y - b.y); zr = b.z + (c.z - b.z) * (y - b.y) / (c.y - b.y); }
    }
    if (xl > xr) { int t = xl; xl = xr; xr = t; t = zl; zl = zr; zr = t; }
    int32_t z16 = zl << 8, dz16 = xr > xl ? ((zr - zl) << 8) / (xr - xl) : 0;
    int x0 = xl;
    if (x0 < 0) { z16 += dz16 * -x0; x0 = 0; }
    int x1 = min(xr, SS_N - 1);
    for (int x = x0; x <= x1; x++, z16 += dz16) ssPut(x, y, z16 >> 8, col);
  }
}

void ssLine(ScreenPt a, ScreenPt b, uint16_t col) {
  if (!a.ok || !b.ok) return;
  int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
  int steps = max(dx, -dy), k = 0;
  for (int n = 0; n < 4 * SS_N; n++, k++) {
    ssPut(x0, y0, steps ? a.z + (b.z - a.z) * k / steps : a.z, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Average each 2x2 block of samples into the screen, weighted by how many are covered.
// Only the area the model's points span (bx0..bx1, by0..by1, in samples) is visited.
void ssResolve(int bx0, int by0, int bx1, int by1) {
  for (int y = max(0, by0 >> 1); y <= min(SS_N / 2 - 1, by1 >> 1); y++) {
    int sy = ssOY + y;
    if ((unsigned)sy >= H) continue;
    for (int x = max(0, bx0 >> 1); x <= min(SS_N / 2 - 1, bx1 >> 1); x++) {
      int sx = ssOX + x;
      if ((unsigned)sx >= W) continue;
      int i0 = (2 * y) * SS_N + 2 * x;
      int n = 0, r = 0, g = 0, b = 0;
      for (int k = 0; k < 4; k++) {
        int i = i0 + (k >> 1) * SS_N + (k & 1);
        if (ssZ[i] == SS_EMPTY) continue;
        uint16_t c = ssCol[i];
        r += (c >> 11) << 3; g += ((c >> 5) & 63) << 2; b += (c & 31) << 3;
        n++;
      }
      if (!n) continue;
      uint16_t &dst = fb[sy * W + sx];
      dst = blend565(dst, r / n, g / n, b / n, n * 64);
    }
  }
}

uint16_t planeCol(const uint8_t *c, int shade) { return rgb((c[0] * shade) >> 8, (c[1] * shade) >> 8, (c[2] * shade) >> 8); }

void drawPlane() {
  // centre the supersampling buffer on the plane
  int32_t wx, wy; float wh, csx, csy, zc;
  planePoint(0, 0, 0.3f, wx, wy, wh);
  if (!project(wx, wy, wh, csx, csy, zc)) return;
  ssOX = (int)floorf(csx) - SS_N / 4;
  ssOY = (int)floorf(csy) - SS_N / 4;

  ScreenPt p[48];
  int bx0 = SS_N, by0 = SS_N, bx1 = -1, by1 = -1;
  for (int v = 0; v < PL->nv; v++) {
    p[v] = ssProject(PL->verts[v][0], PL->verts[v][1], PL->verts[v][2]);
    if (!p[v].ok) continue;
    bx0 = min(bx0, p[v].x - 2); bx1 = max(bx1, p[v].x + 2);
    by0 = min(by0, p[v].y - 2); by1 = max(by1, p[v].y + 2);
  }
  for (int i = 0; i < PL->np; i++)                        // room for the whole propeller disc
    if (PL->prims[i].type == PR_PROP && p[PL->prims[i].a].ok) {
      const ScreenPt &h = p[PL->prims[i].a];
      bx0 = min(bx0, h.x - 12); bx1 = max(bx1, h.x + 12);
      by0 = min(by0, h.y - 12); by1 = max(by1, h.y + 12);
    }
  // whole 2x2 sample blocks, so the averaging below only reads samples that were cleared here
  // (it used to read one row/column past the cleared area: never-used buffer memory, black,
  // which showed as a faint dark bar under the plane during loops)
  bx0 = max(bx0, 0) & ~1; by0 = max(by0, 0) & ~1;
  bx1 = min(bx1, SS_N - 1) | 1; by1 = min(by1, SS_N - 1) | 1;
  if (bx1 < bx0 || by1 < by0) return;
  for (int y = by0; y <= by1; y++) memset(ssZ + y * SS_N + bx0, 0xFF, (bx1 - bx0 + 1) * 2);

  int k = clampi((int)(128 + pbSin * 300), 0, 256);       // wing shading by bank, blended smoothly
  for (int i = 0; i < PL->np; i++) {
    const Prim &q = PL->prims[i];
    const uint8_t *cc = PL->cols[q.col];
    uint16_t col = planeCol(cc, planeShade);
    switch (q.type) {
      case PR_TRI:  ssTri(p[q.a], p[q.b], p[q.c], col); break;
      case PR_QUAD: ssTri(p[q.a], p[q.b], p[q.c], col); ssTri(p[q.a], p[q.c], p[q.d], col); break;
      case PR_LINE: ssLine(p[q.a], p[q.b], col); break;
      case PR_DOT:
        if (p[q.a].ok) for (int j = 0; j < 4; j++) ssPut(p[q.a].x + (j & 1), p[q.a].y + (j >> 1), p[q.a].z, col);
        break;
      case PR_WING_L: case PR_WING_R: {
        int kk = q.type == PR_WING_L ? k : 256 - k;
        int lit = lerpi(220, 290, kk);                    // 0.86x .. 1.13x
        col = rgb(min(255, cc[0] * lit >> 8) * planeShade >> 8, min(255, cc[1] * lit >> 8) * planeShade >> 8, min(255, cc[2] * lit >> 8) * planeShade >> 8);
        ssTri(p[q.a], p[q.b], p[q.c], col); ssTri(p[q.a], p[q.c], p[q.d], col);
        break;
      }
      case PR_PROP: {                                     // faint see-through disc, a true circle in 3D
        const float *hub = PL->verts[q.a], *rim = PL->verts[q.b];
        float pr = sqrtf((rim[1] - hub[1]) * (rim[1] - hub[1]) + (rim[2] - hub[2]) * (rim[2] - hub[2]));
        const int NS = 12;
        ScreenPt ring[NS];
        for (int j = 0; j < NS; j++) {
          float an = j * 6.2831853f / NS;
          ring[j] = ssProject(hub[0], hub[1] + pr * cosf(an), hub[2] + pr * sinf(an));
        }
        ssStipple = true;
        for (int j = 0; j < NS; j++) ssTri(p[q.a], ring[j], ring[(j + 1) % NS], col);
        ssStipple = false;
        break;
      }
      case PR_GLOW:
        if (p[q.a].ok) {
          uint16_t g = (tick & 2) ? rgb(cc[0], cc[1], cc[2]) : rgb(255, 230, 150);
          for (int j = 0; j < 4; j++) ssPut(p[q.a].x + (j & 1), p[q.a].y + (j >> 1), p[q.a].z, g);
        }
        break;
    }
  }
  ssResolve(bx0, by0, bx1, by1);

  // navigation lights (dusk and night), on top at full brightness
  if (dl.glow > 60) {
    int a = min(256, (dl.glow - 60) * 2);
    const ScreenPt &l = p[PL->tipL], &r = p[PL->tipR], &s = p[PL->strobe];
    if (l.ok) { int x = ssOX + (l.x >> 1), y = ssOY + (l.y >> 1); if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = blend565(fb[y * W + x], 255, 40, 40, a); }
    if (r.ok) { int x = ssOX + (r.x >> 1), y = ssOY + (r.y >> 1); if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = blend565(fb[y * W + x], 40, 255, 80, a); }
    if ((tick % 45) < 2 && s.ok) px(ssOX + (s.x >> 1), ssOY + (s.y >> 1), rgb(255, 255, 255));
  }
}

// Wingtip trails: a faint white streak from each wingtip, invisible in level flight and
// stronger the harder the plane banks; each point fades as it ages.
const int TRAIL_N = 40;
int32_t trX[2][TRAIL_N], trY[2][TRAIL_N];
float trH[2][TRAIL_N];
uint8_t trA[2][TRAIL_N];
int trHead = 0;

void updateTrails() {
  float s = fabsf(bank) / PL->maxBank;
  int a = (int)(200 * clampf((s - 0.12f) / 0.6f, 0, 1));
  a = max(a, (int)(210 * trickGlow));                     // aerobatics draw them in the sky
  for (int side = 0; side < 2; side++) {
    const float *v = PL->verts[side ? PL->tipR : PL->tipL];
    planePoint(v[0], v[1], v[2], trX[side][trHead], trY[side][trHead], trH[side][trHead]);
    trA[side][trHead] = a;
  }
  trHead = (trHead + 1) % TRAIL_N;
}

void blendLine(float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
  int n = (int)max(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
  if (n > 120) return;
  int32_t x = (int32_t)(x0 * 65536), y = (int32_t)(y0 * 65536);
  int32_t dx = (int32_t)((x1 - x0) * 65536) / n, dy = (int32_t)((y1 - y0) * 65536) / n;
  for (int k = 0; k < n; k++, x += dx, y += dy) {
    int sx = x >> 16, sy = y >> 16;
    if ((unsigned)sx < W && (unsigned)sy < H) fb[sy * W + sx] = blend565(fb[sy * W + sx], r, g, b, a);
  }
}

void drawTrails() {
  int r = 235 * planeShade >> 8, g = 240 * planeShade >> 8, b = 250 * planeShade >> 8;
  for (int side = 0; side < 2; side++) {
    float x0 = 0, y0 = 0, z0;
    bool ok0 = false;                                     // each point is projected once
    for (int k = 0; k < TRAIL_N - 1; k++) {
      int i1 = (trHead - 1 - k + 2 * TRAIL_N) % TRAIL_N, i2 = (i1 - 1 + TRAIL_N) % TRAIL_N;
      int a = trA[side][i1] * (TRAIL_N - 1 - k) / TRAIL_N;
      if (a < 6) { ok0 = false; continue; }
      if (!ok0) ok0 = project(trX[side][i1], trY[side][i1], trH[side][i1], x0, y0, z0) && z0 >= 2.5f;
      float x1, y1, z1;
      bool ok1 = project(trX[side][i2], trY[side][i2], trH[side][i2], x1, y1, z1) && z1 >= 2.5f;
      if (ok0 && ok1) blendLine(x0, y0, x1, y1, r, g, b, a);
      x0 = x1; y0 = y1; ok0 = ok1;
    }
  }
}

// Shadow settings for this frame (the shadow itself is drawn by renderTerrain)
void setupShadow() {
  int32_t sx, sy; float sh;
  planePoint(0, 0, 0, sx, sy, sh);                         // the plane itself, not its path
  int agl = (int)sh - groundAt(sx, sy);
  shadowK = (dl.day * clampi(160 - agl * 2, 0, 140)) >> 8;   // darker when low, fades out with height
  // ...and fades out during aerobatics: pointing straight up or down, the plane's shadow is just
  // a thin hard bar that looks like it's floating (the footprint doesn't follow pitch or roll)
  shadowK = (shadowK * (256 - (int)(256 * min(1.0f, trickGlow * 2.5f)))) >> 8;
  shadowOn = shadowK > 8;
  shX = (sx & 0xFFFF) << 8;
  shY = (sy & 0xFFFF) << 8;
  shFX = (int32_t)(pFX * 16384);
  shFY = (int32_t)(pFY * 16384);
  float span = 0;
  for (int v = 0; v < PL->nv; v++) span = max(span, fabsf(PL->verts[v][1]));
  shSpan = (int32_t)(span * fabsf(pbCos) * 256);
  shTail = (int32_t)(1.2f * fabsf(pbCos) * 256);
}

// ---------- selection screen ----------
// Each plane spins on a turntable under a soft spotlight with its name and three stat bars
// (speed, agility, cruising height). Press BOOT to fly the one on screen; after all three have
// been shown, one is picked at random.
bool buttonPressed() { return s_btn; }

void drawStatBar(int y, const char *label, int value, uint16_t col) {
  drawText(8, y, label, rgb(150, 150, 160));
  for (int k = 0; k < 5; k++) {
    uint16_t c = k < value ? col : rgb(40, 40, 48);
    for (int yy = 1; yy < 6; yy++) for (int xx = 0; xx < 4; xx++) px(29 + k * 6 + xx, y + yy, c);
  }
}

void showcaseFrame(int idx, int f, bool chosen) {
  PL = &PLANES[idx];
  tick++;
  heading = 0.9f + f * 0.045f;                           // turntable
  bank = 0.12f * sinf(f * 0.07f);
  climb = 0;
  planeBasis();
  memset(fb, 0, (size_t)W * (size_t)H * sizeof(uint16_t));
  for (int i = 0; i < W; i++) ybuf[i] = H;
  // spotlight on the floor
  int spot[3] = { 55, 65, 85 };
  skyBlob((W / 2) * 16, (H * 35 / 64) * 16, (W * 28 / 64) * 16, (H * 5 / 64) * 16, spot, spot, 200);
  drawPlane();
  const uint8_t *nc = PL->cols[0];
  uint16_t nameCol = rgb(max(nc[0], (uint8_t)120), max(nc[1], (uint8_t)120), max(nc[2], (uint8_t)120));
  if (!chosen || (f / 6) % 2 == 0) drawText((W - textWidth(PL->name)) / 2, 1, PL->name, nameCol);
  for (int k = 0; k < 3; k++) {                          // which plane of three
    uint16_t c = k == idx ? rgb(255, 255, 255) : rgb(60, 60, 70);
    px(56 + k * 3, 3, c); px(57 + k * 3, 3, c);
  }
  uint16_t bar = rgb(255, 200, 60);
  drawStatBar(40, "SPD", PL->statSpeed, bar);            // 7-pixel rows with a 1-pixel gap, the last
  drawStatBar(48, "AGI", PL->statAgility, bar);          // ending on row 62
  drawStatBar(56, "ALT", PL->statAlt, bar);
}

int runSelection() {
  int chosen = FORCE_PLANE >= 0 ? FORCE_PLANE : (int)(esp_random() % 3);
  PL = &PLANES[chosen];
  Serial.printf("Plane: %s\n", PLANES[chosen].name);
  return chosen;
}


// ---------- setup and main loop ----------
void buildTables() {
  KX = (W * 0.5f) / TAN_HALF_FOV;
  KY = KX / 2;
  HORIZON_Y = max(8, H * 20 / 64);
  for (int i = 0; i < 256; i++) sin8[i] = 128 + 127.0f * sinf(i * 6.2831853f / 256.0f);
  for (int i = 0; i < 32; i++) { float f = i / 32.0f; smooth32[i] = (uint8_t)min(255.0f, 256 * f * f * (3 - 2 * f)); }
  buildMatLit();
  buildRenderTables();
}

void generateWorld() {
  worldSeed = esp_random();
  randomSeed(worldSeed);
  for (int k = 0; k < 8; k++) { seedOX[k] = random(0, 30000); seedOY[k] = random(0, 30000); }
  for (int i = 0; i < NUM_BIOMES; i++) biomeOrder[i] = i;
  for (int i = NUM_BIOMES - 1; i > 0; i--) { int j = random(i + 1); uint8_t t = biomeOrder[i]; biomeOrder[i] = biomeOrder[j]; biomeOrder[j] = t; }
  for (int k = 0; k < 4; k++) wanderPhase[k] = random(0, 628) / 100.0f;

  for (int i = 0; i < NUM_STARS; i++) {
    starAz[i] = random(0, 6283) / 1000.0f;
    starEl[i] = random(0, 24);
    starTw[i] = random(256);
    starHue[i] = random(3);
  }
  makeClouds();
  sunAz0 = random(0, 6283) / 1000.0f;
  windDir = random(0, 628) / 100.0f;
  moonPhase0 = random(0, 1000) / 1000.0f;                // a random moon phase to start with
  Serial.printf("Moon phase at boot %.2f (0 new, 0.5 full)\n", moonPhase0);

  // Start somewhere far from the origin (so coordinates never go negative), facing a random
  // way; if there's a city nearby, start a little way out and fly towards it.
  int sx = 100000 + random(0, 50000), sy = 100000 + random(0, 50000);
  heading = random(0, 628) / 100.0f;
  int cx, cy;
  uint32_t t0 = millis();
  if (findCity(sx, sy, 4, false, 0, cx, cy)) {
    sx = cx - (int)(cosf(heading) * 200);
    sy = cy - (int)(sinf(heading) * 200);
    cityTarget = true; cityX = cx; cityY = cy;
    Serial.printf("Starting 200 cells from a city (search took %lu ms)\n", (unsigned long)(millis() - t0));
  }
  nextCitySearch = CITY_SEARCH_FRAMES;
  planeX = (int32_t)sx << 8;
  planeY = (int32_t)sy << 8;
  bank = climb = turnRate = 0;
  camA = heading;
  updateCamera();
  shear = 0;

  Serial.printf("World seed %08lX, biome bands:", (unsigned long)worldSeed);
  for (int i = 0; i < NUM_BIOMES; i++) Serial.printf(" %s", BIOME_NAME[biomeOrder[i]]);
  Serial.println();

  t0 = millis();
  mapTarget(mapOX, mapOY);
  for (int y = mapOY; y < mapOY + MAP_N; y++) genRow(y, y > mapOY);
  Serial.printf("Initial map %dx%d generated in %lu ms\n", MAP_N, MAP_N, (unsigned long)(millis() - t0));

  float ch = cosf(heading), sh = sinf(heading);
  int maxAhead = 0;
  for (int d = 0; d <= 54; d += 3) maxAhead = max(maxAhead, groundAhead(ch, sh, d));
  planeAlt = altTarget = maxAhead + PL->clrMid;
  camH = planeAlt + PL->camUp;

  updateDayLight();
  for (int f = 0; f < 16; f++) buildFogLevel(f);
}

static bool s_live = false;
static uint32_t s_nextFrame = 0;

bool voxelHostBegin(int w, int h) {
  if (s_live) return true;
  if (w < 64) w = 64;
  if (h < 64) h = 64;
  if (w > FB_MAX_W) w = FB_MAX_W;
  if (h > FB_MAX_H) h = FB_MAX_H;
  W = w;
  H = h;
  Serial.printf("Voxel Flight %dx%d\n", W, H);

  const size_t px = (size_t)W * (size_t)H;
  if (fb) { heap_caps_free(fb); fb = nullptr; }
  if (zb) { heap_caps_free(zb); zb = nullptr; }
  fb = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  zb = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fb || !zb) {
    Serial.println(F("voxel: no PSRAM for framebuffer"));
    if (fb) { heap_caps_free(fb); fb = nullptr; }
    if (zb) { heap_caps_free(zb); zb = nullptr; }
    return false;
  }
  memset(fb, 0, px * sizeof(uint16_t));
  memset(zb, 255, px);

  if (!hmap || !cmap) {
    const size_t n = (size_t)MAP_N * (size_t)MAP_N;
    if (!hmap) hmap = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmap) cmap = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hmap || !cmap) {
      Serial.println(F("voxel: no PSRAM for map"));
      return false;
    }
    memset(hmap, 0, n);
    memset(cmap, 0, n);
  }
  buildTables();
  memset(ssZ, 0xFF, sizeof(ssZ));
  int pick = runSelection();
  if (FORCE_PLANE >= 0) pick = FORCE_PLANE;
  PL = &PLANES[pick];
  tick = 0;
  generateWorld();
  s_nextFrame = millis();
  s_live = true;
  return true;
}

void voxelHostEnd(void) {
  s_live = false;
  s_btn = false;
  s_steer = 0;
  // Keep map + fb for the next open (PSRAM is plentiful; avoids re-alloc churn).
}

uint16_t* voxelHostFramebuffer(void) { return fb; }
int voxelHostWidth(void) { return W; }
int voxelHostHeight(void) { return H; }

bool voxelHostTick(void) {
  if (!s_live) return false;
  uint32_t now = millis();
  if ((int32_t)(now - s_nextFrame) < 0) return true;
  s_nextFrame += FRAME_MS;
  if ((int32_t)(now - s_nextFrame) > (int32_t)FRAME_MS) s_nextFrame = now + FRAME_MS;

  updateDayLight();
  buildFogLevel(fogLevelNext);
  fogLevelNext = (fogLevelNext + 1) & 15;
  updateFlight();
  int tox, toy;
  mapTarget(tox, toy);
  int lag = max(abs(tox - mapOX), abs(toy - mapOY));
  streamMap(tox, toy, lag > 26 ? GEN_LINES_PER_FRAME : 1);
  scanBridges();
  updateBalloon();

  shear = -tanf(camRoll);
  if (revealSteps < numSteps) revealSteps += 3;
  planeBasis();
  updateTrails();
  setupShadow();
  renderTerrain();
  drawSky();
  drawBridges();
  drawBalloon();
  drawTrails();
  drawPlane();
  blit();
  return true;
}
