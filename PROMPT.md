# Build a configurable Windows capture-test application ("fakegame")

## Context

I work on **Streamlabs Desktop**, which embeds OBS Studio's `win-capture` plugin (Game Capture /
Window Capture). I need a small, purpose-built Win32 application to test capture behaviour
locally. Copies of system binaries (`charmap.exe` etc.) renamed to game executables do not work —
they fail silently on SxS/manifest resolution.

This is a **developer test tool**, not a game. Optimise for: builds in seconds, zero third-party
dependencies, trivially renameable, fully configurable from the command line.

## Where it lives

Create it as a standalone project at `C:\work\repos\capture-test-app` (already exists, containing
only this file; `git init` it). Do not modify any sibling repo under `C:\work\repos\`.

## Build

- **CMake + MSVC (Visual Studio 2022), x64 primary.** Also support an x86 configuration — OBS
  injects `graphics-hook32.dll` via a separate 32-bit inject helper, and that path deserves
  coverage.
- **Windows SDK only.** Link `d3d11`, `dxgi`, `d3dcompiler`, `user32`, `gdi32`. No vcpkg, no
  Conan, no submodules.
- **Static CRT (`/MT`, `/MTd`)** and **no external manifest dependencies.** The binary must run
  correctly after being copied to an arbitrary directory and renamed to an arbitrary filename.
  This is a hard requirement — it's the whole reason the tool exists.
- Compile HLSL inline with `D3DCompile` at runtime, or embed precompiled bytecode. Do not ship
  separate `.cso`/`.hlsl` files the exe has to find on disk — a renamed, relocated copy must be
  fully self-contained in one file.
- Output binary name: `fakegame.exe`.

## Requirements the OBS side imposes

These come from reading OBS source; treat them as non-negotiable constraints.

1. **Window must be enumerable.** OBS's `check_window_valid()`
   (`libobs/util/windows/window-helpers.c:352`) requires: `IsWindowVisible()` true; not
   `IsIconic()`; not cloaked (`DWMWA_CLOAKED`); no `WS_EX_TOOLWINDOW`; no `WS_CHILD`; non-zero
   `GetClientRect`. Use a plain `WS_OVERLAPPEDWINDOW` top-level window. Do not set
   `WS_EX_TOOLWINDOW`.
2. **Executable name is the primary match key.** OBS compares the bare filename
   case-insensitively (`cs2.exe`, `destiny2.exe`, `javaw.exe`, …). The app must never derive
   behaviour, config paths, or resource paths from its own filename.
3. **Window class name must be configurable at startup.** Some OBS compatibility entries match on
   class only (`match_flags: 4`, e.g. `Chrome_WidgetWin_0`). Class is fixed at `RegisterClassEx`
   time, so it must be a startup flag, not a runtime toggle.
4. **Window title must be configurable**, at startup and at runtime. OBS title matching is a
   *prefix* match (`astrcmpi_n`), which is worth being able to exercise.
5. **Avoid OBS's exe blacklist** for default names (`plugins/win-capture/game-capture.c:1313`).
   Blacklisted: `explorer, steam, battle.net, galaxyclient, skype, uplay, origin, devenv, taskmgr,
   chrome, discord, firefox, systemsettings, applicationframehost, cmd, shellexperiencehost,
   winstore.app, searchui, lockapp`. Default to `fakegame.exe`.

Reference for the match flags (`plugins/win-capture/compat-helpers.c:11`):
`MATCH_EXE = 1`, `MATCH_TITLE = 2`, `MATCH_CLASS = 4`. The Counter-Strike 2 entry uses
`match_flags: 1` — executable name only.

## Configuration surface

Command-line flags, all optional with sane defaults. Also accept an optional `--config <path>`
INI/JSON that the flags override. Print resolved config to stdout **and** show it on-screen at
startup.

| Flag | Default | Notes |
|---|---|---|
| `--title <string>` | `Fake Game` | Window title |
| `--class <string>` | `FakeGameWindowClass` | Window class name |
| `--width <n>` / `--height <n>` | `1280` / `720` | Client area size |
| `--mode <windowed\|borderless\|fullscreen-exclusive>` | `windowed` | Game Capture behaves differently per mode |
| `--api <d3d11\|d3d12\|opengl\|none>` | `d3d11` | `none` = GDI-only window with **no swapchain**; must reproduce OBS's *"Specified window is not a game"* state. Implement `d3d11` first; `d3d12`/`opengl` are stretch goals — structure the renderer behind an interface so they can be added later |
| `--fps <n>` | `60` | Frame pacing |
| `--vsync <0\|1>` | `1` | |
| `--flip-model <0\|1>` | `1` | `DXGI_SWAP_EFFECT_FLIP_DISCARD` vs legacy `DISCARD`. The hook takes different paths for each — both must be testable |
| `--buffers <n>` | `2` | Swapchain buffer count |
| `--exit-after <seconds>` | `0` (never) | For scripted runs |
| `--topmost` | off | |
| `--no-hud` | off | |

## On-screen content

The window must make it **immediately obvious whether capture is live and current**, not showing
a stale frame:

- A monotonically increasing **frame counter**, rendered large.
- A **moving element** (rotating/translating coloured quad) that visibly animates every frame.
- A colour that cycles per frame, so a frozen capture is obvious at a glance.
- A HUD text block: resolution, graphics API, swap effect, present mode, window class, window
  title, PID, elapsed time.
- Text rendering: keep it dependency-free. `ID3D11` + a simple embedded bitmap font, or
  `IDXGISurface1::GetDC()` + GDI `DrawText`, is fine. Do not pull in DirectWrite/D2D if it
  complicates the build.

## Runtime controls (keyboard)

These exercise the hook's re-initialisation paths, which is where capture bugs concentrate:

- `F1` — toggle windowed / borderless / fullscreen-exclusive
- `F2` — resize the swapchain to a different resolution
- `F3` — destroy and recreate the swapchain entirely
- `F4` — destroy and recreate the D3D device (simulates a device-lost / TDR recovery)
- `F5` — change the window title at runtime (append a counter)
- `F6` — start/stop a **churn mode** that repeatedly resizes and recreates the swapchain at ~2 Hz
  until stopped
- `Esc` — quit

Log every one of these transitions to stdout with a timestamp, so an OBS log can be correlated
against it.

## Helper script

Ship `tools/spawn-as.ps1` (PowerShell, not Bash) that copies the built `fakegame.exe` to a target
directory under a given name and launches it with passthrough arguments:

```
.\tools\spawn-as.ps1 -As cs2.exe -Args '--title "Counter-Strike 2"'
```

Include a short table in the README of useful names drawn from OBS's `compatibility.json` and the
severity each produces — e.g. `cs2.exe` and `csgo.exe` are severity 1 (Warning), `destiny2.exe` is
severity 2 (Error), `javaw.exe` is severity 0 (Normal). This lets a tester hit every severity
branch by renaming alone.

## Explicitly out of scope

No game logic, no audio, no input handling beyond the hotkeys above, no networking, no installer,
no anti-cheat emulation, no CI. Single source file is acceptable if it stays readable; split only
when it genuinely helps.

## Acceptance criteria

Verify each of these and report the actual result — not "should work":

1. `cmake --build` produces `fakegame.exe` (x64, Release) with zero warnings at `/W4`.
2. Copy the exe alone to an empty directory, rename it to `cs2.exe`, run it → window appears and
   animates. **No missing-DLL or SxS error.** This is the case the tool exists to fix.
3. In Streamlabs Desktop or OBS: add a **Game Capture** source → *Capture specific window* → the
   `fakegame.exe` window appears in the dropdown.
4. With `--api d3d11`: capture shows the live animated content, and the frame counter advances in
   the preview.
5. With `--api none`: the window still appears in the dropdown but capture fails — confirming the
   "not a game" state.
6. Renamed to `cs2.exe`: the properties panel shows the Counter-Strike 2 compatibility warning
   (`-allow_third_party_software`).
7. `F1`/`F3`/`F6` do not crash the app, and capture either recovers or fails cleanly — record
   which.
8. Same as (3)–(4) for the **x86** build.

## Deliverables

Working source, `CMakeLists.txt`, `tools/spawn-as.ps1`, and a `README.md` covering build steps,
every flag, every hotkey, and the rename-based severity table.
