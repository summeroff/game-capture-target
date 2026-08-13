#pragma once

#include "block_capture.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <string>

enum class WindowMode
{
  Windowed,
  Borderless,
  FullscreenExclusive,
};

enum class GraphicsApi
{
  D3D11,
  D3D12,
  Vulkan,
  D3D9,   // optional / not yet
  OpenGL, // reserved
  None,
};

// CLI arm for F1–F4: `--recreate-swapchain-after 5` or `5,repeat`.
struct ScheduledAfter
{
  double afterSec = 0; // 0 = off
  bool repeat = false;
};

struct Config
{
  std::wstring title = L"Fake Game";
  std::wstring windowClass = L"FakeGameWindowClass";
  int width = 1280;
  int height = 720;
  WindowMode mode = WindowMode::Windowed;
  GraphicsApi api = GraphicsApi::D3D11;
  int fps = 60;
  bool vsync = true;
  bool flipModel = true;
  int buffers = 2;
  int exitAfterSeconds = 0;
  bool topmost = false;
  bool noHud = false;
  bool verbose = false; // per-frame scene-emit / d3d*-draw / d3d*-hud (default off)

  ScheduledAfter recreateSwapchainAfter;
  ScheduledAfter recreateDeviceAfter;
  ScheduledAfter resizeAfter;
  ScheduledAfter modeCycleAfter;
  double churnHz = 0; // 0 = off; >0 arms F6-style churn at launch

  scene::SceneId scene = scene::SceneId::Aurora;
  uint32_t sceneSeed = 0x00C5A2EEu;
  std::wstring dumpFramePath; // if set, d3d11 writes one BMP after a few frames
  // Hold GPU memory for capture/VRAM stress tests. 0 = off.
  // Presets: 100, 500, 1024 (1GB), 2048 (2GB).
  int gpuMemMb = 0;

  BlockCaptureMode blockCapture = BlockCaptureMode::None;
  int blockCaptureAfterSeconds = 0; // 0 = apply immediately (if mode != none)
  bool showBlockErrors = false;     // if true, skip SetErrorMode suppression

  std::string profileId;
  std::string profileExeName; // profile's declared exe; rename is done by launch scripts
  // Profile identity before CLI overrides / --instance suffixes (for match warnings).
  std::wstring profileExpectedClass;
  std::wstring profileExpectedTitle;
  bool captureExpected = true;
  int profileSeverity = -1;
  int profileMatchFlags = 0; // EXE=1 TITLE=2 CLASS=4
  bool profileGameCapture = true;
  bool profileWindowCapture = false;

  // NDJSON events on stdout (human Log → stderr). ready also optional to file.
  bool eventsJson = false;
  std::wstring readyFile;

  // Disambiguate concurrent same-profile runs without breaking OBS match mode.
  // Appended to window class (exe-matched) or title (class-matched).
  std::wstring instanceId;

  bool listProfiles = false;
  bool listProfilesJson = false;
  bool listScenes = false;
  bool help = false;
  bool version = false;
};

// Parse argv. Flags override --profile.
bool ParseConfig(int argc, wchar_t** argv, Config* out, std::wstring* error);

const wchar_t* WindowModeName(WindowMode m);
const wchar_t* GraphicsApiName(GraphicsApi a);
WindowMode NextWindowMode(WindowMode m);

void PrintConfig(const Config& c);
void PrintHelp();
void PrintVersion();
