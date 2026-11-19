// Types shared by voxel-flight.ino. They live in a header because the Arduino build inserts
// function prototypes near the top of the .ino, before any struct declared there.
#pragma once
#include <Arduino.h>

// Biomes. The world blends between them along a slow "biome" noise field; the order they
// appear in is shuffled by the world seed, so neighbours differ from boot to boot.
enum Biome : uint8_t { B_LAKES, B_MOUNTAINS, B_CANYON, B_FARMLAND, B_COAST, NUM_BIOMES };

// Ground materials. A map cell's colour index is (material << 3) | light (0..7).
enum Material : uint8_t {
  M_DEEP, M_SHALLOW, M_SAND, M_GRASS, M_FOREST, M_ROCK, M_SNOW,
  M_STRATA_A, M_STRATA_B, M_STRATA_C, M_DRY,
  M_WHEAT, M_PLOUGHED, M_CROP, M_HEDGE,
  M_ROAD, M_LAMP, M_PLAZA, M_TUNNEL,
  // everything from M_ROOF on is a building: drawn with crisp edges (square tops)
  M_ROOF, M_ROOF_LIT,
  // building walls, in pairs (plain, windows lit at night); facade kind k = M_CONCRETE + 2k (+1 lit)
  M_CONCRETE, M_CONCRETE_LIT, M_GLASS, M_GLASS_LIT, M_DARKGLASS, M_DARKGLASS_LIT, M_BRICK, M_BRICK_LIT,
  M_ANTENNA, M_ROOFTOP,
  NUM_MATERIALS   // at most 32 (colour index = material << 3 | light)
};

// Extra facts terrain() reports about a cell, used to pick its material
enum TerrainFlag : uint8_t { F_TREE = 1, F_FORCED = 2, F_LANE = 4 };   // F_LANE: a country road (kept level)

struct TerrainInfo {
  uint8_t biome;     // dominant biome
  uint8_t flags;     // TerrainFlag bits
  uint8_t forced;    // material, when F_FORCED (fields, roads, buildings)
  int16_t raw;       // height before water levelling (for deep/shallow water)
};

// Noise octaves shared by every biome's height formula (each 0..255)
struct Octaves { int a, b, c, d, e; };

// A screen point of the projected plane model (in supersampled pixels)
struct ScreenPt { int x, y, z; bool ok; };   // z = depth, 1/256 cell

// A projected world point with its distance, for depth-tested 3D objects (bridges)
struct ScreenPtZ { float x, y, z; bool ok; };

// Lighting for the current time of day (colours 0..255, factors 0..256)
struct DayLight {
  int skyH[3], skyZ[3];   // sky colour at the horizon (also the haze colour) and overhead
  int ground[3];          // multiplier for sunlit / moonlit ground
  int glow;               // town lights, 0 by day .. 256 at night
  int stars;              // star visibility
  int day;                // 0 night .. 256 full day
  int warm;               // sunrise / sunset strength
  int cloud[3];           // cloud colour
};


// Plane models: a list of 3D points and a list of drawing primitives that use them
enum PrimType : uint8_t {
  PR_TRI, PR_QUAD, PR_LINE, PR_DOT,     // a, b, c, d = point indices
  PR_WING_L, PR_WING_R,                 // quads shaded by bank (left / right wing)
  PR_PROP,                              // see-through propeller disc: hub a, radius = distance to b
  PR_GLOW                               // flickering engine glow at a (not dimmed at night)
};
struct Prim { uint8_t type, a, b, c, d, col; };

// One selectable plane: its model, colours and how it flies
struct PlaneDef {
  const char *name;
  const float (*verts)[3]; uint8_t nv;  // forward, right, up (cells)
  const Prim *prims; uint8_t np;        // drawn in order (back to front from the chase camera)
  const uint8_t (*cols)[3];             // colour slots used by the primitives
  int8_t tipL, tipR, strobe;            // points for the navigation lights
  float speed;                          // cells per frame
  float maxTurn;                        // radians per frame
  float bankPerTurn, maxBank;           // how far it leans into turns
  float clrMid, clrMin;                 // cruising height above the ground ahead
  float camBack, camUp;                 // chase camera
  uint8_t statSpeed, statAgility, statAlt;   // 1..5, for the selection screen
  uint8_t trick;                        // aerobatics now and then: TRICK_NONE / TRICK_LOOP / TRICK_ROLL
};
enum Trick : uint8_t { TRICK_NONE, TRICK_LOOP, TRICK_ROLL };

// A bridge carrying a city avenue over water (world cells, deck height in height units)
// A hot air balloon drifting on the wind (world cells; h = envelope centre in height units)
struct Balloon { float x, y, h, vx, vy; bool active; uint8_t look; };

// (x0, y0) is the city end. Each end has an approach ramp down to the ground: rampA cells back
// from (x0, y0) to height landA, rampB cells on from (x1, y1) to height landB.
struct Bridge { int32_t x0, y0, x1, y1; int deck; bool used; uint8_t style; uint8_t rampA, rampB; int16_t landA, landB; };
