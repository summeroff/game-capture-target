#include "scene/scene.hpp"

#include <cstdio>

#include <Windows.h>

namespace scene
{

// Defined in scene_*.cpp
std::unique_ptr<IScene> CreateAuroraScene();
std::unique_ptr<IScene> CreateOrbitalScene();

const wchar_t* SceneIdName(SceneId id)
{
  switch (id)
  {
  case SceneId::Aurora:
    return L"aurora";
  case SceneId::Orbital:
    return L"orbital";
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
  }
  return CreateAuroraScene();
}

void PrintSceneList()
{
  std::printf("Available scenes:\n");
  std::printf("  aurora   Nebula backdrop, orb rings, comet, EQ bars (default)\n");
  std::printf("  orbital  Starfield defense: drones, asteroids, explosions, flash\n");
  std::printf("\nUpcoming: highway, skirmish, sonar\n");
  std::printf("Flags: --scene <name>  --scene-seed <u32>  --list-scenes\n");
}

} // namespace scene
