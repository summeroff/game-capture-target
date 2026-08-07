# fakegame — Windows capture test app

Small, dependency-free Win32 tool for exercising **OBS / Streamlabs Desktop** Game Capture
and Window Capture locally. Built because renaming system binaries (`charmap.exe` → `cs2.exe`)
fails silently on SxS/manifest resolution.

Optimised for: builds in seconds, zero third-party deps, trivially renameable, fully
configurable from the command line. Behaviour never depends on the executable filename.

## Build

**Requirements:** Visual Studio 2022 (MSVC), CMake ≥ 3.20, Windows 10/11 SDK.

```bat
scripts\build.bat
```

Produces `build\bin\fakegame.exe` (x64, Release, static CRT `/MT`).

x86 (covers the 32-bit `graphics-hook32.dll` inject path):

```bat
scripts\build-x86.bat
```

→ `build-x86\bin\fakegame.exe`

Manual:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

No vcpkg, no Conan, no submodules. Links only: `d3d11`, `dxgi`, `d3dcompiler`, `user32`,
`gdi32`, `dwmapi`.

### Self-contained binary

- Static CRT (`/MT` / `/MTd`)
- HLSL compiled at runtime with `D3DCompile` (no `.cso` / `.hlsl` beside the exe)
- Copy the single `.exe` anywhere, rename it anything, run it

## Run

```bat
build\bin\fakegame.exe
build\bin\fakegame.exe --title "Counter-Strike 2" --api d3d11
build\bin\fakegame.exe --api none --exit-after 10
```

### Rename helper

```powershell
.\tools\spawn-as.ps1 -As cs2.exe -GameArgs '--title "Counter-Strike 2"'
.\tools\spawn-as.ps1 -As destiny2.exe
.\tools\spawn-as.ps1 -As javaw.exe -GameArgs '--api d3d11'
```

Copies `fakegame.exe` → `tools\_spawn\<name>` and launches it. Passthrough args are unchanged.

### Useful exe names (OBS compatibility severities)

Drawn from OBS `compatibility.json` / win-capture severity handling. Rename alone hits each branch:

| Exe name        | Typical severity | Notes                                      |
|-----------------|------------------|--------------------------------------------|
| `fakegame.exe`  | (none)           | Default; not on the exe blacklist           |
| `javaw.exe`     | 0 Normal         | Minecraft-style Java launcher              |
| `cs2.exe`       | 1 Warning        | Counter-Strike 2; third-party software warn|
| `csgo.exe`      | 1 Warning        | CS:GO                                      |
| `destiny2.exe`  | 2 Error          | Destiny 2                                  |
| `overwatch.exe` | varies           | Blizzard titles often special-cased        |

OBS blacklisted default names (avoid for the *built* binary; fine as rename targets only if you
intentionally want blacklist behaviour):  
`explorer, steam, battle.net, galaxyclient, skype, uplay, origin, devenv, taskmgr, chrome,
discord, firefox, systemsettings, applicationframehost, cmd, shellexperiencehost,
winstore.app, searchui, lockapp`.

Match flags reference: `MATCH_EXE=1`, `MATCH_TITLE=2`, `MATCH_CLASS=4`.

## Command-line flags

All optional. `--config <path>` loads an INI first; flags override file values.
Resolved config is printed to **stdout** and shown in the on-screen HUD.

| Flag | Default | Notes |
|------|---------|-------|
| `--title <string>` | `Fake Game` | Window title (OBS title match is prefix) |
| `--class <string>` | `FakeGameWindowClass` | Window class (fixed at `RegisterClassEx`) |
| `--width <n>` | `1280` | Client area width |
| `--height <n>` | `720` | Client area height |
| `--mode <mode>` | `windowed` | `windowed` \| `borderless` \| `fullscreen-exclusive` |
| `--api <api>` | `d3d11` | `d3d11` \| `none` (`d3d12`/`opengl` reserved) |
| `--fps <n>` | `60` | Frame pacing |
| `--vsync <0\|1>` | `1` | |
| `--flip-model <0\|1>` | `1` | `FLIP_DISCARD` vs legacy `DISCARD` |
| `--buffers <n>` | `2` | Swapchain buffer count (2–8) |
| `--exit-after <seconds>` | `0` | `0` = never |
| `--topmost` | off | |
| `--no-hud` | off | |
| `--config <path>` | — | INI; keys mirror flag names (`title=`, `api=`, …) |
| `--help` | | |

### Example INI

```ini
title=Fake Game
class=FakeGameWindowClass
width=1280
height=720
mode=windowed
api=d3d11
fps=60
vsync=1
flip-model=1
buffers=2
```

## Hotkeys

Every transition is logged to stdout with a timestamp (correlate with OBS logs).

| Key | Action |
|-----|--------|
| `F1` | Cycle windowed → borderless → fullscreen-exclusive |
| `F2` | Resize swapchain through preset resolutions |
| `F3` | Destroy + recreate swapchain |
| `F4` | Destroy + recreate D3D device (TDR / device-lost sim) |
| `F5` | Change window title at runtime (append counter) |
| `F6` | Toggle churn mode (~2 Hz resize + recreate) |
| `Esc` | Quit |

## On-screen content

Designed so a **stale** capture is obvious:

- Large monotonically increasing **frame counter**
- **Rotating / orbiting coloured quad** every frame
- **Cycling clear colour**
- HUD: resolution, API, swap effect, present mode, window class/title, PID, elapsed time

`--api none` draws the same content with GDI only — **no DXGI swapchain** — so Game Capture
should list the window but fail with the “not a game” path.

## OBS / Streamlabs checklist

1. Build x64 Release; confirm `build\bin\fakegame.exe` exists.
2. Copy alone to an empty folder, rename to `cs2.exe`, run → window animates, no missing-DLL/SxS.
3. Game Capture → *Capture specific window* → `fakegame` / renamed window appears.
4. `--api d3d11`: live frames + advancing counter in preview.
5. `--api none`: window listed, capture fails (“not a game”).
6. Renamed `cs2.exe`: properties show CS2 compatibility warning (`-allow_third_party_software`).
7. Exercise `F1` / `F3` / `F6` — app must not crash; note whether capture recovers.
8. Repeat 3–4 with the x86 build.

## Layout

```
CMakeLists.txt
PROMPT.md
README.md
scripts/build.bat
scripts/build-x86.bat
tools/spawn-as.ps1
src/
  main.cpp
  app.cpp / app.hpp
  config.cpp / config.hpp
  d3d11_renderer.cpp
  none_renderer.cpp
  renderer.hpp
  font8x8.hpp
  log.hpp
```

## License

Internal developer tool. Use / modify freely in your capture testing workflow.
