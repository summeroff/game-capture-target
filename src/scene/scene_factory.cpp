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
