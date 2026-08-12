#include "scene/scene.hpp"

#include <cstdio>

#include <Windows.h>

namespace scene
{

// Defined in scene_*.cpp
std::unique_ptr<IScene> CreateAuroraScene();
std::unique_ptr<IScene> CreateOrbitalScene();
std::unique_ptr<IScene> CreateHighwayScene();
std::unique_ptr<IScene> CreateFractalScene();

const wchar_t* SceneIdName(SceneId id)
{
  switch (id)
  {
  case SceneId::Aurora:
    return L"aurora";
  case SceneId::Orbital:
    return L"orbital";
  case SceneId::Highway:
    return L"highway";
  case SceneId::Fractal:
    return L"fractal";
  }
  return L"?";
}

const wchar_t* BackdropIdName(BackdropId id)
{
  switch (id)
  {
  case BackdropId::Solid:
    return L"solid";
  case BackdropId::Aurora:
    return L"aurora-ps";
  case BackdropId::Starfield:
    return L"starfield";
  case BackdropId::FractalA:
    return L"fractal-a";
  case BackdropId::FractalB:
    return L"fractal-b";
  }
  return L"?";
}

const wchar_t* PrimKindName(PrimKind k)
{
  switch (k)
  {
  case PrimKind::QuadSolid:
    return L"solid";
  case PrimKind::QuadOrb:
    return L"orb";
  case PrimKind::QuadRing:
    return L"ring";
  case PrimKind::Line:
    return L"line";
  case PrimKind::Triangle:
    return L"tri";
  case PrimKind::CircleOutline:
    return L"circle";
  }
  return L"?";
}

PrimCounts CountPrims(const std::vector<Prim>& prims)
{
  PrimCounts c;
  c.total = static_cast<int>(prims.size());
  for (const auto& p : prims)
  {
    switch (p.kind)
    {
    case PrimKind::QuadSolid:
      ++c.solid;
      break;
    case PrimKind::QuadOrb:
      ++c.orb;
      break;
    case PrimKind::QuadRing:
      ++c.ring;
      break;
    case PrimKind::Line:
      ++c.line;
      break;
    case PrimKind::Triangle:
      ++c.tri;
      break;
    case PrimKind::CircleOutline:
      ++c.circle;
      break;
    }
  }
  return c;
}

bool ParseSceneId(const std::wstring& s, SceneId* out)
{
  if (_wcsicmp(s.c_str(), L"aurora") == 0)
  {
    *out = SceneId::Aurora;
    return true;
  }
  if (_wcsicmp(s.c_str(), L"orbital") == 0)
  {
    *out = SceneId::Orbital;
    return true;
  }
  if (_wcsicmp(s.c_str(), L"highway") == 0)
  {
    *out = SceneId::Highway;
    return true;
  }
  if (_wcsicmp(s.c_str(), L"fractal") == 0 || _wcsicmp(s.c_str(), L"raymarch") == 0)
  {
    *out = SceneId::Fractal;
    return true;
  }
  return false;
}

std::unique_ptr<IScene> CreateScene(SceneId id)
{
  switch (id)
  {
  case SceneId::Aurora:
    return CreateAuroraScene();
  case SceneId::Orbital:
    return CreateOrbitalScene();
  case SceneId::Highway:
    return CreateHighwayScene();
  case SceneId::Fractal:
    return CreateFractalScene();
  }
  return CreateAuroraScene();
}

void PrintSceneList()
{
  std::printf("Scenes are per-API. There is no shared visual across backends.\n");
  std::printf("d3d11 is the draw-list reference; d3d12/GDI approximate the same list.\n");
  std::printf("vulkan always uses its own default (cycling clear + title frame).\n");
  std::printf("\n");
  std::printf("  aurora   Nebula, orb rings, comet, EQ bars  [d3d11 d3d12 none]  default\n");
  std::printf("  orbital  Drones, asteroids, explosions      [d3d11 d3d12 none]\n");
  std::printf("  highway  Night neon highway + traffic       [d3d11 d3d12 none]\n");
  std::printf("  fractal  Twigl raymarch (seed even=A/odd=B) [d3d11; d3d12/none approx]\n");
  std::printf("\nUpcoming: skirmish, sonar\n");
  std::printf("Flags: --scene <name>  --scene-seed <u32>  --list-scenes\n");
}

} // namespace scene
