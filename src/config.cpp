#include "config.hpp"

#include "log.hpp"
#include "profiles.hpp"
#include "version_build.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace
{

bool EqI(const std::wstring& a, const wchar_t* b)
{
  return _wcsicmp(a.c_str(), b) == 0;
}

bool ParseBool(const std::wstring& v, bool* out)
{
  if (EqI(v, L"1") || EqI(v, L"true") || EqI(v, L"yes") || EqI(v, L"on"))
  {
    *out = true;
    return true;
  }
  if (EqI(v, L"0") || EqI(v, L"false") || EqI(v, L"no") || EqI(v, L"off"))
  {
    *out = false;
    return true;
  }
  return false;
}

bool ParseInt(const std::wstring& v, int* out)
{
  wchar_t* end = nullptr;
  const long n = wcstol(v.c_str(), &end, 10);
  if (end == v.c_str() || (end && *end != L'\0'))
    return false;
  *out = static_cast<int>(n);
  return true;
}

bool ParseMode(const std::wstring& v, WindowMode* out)
{
  if (EqI(v, L"windowed"))
  {
    *out = WindowMode::Windowed;
    return true;
  }
  if (EqI(v, L"borderless"))
  {
    *out = WindowMode::Borderless;
    return true;
  }
  if (EqI(v, L"fullscreen-exclusive") || EqI(v, L"fullscreen") || EqI(v, L"exclusive"))
  {
    *out = WindowMode::FullscreenExclusive;
    return true;
  }
  return false;
}

bool ParseApi(const std::wstring& v, GraphicsApi* out)
{
  if (EqI(v, L"d3d11") || EqI(v, L"dx11"))
  {
    *out = GraphicsApi::D3D11;
    return true;
  }
  if (EqI(v, L"d3d12") || EqI(v, L"dx12"))
  {
    *out = GraphicsApi::D3D12;
    return true;
  }
  if (EqI(v, L"vulkan") || EqI(v, L"vk"))
  {
    *out = GraphicsApi::Vulkan;
    return true;
  }
  if (EqI(v, L"d3d9") || EqI(v, L"dx9"))
  {
    *out = GraphicsApi::D3D9;
    return true;
  }
  if (EqI(v, L"opengl") || EqI(v, L"gl"))
  {
    *out = GraphicsApi::OpenGL;
    return true;
  }
  if (EqI(v, L"none") || EqI(v, L"gdi"))
  {
    *out = GraphicsApi::None;
    return true;
  }
  return false;
}

// Presets only: 0, 100, 500, 1024 (1gb), 2048 (2gb). Accepts 100mb / 1g / 1gb forms.
bool ParseGpuMemMb(const std::wstring& v, int* out)
{
  if (EqI(v, L"0") || EqI(v, L"off") || EqI(v, L"none"))
  {
    *out = 0;
    return true;
  }
  if (EqI(v, L"100") || EqI(v, L"100mb") || EqI(v, L"100m"))
  {
    *out = 100;
    return true;
  }
  if (EqI(v, L"500") || EqI(v, L"500mb") || EqI(v, L"500m"))
  {
    *out = 500;
    return true;
  }
  if (EqI(v, L"1024") || EqI(v, L"1gb") || EqI(v, L"1g"))
  {
    *out = 1024;
    return true;
  }
  if (EqI(v, L"2048") || EqI(v, L"2gb") || EqI(v, L"2g"))
  {
    *out = 2048;
    return true;
  }
  return false;
}

bool ApplyKeyValue(Config* c, const std::wstring& key, const std::wstring& value,
                   std::wstring* error)
{
  if (EqI(key, L"title"))
  {
    c->title = value;
    return true;
  }
  if (EqI(key, L"class") || EqI(key, L"window_class"))
  {
    c->windowClass = value;
    return true;
  }
  if (EqI(key, L"width"))
  {
    if (!ParseInt(value, &c->width) || c->width < 64)
    {
      *error = L"invalid width";
      return false;
    }
    return true;
  }
  if (EqI(key, L"height"))
  {
    if (!ParseInt(value, &c->height) || c->height < 64)
    {
      *error = L"invalid height";
      return false;
    }
    return true;
  }
  if (EqI(key, L"mode"))
  {
    if (!ParseMode(value, &c->mode))
    {
      *error = L"invalid mode (windowed|borderless|fullscreen-exclusive)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"api"))
  {
    if (!ParseApi(value, &c->api))
    {
      *error = L"invalid api (d3d11|d3d12|vulkan|none)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"fps"))
  {
    if (!ParseInt(value, &c->fps) || c->fps < 1 || c->fps > 1000)
    {
      *error = L"invalid fps";
      return false;
    }
    return true;
  }
  if (EqI(key, L"vsync"))
  {
    if (!ParseBool(value, &c->vsync))
    {
      *error = L"invalid vsync";
      return false;
    }
    return true;
  }
  if (EqI(key, L"flip-model") || EqI(key, L"flip_model"))
  {
    if (!ParseBool(value, &c->flipModel))
    {
      *error = L"invalid flip-model";
      return false;
    }
    return true;
  }
  if (EqI(key, L"buffers"))
  {
    if (!ParseInt(value, &c->buffers) || c->buffers < 2 || c->buffers > 8)
    {
      *error = L"invalid buffers (2-8)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"exit-after") || EqI(key, L"exit_after"))
  {
    if (!ParseInt(value, &c->exitAfterSeconds) || c->exitAfterSeconds < 0)
    {
      *error = L"invalid exit-after";
      return false;
    }
    return true;
  }
  if (EqI(key, L"topmost"))
  {
    if (!ParseBool(value, &c->topmost))
    {
      *error = L"invalid topmost";
      return false;
    }
    return true;
  }
  if (EqI(key, L"no-hud") || EqI(key, L"no_hud"))
  {
    if (!ParseBool(value, &c->noHud))
    {
      *error = L"invalid no-hud";
      return false;
    }
    return true;
  }
  if (EqI(key, L"gpu-mem") || EqI(key, L"gpu_mem") || EqI(key, L"gpu-mem-mb") ||
      EqI(key, L"gpu_mem_mb") || EqI(key, L"vram") || EqI(key, L"vram-mb"))
  {
    if (!ParseGpuMemMb(value, &c->gpuMemMb))
    {
      *error = L"invalid gpu-mem (100|500|1024|2048 or 100mb|500mb|1gb|2gb)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"block-capture") || EqI(key, L"block_capture"))
  {
    if (!ParseBlockCaptureMode(value, &c->blockCapture))
    {
      *error = L"invalid block-capture (none|signature-policy|squat-ipc|unload-hook)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"block-capture-after") || EqI(key, L"block_capture_after"))
  {
    if (!ParseInt(value, &c->blockCaptureAfterSeconds) || c->blockCaptureAfterSeconds < 0)
    {
      *error = L"invalid block-capture-after";
      return false;
    }
    return true;
  }

  *error = L"unknown config key: " + key;
  return false;
}

bool LoadIni(const std::wstring& path, Config* c, std::wstring* error)
{
  std::ifstream in(path);
  if (!in)
  {
    *error = L"cannot open config file: " + path;
    return false;
  }

  std::string line;
  int lineNo = 0;
  while (std::getline(in, line))
  {
    ++lineNo;
    if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
    {
      line.erase(0, 3);
    }

    const auto hash = line.find(';');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    const auto hash2 = line.find('#');
    if (hash2 != std::string::npos)
      line = line.substr(0, hash2);

    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
      ++b;
    line = line.substr(b);
    if (line.empty() || line.front() == '[')
      continue;

    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    std::string k = line.substr(0, eq);
    std::string v = line.substr(eq + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
      k.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
      v.erase(v.begin());
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
      v = v.substr(1, v.size() - 2);

    const int kn = MultiByteToWideChar(CP_UTF8, 0, k.c_str(), -1, nullptr, 0);
    const int vn = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, nullptr, 0);
    std::wstring wk(static_cast<size_t>(kn > 0 ? kn - 1 : 0), L'\0');
    std::wstring wv(static_cast<size_t>(vn > 0 ? vn - 1 : 0), L'\0');
    if (kn > 1)
      MultiByteToWideChar(CP_UTF8, 0, k.c_str(), -1, wk.data(), kn);
    if (vn > 1)
      MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, wv.data(), vn);

    std::wstring err;
    if (!ApplyKeyValue(c, wk, wv, &err))
    {
      *error = L"config line " + std::to_wstring(lineNo) + L": " + err;
      return false;
    }
  }
  return true;
}

bool ConsumeValue(int argc, wchar_t** argv, int* i, std::wstring* out, std::wstring* error,
                  const wchar_t* flag)
{
  if (*i + 1 >= argc)
  {
    *error = std::wstring(L"missing value for ") + flag;
    return false;
  }
  ++(*i);
  *out = argv[*i];
  return true;
}

struct FlagSeen
{
  bool title = false;
  bool cls = false;
  bool block = false;
};

} // namespace

const wchar_t* WindowModeName(WindowMode m)
{
  switch (m)
  {
  case WindowMode::Windowed:
    return L"windowed";
  case WindowMode::Borderless:
    return L"borderless";
  case WindowMode::FullscreenExclusive:
    return L"fullscreen-exclusive";
  }
  return L"?";
}

const wchar_t* GraphicsApiName(GraphicsApi a)
{
  switch (a)
  {
  case GraphicsApi::D3D11:
    return L"d3d11";
  case GraphicsApi::D3D12:
    return L"d3d12";
  case GraphicsApi::Vulkan:
    return L"vulkan";
  case GraphicsApi::D3D9:
    return L"d3d9";
  case GraphicsApi::OpenGL:
    return L"opengl";
  case GraphicsApi::None:
    return L"none";
  }
  return L"?";
}

WindowMode NextWindowMode(WindowMode m)
{
  switch (m)
  {
  case WindowMode::Windowed:
    return WindowMode::Borderless;
  case WindowMode::Borderless:
    return WindowMode::FullscreenExclusive;
  case WindowMode::FullscreenExclusive:
    return WindowMode::Windowed;
  }
  return WindowMode::Windowed;
}

void PrintConfig(const Config& c)
{
  Log("=== resolved config ===");
  Log("  version     = %s", FG_VERSION_STRING);
  Log("  git         = %s dirty=%d", FG_GIT_HASH, FG_GIT_DIRTY);
  if (!c.profileId.empty())
    Log("  profile     = %s", c.profileId.c_str());
  if (!c.profileExeName.empty())
    Log("  profile-exe = %s (rename via launch.ps1 / spawn-as.ps1)", c.profileExeName.c_str());
  Log("  title       = %s", Narrow(c.title).c_str());
  Log("  class       = %s", Narrow(c.windowClass).c_str());
  Log("  size        = %dx%d", c.width, c.height);
  Log("  mode        = %s", Narrow(WindowModeName(c.mode)).c_str());
  Log("  api         = %s", Narrow(GraphicsApiName(c.api)).c_str());
  Log("  fps         = %d", c.fps);
  Log("  vsync       = %d", c.vsync ? 1 : 0);
  Log("  flip-model  = %d", c.flipModel ? 1 : 0);
  Log("  buffers     = %d", c.buffers);
  Log("  exit-after  = %d", c.exitAfterSeconds);
  Log("  topmost     = %d", c.topmost ? 1 : 0);
  Log("  no-hud      = %d", c.noHud ? 1 : 0);
  Log("  scene       = %s", Narrow(scene::SceneIdName(c.scene)).c_str());
  Log("  scene-seed  = 0x%08X (%u)", c.sceneSeed, c.sceneSeed);
  if (!c.dumpFramePath.empty())
    Log("  dump-frame  = %s", Narrow(c.dumpFramePath).c_str());
  Log("  gpu-mem-mb  = %d", c.gpuMemMb);
  Log("  block-capture = %s", Narrow(BlockCaptureModeName(c.blockCapture)).c_str());
  Log("  block-after = %d", c.blockCaptureAfterSeconds);
  Log("  show-block-errors = %d", c.showBlockErrors ? 1 : 0);
  Log("  capture-expected = %s", c.captureExpected ? "yes" : "NO");
  Log("=======================");
}

void PrintHelp()
{
  std::printf(R"(fakegame — configurable Win32 capture test app for OBS / Streamlabs Desktop

Usage: fakegame.exe [options]

Options:
  --title <string>          Window title (default: Fake Game)
  --class <string>          Window class (default: FakeGameWindowClass)
  --width <n>               Client width (default: 1280)
  --height <n>              Client height (default: 720)
  --mode <mode>             windowed | borderless | fullscreen-exclusive
  --api <api>               d3d11 | d3d12 | vulkan | none
  --fps <n>                 Frame pacing target (default: 60)
  --vsync <0|1>             (default: 1)
  --flip-model <0|1>        FLIP_DISCARD vs DISCARD (d3d11/d3d12; default: 1)
  --buffers <n>             Swapchain buffer count (default: 2)
  --exit-after <seconds>    Auto-quit (default: 0 = never)
  --topmost                 WS_EX_TOPMOST
  --no-hud                  Hide on-screen HUD
  --scene <name>            Visual stress scene (default: aurora)
  --scene-seed <u32>        Deterministic scene RNG seed (default: 0xC5A2EE)
  --list-scenes             Print scene table
  --dump-frame <path.bmp>   d3d11: write one framebuffer BMP after a few frames
  --gpu-mem <size>          Hold GPU RAM: 100|500|1024|2048 or 100mb|500mb|1gb|2gb
  --config <path>           INI file; flags override file values
  --profile <name>          Apply game profile (title/class/block defaults)
  --list-profiles           Print profile table
  --list-profiles --json    Print profiles as JSON (consumed by tools/launch.ps1)
  --block-capture <mode>    none | signature-policy | squat-ipc | unload-hook
  --block-capture-after <s> Delay before applying block (0 = immediate)
  --show-block-errors       Do not suppress Windows loader hard-error dialogs
  --version                 Print version string and exit
  --help                    This help

Scenes:
  aurora   Nebula, orb rings, comet, EQ bars (default freeze-tell)
  orbital  Drones vs asteroids, particles, explosions, screen flash
  highway  Night neon highway: perspective road, traffic, speedo
  fractal  Fullscreen twigl-style raymarch (seed picks variant A/B)

Capture blocking:
  signature-policy  SetProcessMitigationPolicy(MicrosoftSignedOnly) AFTER the
                    renderer is fully up. Blocks unsigned graphics-hook*.dll.
                    IRREVERSIBLE for the process — cannot be toggled off.
                    By default loader "Bad Image" dialogs are suppressed
                    (SetErrorMode); use --show-block-errors to see them.
  squat-ipc         Pre-create OBS hook IPC objects (CaptureHook_* + pid) with
                    an empty DACL so hook init fails. Reversible (F7).
                    Default mechanism for profile cs2-blocked (silent).
  unload-hook       Poll for graphics-hook*.dll and FreeLibrary it. Reversible (F7).

Hotkeys:
  F1  Cycle windowed / borderless / fullscreen-exclusive
  F2  Resize swapchain through preset resolutions
  F3  Destroy + recreate swapchain
  F4  Destroy + recreate device
  F5  Change window title (append counter)
  F6  Toggle churn mode (~2 Hz resize/recreate)
  F7  Toggle reversible capture block (squat-ipc / unload-hook)
  Esc Quit

Rename-friendly: behaviour never depends on the exe filename.
Use tools\launch.ps1 or tools\spawn-as.ps1 to copy+launch as cs2.exe etc.
)");
}

void PrintVersion()
{
  // Single line for scripts: bare version+ghash[.dirty]
  std::printf("%s\n", FG_VERSION_STRING);
}

bool ParseConfig(int argc, wchar_t** argv, Config* out, std::wstring* error)
{
  Config c;
  FlagSeen seen;

  // Pass 1: --config, --list-profiles, --list-scenes, --help, --version, --profile id
  std::wstring configPath;
  std::wstring profileId;
  for (int i = 1; i < argc; ++i)
  {
    if (EqI(argv[i], L"--config"))
    {
      if (i + 1 >= argc)
      {
        *error = L"missing value for --config";
        return false;
      }
      configPath = argv[++i];
    } else if (EqI(argv[i], L"--profile"))
    {
      if (i + 1 >= argc)
      {
        *error = L"missing value for --profile";
        return false;
      }
      profileId = argv[++i];
    } else if (EqI(argv[i], L"--list-profiles"))
    {
      c.listProfiles = true;
    } else if (EqI(argv[i], L"--list-scenes"))
    {
      c.listScenes = true;
    } else if (EqI(argv[i], L"--json"))
    {
      c.listProfilesJson = true;
    } else if (EqI(argv[i], L"--version") || EqI(argv[i], L"-V"))
    {
      c.version = true;
    } else if (EqI(argv[i], L"--help") || EqI(argv[i], L"-h") || EqI(argv[i], L"/?"))
    {
      c.help = true;
    }
  }

  if (c.help)
  {
    *out = std::move(c);
    return true;
  }
  if (c.version)
  {
    *out = std::move(c);
    return true;
  }
  if (c.listProfiles)
  {
    *out = std::move(c);
    return true;
  }
  if (c.listScenes)
  {
    *out = std::move(c);
    return true;
  }

  if (!configPath.empty())
  {
    if (!LoadIni(configPath, &c, error))
      return false;
  }

  if (!profileId.empty())
  {
    const Profile* p = FindProfile(profileId);
    if (!p)
    {
      *error = L"unknown profile: " + profileId + L" (use --list-profiles)";
      return false;
    }
    ApplyProfile(*p, &c);
  }

  // Pass 2: flags override everything
  for (int i = 1; i < argc; ++i)
  {
    const std::wstring a = argv[i];
    if (EqI(a, L"--config") || EqI(a, L"--profile"))
    {
      ++i;
      continue;
    }
    if (EqI(a, L"--list-profiles") || EqI(a, L"--list-scenes") || EqI(a, L"--json") ||
        EqI(a, L"--version") || EqI(a, L"-V") || EqI(a, L"--help") || EqI(a, L"-h") ||
        EqI(a, L"/?"))
    {
      continue;
    }
    if (EqI(a, L"--title"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--title"))
        return false;
      c.title = v;
      seen.title = true;
      continue;
    }
    if (EqI(a, L"--class"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--class"))
        return false;
      c.windowClass = v;
      seen.cls = true;
      continue;
    }
    if (EqI(a, L"--width"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--width"))
        return false;
      if (!ApplyKeyValue(&c, L"width", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--height"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--height"))
        return false;
      if (!ApplyKeyValue(&c, L"height", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--mode"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--mode"))
        return false;
      if (!ApplyKeyValue(&c, L"mode", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--api"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--api"))
        return false;
      if (!ApplyKeyValue(&c, L"api", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--fps"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--fps"))
        return false;
      if (!ApplyKeyValue(&c, L"fps", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--vsync"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--vsync"))
        return false;
      if (!ApplyKeyValue(&c, L"vsync", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--flip-model"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--flip-model"))
        return false;
      if (!ApplyKeyValue(&c, L"flip-model", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--buffers"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--buffers"))
        return false;
      if (!ApplyKeyValue(&c, L"buffers", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--exit-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--exit-after"))
        return false;
      if (!ApplyKeyValue(&c, L"exit-after", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--block-capture"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--block-capture"))
        return false;
      if (!ApplyKeyValue(&c, L"block-capture", v, error))
        return false;
      seen.block = true;
      continue;
    }
    if (EqI(a, L"--block-capture-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--block-capture-after"))
        return false;
      if (!ApplyKeyValue(&c, L"block-capture-after", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--topmost"))
    {
      c.topmost = true;
      continue;
    }
    if (EqI(a, L"--no-hud"))
    {
      c.noHud = true;
      continue;
    }
    if (EqI(a, L"--scene"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--scene"))
        return false;
      if (!scene::ParseSceneId(v, &c.scene))
      {
        *error = L"invalid scene (use --list-scenes)";
        return false;
      }
      continue;
    }
    if (EqI(a, L"--scene-seed"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--scene-seed"))
        return false;
      wchar_t* end = nullptr;
      // Accept hex (0x…) or decimal.
      const unsigned long n = wcstoul(v.c_str(), &end, 0);
      if (end == v.c_str() || (end && *end != L'\0'))
      {
        *error = L"invalid scene-seed";
        return false;
      }
      c.sceneSeed = static_cast<uint32_t>(n);
      continue;
    }
    if (EqI(a, L"--dump-frame"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--dump-frame"))
        return false;
      c.dumpFramePath = v;
      continue;
    }
    if (EqI(a, L"--gpu-mem") || EqI(a, L"--gpu-mem-mb") || EqI(a, L"--vram") ||
        EqI(a, L"--vram-mb"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--gpu-mem"))
        return false;
      if (!ParseGpuMemMb(v, &c.gpuMemMb))
      {
        *error = L"invalid gpu-mem (100|500|1024|2048 or 100mb|500mb|1gb|2gb)";
        return false;
      }
      continue;
    }
    if (EqI(a, L"--show-block-errors"))
    {
      c.showBlockErrors = true;
      continue;
    }

    *error = L"unknown argument: " + a;
    return false;
  }

  if (c.api == GraphicsApi::OpenGL || c.api == GraphicsApi::D3D9)
  {
    *error = std::wstring(L"api '") + GraphicsApiName(c.api) + L"' is not implemented yet";
    return false;
  }

  if (c.blockCapture == BlockCaptureMode::SignaturePolicy && c.blockCaptureAfterSeconds > 0)
  {
    Log("warn: signature-policy with --block-capture-after is one-way (cannot undo)");
  }

  // If profile set captureExpected based on block mode override
  if (seen.block)
  {
    c.captureExpected = (c.blockCapture == BlockCaptureMode::None);
  }

  *out = std::move(c);
  return true;
}
