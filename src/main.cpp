#include "app.hpp"
#include "config.hpp"
#include "log.hpp"

#include <cstdio>
#include <string>

#include <Windows.h>

namespace {

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
  --api <api>               d3d11 | none   (d3d12/opengl reserved)
  --fps <n>                 Frame pacing target (default: 60)
  --vsync <0|1>             (default: 1)
  --flip-model <0|1>        FLIP_DISCARD vs DISCARD (default: 1)
  --buffers <n>             Swapchain buffer count (default: 2)
  --exit-after <seconds>    Auto-quit (default: 0 = never)
  --topmost                 WS_EX_TOPMOST
  --no-hud                  Hide on-screen HUD
  --config <path>           INI file; flags override file values
  --help                    This help

Hotkeys:
  F1  Cycle windowed / borderless / fullscreen-exclusive
  F2  Resize swapchain through preset resolutions
  F3  Destroy + recreate swapchain
  F4  Destroy + recreate D3D device
  F5  Change window title (append counter)
  F6  Toggle churn mode (~2 Hz resize/recreate)
  Esc Quit

Rename-friendly: behaviour never depends on the exe filename.
Use tools\spawn-as.ps1 to copy+launch as cs2.exe etc.
)");
}

// On-screen config dump is rendered by the HUD; also mirror to a startup Message... no, prompt
// says print to stdout AND show on-screen at startup — HUD covers on-screen.
void ShowStartupBanner(const Config& cfg)
{
  // Brief topmost-less flash via stdout only; HUD shows the same fields live.
  PrintConfig(cfg);
  Log("hotkeys: F1 mode | F2 resize | F3 swapchain | F4 device | F5 title | F6 churn | Esc quit");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
  // Ensure CRT stdio is fully buffered line-wise for OBS log correlation.
  setvbuf(stdout, nullptr, _IONBF, 0);

  Config cfg;
  std::wstring error;
  if (!ParseConfig(argc, argv, &cfg, &error)) {
    if (error == L"help") {
      PrintHelp();
      return 0;
    }
    std::fwprintf(stderr, L"error: %s\n\n", error.c_str());
    PrintHelp();
    return 2;
  }

  ShowStartupBanner(cfg);

  App app(cfg);
  if (!app.Initialize(&error)) {
    std::fwprintf(stderr, L"init failed: %s\n", error.c_str());
    Log("init failed: %s", Narrow(error).c_str());
    return 1;
  }

  return app.Run();
}
