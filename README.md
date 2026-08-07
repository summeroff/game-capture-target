# fakegame — Windows capture test app

Small, dependency-free Win32 tool for exercising **OBS / Streamlabs Desktop** Game Capture
and Window Capture locally. Built because renaming system binaries (`charmap.exe` → `cs2.exe`)
fails silently on SxS/manifest resolution.

Optimised for: builds in seconds, zero third-party deps (Vulkan headers vendored), trivially
renameable, fully configurable from the command line. Behaviour never depends on the executable
filename.

**GitHub:** https://github.com/summeroff/capture-test-app (private)

## Build

**Requirements:** Visual Studio 2022 (MSVC), CMake ≥ 3.20, Windows 10/11 SDK.

```bat
scripts\build.bat
```

→ `build\bin\fakegame.exe` (x64, Release, static CRT `/MT`)

```bat
scripts\build-x86.bat
```

→ `build-x86\bin\fakegame.exe`

Links only: `d3d11`, `d3d12`, `dxgi`, `d3dcompiler`, `user32`, `gdi32`, `dwmapi`, `advapi32`,
`psapi`. Vulkan is **LoadLibrary**'d at runtime (`vulkan-1.dll`) — no Vulkan SDK needed to build.
Headers live under `third_party/Vulkan-Headers` (Khronos, vendored — not a git submodule).

## Quick start (QA)

```powershell
# Interactive menu — Enter through defaults for a sensible capturable run
.\tools\launch.ps1

# Reproduce the real CS2 condition: warning + capture never succeeds
.\tools\launch.ps1 -Profile cs2-blocked -Json

# Or explicit:
.\tools\launch.ps1 -Profile cs2 -Api d3d11 -BlockCapture signature-policy -Json

# Machine-readable profile table (same JSON as the exe)
.\tools\launch.ps1 -List -Json
.\build\bin\fakegame.exe --list-profiles --json

# Kill everything this tool spawned
.\tools\launch.ps1 -StopAll
```

`-Json` emits `obsWindowSetting` (`title:class:exe` with `#`/`:` encoded as OBS expects) for
e2e harnesses that cannot pick from the antd virtualized dropdown.

## Capture-refusal modes

Round 1 could show the CS2 compatibility **warning** but still captured successfully. Round 2
adds real refusal; round 3 silences the OS “Bad Image” spam for signature-policy.

| Mode | Flag | Reversible | What it does |
|------|------|------------|--------------|
| none | `--block-capture none` | — | Normal capture |
| signature-policy | `--block-capture signature-policy` | **No** | `SetProcessMitigationPolicy(MicrosoftSignedOnly)` **after** the renderer is fully up. Blocks unsigned `graphics-hook*.dll`. Loader hard-error dialogs suppressed by default (`SetErrorMode`); `--show-block-errors` restores them. |
| squat-ipc | `--block-capture squat-ipc` | **Yes (F7)** | Pre-creates OBS hook IPC objects (`CaptureHook_*` + pid) with an empty DACL. **Default for `cs2-blocked`** (silent, reversible). |
| unload-hook | `--block-capture unload-hook` | **Yes (F7)** | Polls for `graphics-hook*.dll` and `FreeLibrary`s it |

Also:

- `--block-capture-after <seconds>` — capture works first, then block applies (one-way if signature-policy)
- `F7` — toggle reversible blocks
- `--show-block-errors` — keep Windows “Bad Image” dialogs (debug the block mechanism)

## Graphics APIs

| `--api` | Status | Notes |
|---------|--------|-------|
| `d3d11` | default | FLIP_DISCARD / DISCARD via `--flip-model` |
| `d3d12` | supported | DXGI flip swapchain + HUD |
| `vulkan` | supported | Dynamic `vulkan-1.dll`; cycling clear + title frame counter |
| `none` | supported | GDI only, no swapchain → OBS “not a game” |

## Profiles

`--profile <name>` sets title/class/block defaults (overridable by later flags). Single source of
truth: `fakegame.exe --list-profiles --json` (consumed by `launch.ps1` — no duplicate table).

| Profile | exe | Match | Sev | Capture expected |
|---------|-----|-------|-----|------------------|
| cs2 | cs2.exe | exe | Warning | yes |
| cs2-blocked | cs2.exe | exe | Warning | **NO** (default: squat-ipc) |
| minecraft | javaw.exe | exe+title | Normal | yes |
| wuthering | Client-Win64-Shipping.exe | exe+title | Warning | yes |
| destiny2 | destiny2.exe | exe | Error | yes |
| gta-sa | gta-sa.exe | exe | Error | yes |
| chromium-gc | any | class | Error | yes |
| gaming-services | any | class | Error | yes |
| terraria | terraria.exe | exe | Normal | yes |
| roblox | RobloxPlayerBeta.exe | exe | Warning | yes |
| steam | any | class | Normal | Window Capture |
| excel | any | class | Normal | Window Capture |

Prefix title test: `--profile minecraft --title "Minecraft 1.21"`.

## Command-line flags

| Flag | Default | Notes |
|------|---------|-------|
| `--title <string>` | `Fake Game` | |
| `--class <string>` | `FakeGameWindowClass` | Fixed at RegisterClassEx |
| `--width` / `--height` | 1280 / 720 | |
| `--mode` | windowed | windowed \| borderless \| fullscreen-exclusive |
| `--api` | d3d11 | d3d11 \| d3d12 \| vulkan \| none |
| `--fps` | 60 | |
| `--vsync` | 1 | |
| `--flip-model` | 1 | d3d11/d3d12; logged ignored on vulkan/none |
| `--buffers` | 2 | |
| `--exit-after` | 0 | |
| `--topmost` | off | |
| `--no-hud` | off | |
| `--config <path>` | — | INI; flags override |
| `--profile <name>` | — | |
| `--list-profiles` | — | table or with `--json` |
| `--block-capture` | none | none \| signature-policy \| squat-ipc \| unload-hook |
| `--block-capture-after` | 0 | seconds |
| `--show-block-errors` | off | allow Windows loader hard-error dialogs |

## Hotkeys

| Key | Action |
|-----|--------|
| F1 | Cycle windowed / borderless / fullscreen-exclusive |
| F2 | Resize swapchain presets |
| F3 | Recreate swapchain |
| F4 | Recreate device |
| F5 | Change title (+ counter) |
| F6 | Churn mode ~2 Hz |
| F7 | Toggle reversible capture block |
| Esc | Quit |

Every transition is logged to stdout with a timestamp.

## Low-level rename helper

```powershell
.\tools\spawn-as.ps1 -As cs2.exe -GameArgs '--title "Counter-Strike 2"'
```

Prefer `launch.ps1` for profile-aware QA.

## Acceptance (app-side, verified in build)

| # | Check | Result |
|---|-------|--------|
| 1 | x64 Release `/W4` | green |
| 2 | rename → `cs2.exe` alone | no missing DLL (static CRT) |
| 9 | signature-policy applies after D3D | log: `signature-policy APPLIED` |
| — | squat-ipc creates deny DACL objects | log lists CaptureHook_*+pid |
| — | `--api d3d12` | device + swapchain ok |
| — | `--api vulkan` | device + swapchain ok |
| — | `launch.ps1 -Profile cs2 -Json` | pid/hwnd/obsWindowSetting |
| — | `-StopAll` | kills spawn dir processes |

OBS/Streamlabs UI checks (warning text, blank preview, F7 restore) need a human with SLD open.

## Layout

```
CMakeLists.txt
PROMPT.md
README.md
scripts/build.bat  build-x86.bat  build-wx.bat  verify.bat
tools/launch.ps1   spawn-as.ps1
third_party/Vulkan-Headers/   # vendored Khronos headers
src/
  main.cpp app.* config.* profiles.* block_capture.*
  d3d11_renderer.cpp d3d12_renderer.cpp vulkan_renderer.cpp none_renderer.cpp
  renderer.hpp font8x8.hpp log.hpp
```
