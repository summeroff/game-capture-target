#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scene
{

enum class SceneId
{
  Aurora = 0,
  Orbital,
  // Highway, Skirmish, Sonar — later
};

enum class BackdropId
{
  Solid = 0, // clear to solid color
  Aurora,    // d3d11 procedural PS; others approximate
  Starfield, // dark space + CPU stars in draw list
};

// Immediate-mode primitives in pixel space (top-left origin, Y down).
enum class PrimKind
{
  QuadSolid = 0, // axis-aligned or rotated filled rect (center x,y; size w,h; rot radians)
  QuadOrb,       // soft circular orb (renderer may approximate)
  QuadRing,      // hollow disc
  Line,          // x,y → x2,y2; w = thickness
  Triangle,      // isosceles: center x,y; tip along +rot; base=w height=h
  CircleOutline, // center x,y; radius=w; thickness≈h
};

struct Prim
{
  PrimKind kind = PrimKind::QuadSolid;
  float x = 0.f;
  float y = 0.f;
  float rot = 0.f;
  float w = 1.f;
  float h = 1.f;
  float r = 1.f;
  float g = 1.f;
  float b = 1.f;
  float a = 1.f;
  float x2 = 0.f; // line end
  float y2 = 0.f;
};

struct SceneConfig
{
  SceneId id = SceneId::Aurora;
  uint32_t seed = 0x00C5A2EEu;
  float intensity = 1.f;
};

struct SceneHudExtras
{
  std::wstring line1;
  std::wstring line2;
};

struct SceneDraw
{
  BackdropId backdrop = BackdropId::Solid;
  float clearR = 0.01f;
  float clearG = 0.01f;
  float clearB = 0.03f;
  float flashR = 1.f;
  float flashG = 0.55f;
  float flashB = 0.2f;
  float flashA = 0.f; // additive fullscreen flash
  std::vector<Prim> prims;
  SceneHudExtras hud;
};

const wchar_t* BackdropIdName(BackdropId id);
const wchar_t* PrimKindName(PrimKind k);

// Tally prims for render-path diagnostics.
struct PrimCounts
{
  int total = 0;
  int solid = 0;
  int orb = 0;
  int ring = 0;
  int line = 0;
  int tri = 0;
  int circle = 0;
};
PrimCounts CountPrims(const std::vector<Prim>& prims);

class IScene
{
public:
  virtual ~IScene() = default;
  virtual void Reset(const SceneConfig& cfg, int w, int h) = 0;
  virtual void Resize(int w, int h) = 0;
  // elapsedSec = wall time since start; dt = frame delta. Prefer fixed-step inside.
  virtual void Update(double elapsedSec, float dt, int w, int h) = 0;
  virtual void Emit(SceneDraw& out) = 0;
  virtual const wchar_t* Name() const = 0;
  virtual SceneId Id() const = 0;
};

const wchar_t* SceneIdName(SceneId id);
bool ParseSceneId(const std::wstring& s, SceneId* out);
std::unique_ptr<IScene> CreateScene(SceneId id);

void PrintSceneList();

// Helpers used by scene implementations when packing prims.
inline Prim MakeOrb(float x, float y, float size, float r, float g, float b, float a)
{
  Prim p;
  p.kind = PrimKind::QuadOrb;
  p.x = x;
  p.y = y;
  p.w = size;
  p.h = size;
  p.r = r;
  p.g = g;
  p.b = b;
  p.a = a;
  return p;
}

inline Prim MakeRing(float x, float y, float sizeX, float sizeY, float rot, float r, float g,
                     float b, float a)
{
  Prim p;
  p.kind = PrimKind::QuadRing;
  p.x = x;
  p.y = y;
  p.rot = rot;
  p.w = sizeX;
  p.h = sizeY;
  p.r = r;
  p.g = g;
  p.b = b;
  p.a = a;
  return p;
}

inline Prim MakeSolid(float x, float y, float w, float h, float rot, float r, float g, float b,
                      float a)
{
  Prim p;
  p.kind = PrimKind::QuadSolid;
  p.x = x;
  p.y = y;
  p.rot = rot;
  p.w = w;
  p.h = h;
  p.r = r;
  p.g = g;
  p.b = b;
  p.a = a;
  return p;
}

inline Prim MakeLine(float x0, float y0, float x1, float y1, float thickness, float r, float g,
                     float b, float a)
{
  Prim p;
  p.kind = PrimKind::Line;
  p.x = x0;
  p.y = y0;
  p.x2 = x1;
  p.y2 = y1;
  p.w = thickness;
  p.h = thickness;
  p.r = r;
  p.g = g;
  p.b = b;
  p.a = a;
  return p;
}

inline Prim MakeTri(float x, float y, float base, float height, float rot, float r, float g,
                    float b, float a)
{
  Prim p;
  p.kind = PrimKind::Triangle;
  p.x = x;
  p.y = y;
  p.rot = rot;
  p.w = base;
  p.h = height;
  p.r = r;
  p.g = g;
  p.b = b;
  p.a = a;
  return p;
}

} // namespace scene
