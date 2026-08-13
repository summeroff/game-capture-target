# game-capture-target (`fakegame.exe`)

Small, dependency-free Win32 tool for exercising **OBS / Streamlabs Desktop** Game Capture
and Window Capture locally. Built because renaming system binaries (`charmap.exe` → `cs2.exe`)
fails silently on SxS/manifest resolution.

Optimised for: builds in seconds, zero third-party deps (Vulkan headers vendored), trivially
renameable, fully configurable from the command line. Behaviour never depends on the executable
filename.

**GitHub:** https://github.com/summeroff/game-capture-target (public, MIT)  
Binary name remains **`fakegame.exe`** (build output); rename freely for OBS profile tests.

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
adds real refusal; round 3 silences the OS “Bad Image” spam for signature-policy. Round 4
defaults `cs2-blocked` back to **signature-policy** (verified), fixes squat-ipc (dup-mutex +
empty DACL + self-verify), and adds harness NDJSON events.

| Mode | Flag | Reversible | What it does |
|------|------|------------|--------------|
| none | `--block-capture none` | — | Normal capture |
| signature-policy | `--block-capture signature-policy` | **No** | `SetProcessMitigationPolicy(MicrosoftSignedOnly)` **after** the renderer is fully up. Blocks unsigned `graphics-hook*.dll`. Loader hard-error dialogs suppressed **only for this mode** (`SetErrorMode`); `--show-block-errors` restores them. **Default for `cs2-blocked`.** |
| squat-ipc | `--block-capture squat-ipc` | **Yes (F7)** | Holds `graphics_hook_dup_mutex`+pid (hook DllMain duplicate early-out) + empty-DACL `CaptureHook_*` objects. Self-verifies or exits 5. |
| unload-hook | `--block-capture unload-hook` | **Yes (F7)** | Polls for `graphics-hook*.dll` and `FreeLibrary`s it |

Also:

- `--block-capture-after <seconds>` — capture works first, then block applies (one-way if signature-policy)
- `F7` — toggle reversible blocks
- `--show-block-errors` — keep Windows “Bad Image” dialogs (debug the block mechanism)
- Every block emits `block_active` with `verified` when `--events json` is on; unverified → exit **5**

## Test-harness events (addendum 4)

```powershell
.\build\bin\fakegame.exe --profile cs2 --events json --ready-file ready.json --exit-after 5
```

| Flag | Notes |
|------|-------|
| `--events json` | NDJSON on **stdout** (`ready`, `warning`, `block_active`, `hook_attempt` / `hooked` / `unhooked` / `hook_blocked`); human logs on **stderr** |
| `--ready-file <path>` | Writes the `ready` object (same fields) for harnesses that cannot read stdout |
| `--instance <id>` | Disambiguate concurrent runs: suffix on **class** (exe-matched) or **title** (class-matched) |

`ready` includes app-owned `obsWindowSetting` (`title:class:exe` with `#`→`#22`, `:`→`#3A`),
`clientWidth` / `clientHeight`, `hwnd`, `pid`, `blockCapture`, `captureExpected`.

If a profile matches on **exe** but the process image is still `fakegame.exe` (no rename), a
`warning` event with `code=exe_mismatch` is emitted (and logged) right after `ready` — so harnesses
do not silently miss the CS2 compatibility entry. Title-matched profiles get `title_mismatch` when
the live title does not start with the profile title (prefix, case-insensitive). Class-matched
profiles get `class_mismatch` when the live class differs from the profile declaration.

Exit codes: `0` OK · `2` bad args · `3` window create · `4` renderer · `5` block unverified · `1` other.

## Graphics APIs

| `--api` | Status | Notes |
|---------|--------|-------|
| `d3d11` | default | FLIP_DISCARD / DISCARD via `--flip-model` |
| `d3d12` | supported | DXGI flip swapchain + HUD |
| `vulkan` | supported | Own default visual: cycling clear + title frame counter. `--scene` ignored. |
| `none` | supported | GDI only, no swapchain → OBS “not a game” |

## Profiles

`--profile <name>` sets title/class/block defaults (overridable by later flags). Single source of
truth: `fakegame.exe --list-profiles --json` (consumed by `launch.ps1` — no duplicate table).

| Profile | exe | Match | Sev | Capture expected |
|---------|-----|-------|-----|------------------|
| cs2 | cs2.exe | exe | Warning | yes |
| cs2-blocked | cs2.exe | exe | Warning | **NO** (default: signature-policy) |
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
| `--verbose` | off | per-frame `scene-emit` / `d3d*-draw` / `d3d*-hud` on stderr |
| `--recreate-swapchain-after <s>[,repeat]` | off | F3 after N seconds |
| `--recreate-device-after <s>[,repeat]` | off | F4 after N seconds |
| `--resize-after <s>[,repeat]` | off | F2 after N seconds |
| `--mode-cycle-after <s>[,repeat]` | off | F1 after N seconds |
| `--churn <hz>` | off | arm F6-style churn at launch |
| `--scene <name>` | `aurora` | `aurora` \| `orbital` \| `highway` \| `fractal` |
| `--scene-seed <u32>` | `0xC5A2EE` | Deterministic scene RNG; fractal even=A / odd=B |
| `--list-scenes` | — | Scene table |
| `--dump-frame <path.bmp>` | — | d3d11/d3d12: one framebuffer BMP after a few frames |
| `--gpu-mem <size>` | off | Hold GPU RAM: `100`/`500`/`1024`/`2048` or `100mb`/`500mb`/`1gb`/`2gb` |
| `--profile <name>` | — | |
| `--list-profiles` | — | table or with `--json` |
| `--block-capture` | none | none \| signature-policy \| squat-ipc \| unload-hook |
| `--block-capture-after` | 0 | seconds |
| `--show-block-errors` | off | allow Windows loader hard-error dialogs |
| `--events json` | off | NDJSON on stdout (`ready`, `hooked`, `unhooked`, `swapchain_recreated`, …); Log → stderr |
| `--ready-file <path>` | — | write `ready` JSON for harnesses |
| `--instance <id>` | — | disambiguate concurrent runs |
| `--version` / `-V` | — | print version (`X.Y.Z+gSHA` or `0.0.0-dev+gSHA`) and exit |

## Scenes (per-API visuals)

Scenes are **not** shared across APIs. Each backend has its own default visual.

| `--api` | Default visual | `--scene` |
|---------|----------------|-----------|
| `d3d11` | `aurora` draw-list (reference quality) | `aurora` / `orbital` / `highway` / `fractal` |
| `d3d12` | same draw-list, lighter shaders | same names; fractal/aurora PS approximated |
| `none` | GDI approximation of the draw-list | same names |
| `vulkan` | cycling clear + title frame counter | accepted, **ignored** |

| `--scene` (d3d11/d3d12/none) | What you see | Best for |
|-----------|--------------|----------|
| `aurora` (default) | Nebula, soft orbs, comet, EQ bars | General freeze tell |
| `orbital` | Starfold backdrop, drones, asteroids, flash | Particles, alpha, tiny sprites, HUD |
| `highway` | Night neon highway, perspective road, traffic, speedo | Aspect ratio, resize, clipping |
| `fractal` | Fullscreen twigl-style raymarch (+ orbiting material tris) | GPU fill-rate / heavy PS (d3d11) |

`--scene-seed <u32>` makes draw-list spawns/motion reproducible on d3d11/d3d12/none.

## Hotkeys

| Key | Action |
|-----|--------|
| F1 | Cycle windowed / borderless / fullscreen-exclusive (`--mode-cycle-after`) |
| F2 | Resize swapchain presets (`--resize-after`) |
| F3 | Recreate swapchain (`--recreate-swapchain-after`) |
| F4 | Recreate device (`--recreate-device-after`) |
| F5 | Append `#N` to the current title (then re-check profile match) |
| F6 | Churn mode ~2 Hz (`--churn <hz>`) |
| F7 | Toggle reversible capture block |
| Esc | Quit |

Every transition is logged (stdout, or stderr when `--events json`).

## Low-level rename helper

```powershell
.\tools\spawn-as.ps1 -As cs2.exe -GameArgs '--title "Counter-Strike 2"'
```

Prefer `launch.ps1` for profile-aware QA. `-Json` launches with `--events json` + `--ready-file`
and prefers the app `ready` object (including `obsWindowSetting`) over re-encoding in PowerShell.

## Acceptance (app-side, verified in build)

| # | Check | Result |
|---|-------|--------|
| 1 | x64 Release `/W4` | green |
| 2 | rename → `cs2.exe` alone | no missing DLL (static CRT) |
| 9 | signature-policy applies after D3D | log + `block_active.verified` |
| 26 | cs2-blocked default signature-policy | list-profiles + smoke |
| 27 | `--events json` ready + obsWindowSetting | smoke |
| 29 | block self-verify or exit 5 | squat + signature smoke |
| — | squat-ipc dup-mutex + deny DACL | verify CreateEvent ACCESS_DENIED |
| — | `--api d3d12` | device + swapchain ok |
| — | `--api vulkan` | device + swapchain ok |
| — | `launch.ps1 -Profile cs2 -Json` | pid/hwnd/obsWindowSetting |
| — | `-StopAll` | kills spawn dir processes |

OBS/Streamlabs UI checks (warning text, blank preview, F7 restore) need a human with SLD open.

## CI / code style

GitHub Actions (`.github/workflows/ci.yml`) on push/PR to `master`, and on tags `v*`:

| Job | What |
|-----|------|
| **Format** | Ubuntu + pinned `clang-format-18` via `scripts/format.sh --check` |
| **Build & smoke (x64)** | VS 2022 Release + `scripts/ci-smoke.ps1` (help, `--version`, profiles JSON, d3d11/d3d12/none, flip-model 0, squat-ipc, cs2 profile; vulkan optional) |
| **Build (Win32)** | x86 Release + short d3d11/none smoke |
| **GitHub Release** | on `v*` tags only — attaches `fakegame-vX.Y.Z-win-x64.zip` + `…-win-x86.zip` (each zip includes **`fakegame.pdb`**) |

Brace style is **Allman** (`{` / `}` on their own line) with **`} else {`** / **`} else if`** allowed joined. See `CODING_STYLE.md` and `.clang-format`.

**Local format gate (run before every push):** CI pins **Ubuntu clang-format 18.1.3**. Local LLVM 22
will often green-check while CI fails on `<<` wrapping. Prefer:

```bat
set PYTHONPATH=
"%LOCALAPPDATA%\hermes\hermes-agent\venv\Scripts\python.exe" -m pip install "clang-format==18.1.3"
scripts\format.bat
scripts\format.bat --check
```

`format.bat` prefers the PyPI 18.x binary under `site-packages\clang_format\data\bin\` and prints
the version. Override with `set CLANG_FORMAT=C:\path\to\clang-format.exe`.

```bat
scripts\format.bat
scripts\format.bat --check
scripts\ci-smoke.ps1
```

Workflow for changes: feature branch → PR → leave open for Copilot/human review (do not auto-merge).

## Debug symbols (PDBs) + freeze dumps

Release and local MSVC builds emit **`fakegame.pdb`** next to `fakegame.exe` (`/Zi` + `/DEBUG:FULL`). Tag zips always include the PDB.

| Goal | What to do |
|------|------------|
| Local hang/crash | Use the PDB from the same `build\bin\` as the exe that froze |
| Renamed launch (`cs2.exe` via `launch.ps1`) | PDB is still `fakegame.pdb` — PE debug directory points at that name. Put the PDB next to the renamed exe **or** point the debugger at a symbol path that contains it |
| Released binary dump | Download the **same** release zip (x64 vs x86 must match). PDB GUID must match that PE — a rebuild of the same commit on another machine usually will **not** load |

**WinDbg (example):**

```text
.sympath+ C:\path\to\unzipped-release-or-build-bin
.reload /f
k
!analyze -v
```

**Visual Studio:** open the `.dmp` → set symbol path to the folder that has the matching `fakegame.pdb`.

Notes:

- A dump from a binary that shipped **without** a PDB (e.g. early `v0.2.0` if the zip had no PDB) cannot get a full stack from a later rebuild — the CodeView signature will not match.
- A freeze while a harness pipes/redirects stdout can be the **reader** blocking on a full pipe, not only an app bug. Prefer `--ready-file` + stderr logs, or consume NDJSON promptly when using `--events json`.

## Releases (version tags)

Version is **injected from the git tag** at configure time. Local/PR builds stay `0.0.0-dev+g<sha>` — there is no per-release CMake bump commit.

| Build | Version string | PE `FILEVERSION` |
|-------|----------------|------------------|
| Local / PR / untagged CI | `0.0.0-dev+g…` | `0.0.0.0` |
| Tag `v1.0.0` | `1.0.0+g…` | `1.0.0.0` |
| Tag `v1.0.0-b1` (prerelease) | `1.0.0-b1+g…` | `1.0.0.0` |

Cut a release from updated `master`:

```bash
git checkout master && git pull
git tag -a v1.0.0 -m "v1.0.0 — summary"
git push origin v1.0.0
# CI builds, smokes, and publishes a GitHub Release with x64 + x86 zips
```

Beta / prerelease: use a SemVer hyphen (`v1.0.0-b1`, not `v1.0.0b1`). CI marks the GitHub Release as prerelease when the version contains `-`.

Optional local tag sim:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFG_RELEASE_VERSION=1.0.0
cmake --build build --config Release
build\bin\fakegame.exe --version
```

## Layout

```
CMakeLists.txt
cmake/version_build.h.in
PROMPT.md
README.md
CODING_STYLE.md
.clang-format
.github/workflows/ci.yml
scripts/build.bat  build-x86.bat  build-wx.bat  verify.bat
scripts/format.bat format.sh  ci-smoke.ps1
tools/launch.ps1   spawn-as.ps1
third_party/Vulkan-Headers/   # vendored Khronos headers
src/
  main.cpp app.* config.* profiles.* block_capture.* events.* exit_codes.hpp
  scene/   # aurora + orbital + highway draw-list sims
  d3d11_renderer.cpp d3d12_renderer.cpp vulkan_renderer.cpp none_renderer.cpp
  renderer.hpp font8x8.hpp log.hpp
```
