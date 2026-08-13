#include "config.hpp"

#include "log.hpp"
#include "profiles.hpp"
#include "version_build.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// "5" / "5,repeat" / "hooked" / "hooked+2" / "hooked+2,repeat".
bool ParseScheduledAfter(const std::wstring& v, ScheduledAfter* out, std::wstring* error)
{
  if (!out)
    return false;
  std::wstring body = v;
  bool repeat = false;
  const auto comma = v.find(L',');
  if (comma != std::wstring::npos)
  {
    body = v.substr(0, comma);
    const std::wstring extra = v.substr(comma + 1);
    if (extra.empty() || !EqI(extra, L"repeat"))
    {
      if (error)
        *error = L"invalid after suffix (use ,repeat)";
      return false;
    }
    repeat = true;
  }
  if (body.empty())
  {
    if (error)
      *error = L"invalid after-seconds (want >0, hooked, or hooked+N)";
    return false;
  }

  bool afterHooked = false;
  std::wstring sec = body;
  if (body.size() >= 6 && _wcsnicmp(body.c_str(), L"hooked", 6) == 0)
  {
    afterHooked = true;
    if (body.size() == 6)
    {
      out->afterSec = 0;
      out->repeat = repeat;
      out->afterHooked = true;
      return true;
    }
    if (body[6] != L'+')
    {
      if (error)
        *error = L"invalid hooked offset (use hooked or hooked+N)";
      return false;
    }
    sec = body.substr(7);
  }

  wchar_t* end = nullptr;
  const double n = wcstod(sec.c_str(), &end);
  if (end == sec.c_str() || (end && *end != L'\0') || !std::isfinite(n) || n < 0 ||
      (!afterHooked && n <= 0))
  {
    if (error)
      *error = L"invalid after-seconds (want >0, hooked, or hooked+N[,repeat])";
    return false;
  }
  out->afterSec = n;
  out->repeat = repeat;
  out->afterHooked = afterHooked;
  return true;
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

// MATCH_CLASS = 4. Class-only profiles: suffix goes on title so class still matches.
// Otherwise suffix goes on window class so exe/title match stays intact.
// No Log here — ParseConfig runs before --events redirects Log to stderr.
void ApplyInstanceId(Config* c)
{
  if (!c || c->instanceId.empty())
    return;

  const bool classOnly = (c->profileMatchFlags == 4) ||
                         ((c->profileMatchFlags & 4) != 0 && (c->profileMatchFlags & 3) == 0);
  if (classOnly)
  {
    c->title += L" [";
    c->title += c->instanceId;
    c->title += L"]";
  } else
  {
    c->windowClass += L"_";
    c->windowClass += c->instanceId;
  }
}

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
  Log("  verbose     = %d", c.verbose ? 1 : 0);
  if (c.recreateSwapchainAfter.afterSec > 0 || c.recreateSwapchainAfter.afterHooked)
    Log("  recreate-swapchain-after = %s%.3fs%s",
        c.recreateSwapchainAfter.afterHooked ? "hooked+" : "", c.recreateSwapchainAfter.afterSec,
        c.recreateSwapchainAfter.repeat ? " repeat" : "");
  if (c.recreateDeviceAfter.afterSec > 0 || c.recreateDeviceAfter.afterHooked)
    Log("  recreate-device-after = %s%.3fs%s", c.recreateDeviceAfter.afterHooked ? "hooked+" : "",
        c.recreateDeviceAfter.afterSec, c.recreateDeviceAfter.repeat ? " repeat" : "");
  if (c.resizeAfter.afterSec > 0 || c.resizeAfter.afterHooked)
    Log("  resize-after = %s%.3fs%s", c.resizeAfter.afterHooked ? "hooked+" : "",
        c.resizeAfter.afterSec, c.resizeAfter.repeat ? " repeat" : "");
  if (c.modeCycleAfter.afterSec > 0 || c.modeCycleAfter.afterHooked)
    Log("  mode-cycle-after = %s%.3fs%s", c.modeCycleAfter.afterHooked ? "hooked+" : "",
        c.modeCycleAfter.afterSec, c.modeCycleAfter.repeat ? " repeat" : "");
  if (c.churnHz > 0)
    Log("  churn       = %.3f Hz", c.churnHz);
  Log("  scene       = %s", Narrow(scene::SceneIdName(c.scene)).c_str());
  Log("  scene-seed  = 0x%08X (%u)", c.sceneSeed, c.sceneSeed);
  if (!c.dumpFramePath.empty())
    Log("  dump-frame  = %s", Narrow(c.dumpFramePath).c_str());
  Log("  gpu-mem-mb  = %d", c.gpuMemMb);
  Log("  block-capture = %s", Narrow(BlockCaptureModeName(c.blockCapture)).c_str());
  Log("  block-after = %d", c.blockCaptureAfterSeconds);
  Log("  show-block-errors = %d", c.showBlockErrors ? 1 : 0);
  Log("  capture-expected = %s", c.captureExpected ? "yes" : "NO");
  Log("  events       = %s", c.eventsJson ? "json" : "off");
  if (!c.readyFile.empty())
    Log("  ready-file   = %s", Narrow(c.readyFile).c_str());
  if (!c.instanceId.empty())
    Log("  instance     = %s", Narrow(c.instanceId).c_str());
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
  --verbose                 Per-frame scene-emit / d3d*-draw / d3d*-hud on stderr
  --recreate-swapchain-after <when>[,repeat]  F3: 5 | hooked | hooked+2
  --recreate-device-after <when>[,repeat]     F4
  --resize-after <when>[,repeat]              F2
  --mode-cycle-after <when>[,repeat]          F1
  --churn <hz>              Arm F6-style resize/recreate churn at launch
  --scene <name>            Draw-list scene for d3d11/d3d12/none (default: aurora)
  --scene-seed <u32>        Deterministic scene RNG seed (default: 0xC5A2EE)
  --list-scenes             Scene table + which APIs implement them
  --dump-frame <path.bmp>   d3d11/d3d12: write one framebuffer BMP after a few frames
  --gpu-mem <size>          Hold GPU RAM: 100|500|1024|2048 or 100mb|500mb|1gb|2gb
  --profile <name>          Apply game profile (title/class/block defaults)
  --list-profiles           Print profile table
  --list-profiles --json    Print profiles as JSON (consumed by tools/launch.ps1)
  --block-capture <mode>    none | signature-policy | squat-ipc | unload-hook
  --block-capture-after <s> Delay before applying block (0 = immediate)
  --show-block-errors       Do not suppress Windows loader hard-error dialogs
  --events json             NDJSON on stdout: ready, warning, block_active,
                            hook_attempt, hooked, unhooked, hook_blocked,
                            swapchain_recreated, device_recreated, resized, mode_changed
                            unhooked = HookReady gone after hooked (not F3/F4 recreate)
  --ready-file <path>       Write ready event JSON to file (harnesses without stdout)
  --instance <id>           Disambiguate concurrent runs (class or title suffix)
  --version / -V            Print version string and exit
  --help                    This help

Scenes (per-API; no visual parity across backends):
  aurora   d3d11/d3d12/none default draw-list (nebula, orbs, comet, bars)
  orbital  d3d11/d3d12/none (drones, asteroids, flash)
  highway  d3d11/d3d12/none (night neon road)
  fractal  d3d11 reference raymarch; d3d12/none approximate
  vulkan   always uses its own default (cycling clear + title frame);
           --scene is accepted and ignored

Capture blocking:
  signature-policy  SetProcessMitigationPolicy(MicrosoftSignedOnly) AFTER the
                    renderer is fully up. Blocks unsigned graphics-hook*.dll.
                    IRREVERSIBLE for the process — cannot be toggled off.
                    Loader "Bad Image" dialogs are suppressed (SetErrorMode)
                    only for this mode; use --show-block-errors to see them.
                    Default mechanism for profile cs2-blocked (verified).
  squat-ipc         Hold graphics_hook_dup_mutex+pid (hook DllMain duplicate
                    early-out) + empty-DACL CaptureHook_* objects. Reversible (F7).
  unload-hook       Poll for graphics-hook*.dll and FreeLibrary it. Reversible (F7).

  Every block mode self-verifies and emits block_active{verified}. Unverified
  blocks exit non-zero (code 5).

Hotkeys:
  F1  Cycle windowed / borderless / fullscreen-exclusive
  F2  Resize swapchain through preset resolutions
  F3  Destroy + recreate swapchain
  F4  Destroy + recreate device
  F5  Append #N to the current title (then re-check profile match)
  F6  Toggle churn mode (~2 Hz resize/recreate)
  F7  Toggle reversible capture block (squat-ipc / unload-hook)
  Esc Quit

Exit codes:
  0  OK
  2  Bad arguments
  3  Window create failed
  4  Renderer init failed
  5  Block requested but verification failed
  1  Other failure

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

  // Pass 1: --list-profiles, --list-scenes, --help, --version, --profile id
  std::wstring profileId;
  for (int i = 1; i < argc; ++i)
  {
    if (EqI(argv[i], L"--profile"))
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
    if (EqI(a, L"--profile"))
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
    if (EqI(a, L"--verbose") || EqI(a, L"--trace"))
    {
      c.verbose = true;
      continue;
    }
    if (EqI(a, L"--recreate-swapchain-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--recreate-swapchain-after"))
        return false;
      if (!ParseScheduledAfter(v, &c.recreateSwapchainAfter, error))
        return false;
      continue;
    }
    if (EqI(a, L"--recreate-device-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--recreate-device-after"))
        return false;
      if (!ParseScheduledAfter(v, &c.recreateDeviceAfter, error))
        return false;
      continue;
    }
    if (EqI(a, L"--resize-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--resize-after"))
        return false;
      if (!ParseScheduledAfter(v, &c.resizeAfter, error))
        return false;
      continue;
    }
    if (EqI(a, L"--mode-cycle-after"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--mode-cycle-after"))
        return false;
      if (!ParseScheduledAfter(v, &c.modeCycleAfter, error))
        return false;
      continue;
    }
    if (EqI(a, L"--churn"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--churn"))
        return false;
      wchar_t* end = nullptr;
      const double hz = wcstod(v.c_str(), &end);
      if (end == v.c_str() || (end && *end != L'\0') || !std::isfinite(hz) || hz <= 0 || hz > 30)
      {
        *error = L"invalid --churn Hz (want >0 and <=30)";
        return false;
      }
      c.churnHz = hz;
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
    if (EqI(a, L"--events"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--events"))
        return false;
      if (_wcsicmp(v.c_str(), L"json") != 0)
      {
        *error = L"invalid --events (only 'json' is supported)";
        return false;
      }
      c.eventsJson = true;
      continue;
    }
    if (EqI(a, L"--ready-file"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--ready-file"))
        return false;
      c.readyFile = v;
      continue;
    }
    if (EqI(a, L"--instance"))
    {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--instance"))
        return false;
      if (v.empty())
      {
        *error = L"empty --instance";
        return false;
      }
      // Keep instance token simple for class/title safety.
      for (wchar_t ch : v)
      {
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch))
        {
          *error = L"invalid --instance (avoid path/control chars)";
          return false;
        }
      }
      c.instanceId = v;
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

  // Instance suffix after title/class overrides so CLI --title/--class are base values.
  if (!c.instanceId.empty())
  {
    if (seen.title && ((c.profileMatchFlags == 4) ||
                       ((c.profileMatchFlags & 4) != 0 && (c.profileMatchFlags & 3) == 0)))
    {
      // User set title explicitly on class-matched profile; still append instance.
    }
    if (seen.cls && !((c.profileMatchFlags == 4) ||
                      ((c.profileMatchFlags & 4) != 0 && (c.profileMatchFlags & 3) == 0)))
    {
      // User set class on exe-matched; still append instance.
    }
    ApplyInstanceId(&c);
  }

  *out = std::move(c);
  return true;
}
