# Build a configurable Windows capture-test application ("fakegame")

## Context

I work on **Streamlabs Desktop**, which embeds OBS Studio's `win-capture` plugin (Game Capture /
Window Capture). I need a small, purpose-built Win32 application to test capture behaviour
locally. Copies of system binaries (`charmap.exe` etc.) renamed to game executables do not work —
they fail silently on SxS/manifest resolution.

This is a **developer test tool**, not a game. Optimise for: builds in seconds, zero third-party
dependencies, trivially renameable, fully configurable from the command line.

## Where it lives

Public repo: https://github.com/summeroff/game-capture-target (MIT).  
Local clone may still live under an older folder name (e.g. `C:\work\repos\capture-test-app`);
that path is not load-bearing — behaviour never depends on directory or exe name.
Do not modify any sibling repo under `C:\work\repos\` unless that is the task.

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

---

# Addendum — round 2

Round 1 is built and working: a renamed `cs2.exe` is listed by Game Capture and triggers the
Counter-Strike 2 compatibility warning. The gap is that it then **captures successfully**, so it
exercises the message but not the condition. Real CS2 launched without
`-allow_third_party_software` shows the warning *and* refuses to be captured. Three additions.

## 1. Capture-refusal modes (highest priority)

New flag `--block-capture <none|signature-policy|squat-ipc|unload-hook>`, default `none`.

The point is to reproduce "warning is shown AND capture never succeeds", so the whole failure
path can be exercised: OBS retries the hook forever, the status text stays, no frames arrive.

**`signature-policy`** — most faithful to the CS2/anti-cheat case. Call
`SetProcessMitigationPolicy(ProcessSignaturePolicy, ...)` with `MicrosoftSignedOnly = 1`.
OBS's `graphics-hook64.dll` is not Microsoft-signed, so the injected load fails outright — the
same class of block third-party-software protection applies.
- **Apply it only after** the D3D device, compiled shaders, and swapchain exist. Graphics driver
  user-mode DLLs and `d3dcompiler` load lazily and are not Microsoft-signed either; applying the
  policy too early will break the renderer, not the hook.
- This mitigation is **irreversible for the process** — it cannot be paired with a
  "block later, unblock again" toggle. Say so in `--help` and the README.

**`squat-ipc`** — reversible. Pre-create OBS's hook handshake objects with a DACL that denies
access, so hook initialisation fails even if the DLL loads. Base names are in
`obs-studio/shared/obs-hook-config/graphics-hook-info.h`:
`CaptureHook_Restart`, `CaptureHook_Stop`, `CaptureHook_HookReady`, `CaptureHook_Exit`,
`CaptureHook_Initialize`, `CaptureHook_KeepAlive`, `CaptureHook_TextureMutex1/2`.
The target process id is appended in decimal — confirm the exact construction against
`init_event` / `init_mutex` in `plugins/win-capture/graphics-hook/graphics-hook.c` and
`open_event_gc` in `plugins/win-capture/game-capture.c` rather than trusting this list.

**`unload-hook`** — reversible. Poll the module list for `graphics-hook*.dll` and `FreeLibrary`
it. Crude and racy, but it models detect-and-evict anti-cheat and is easy to toggle live.

Also add:
- `--block-capture-after <seconds>` — capture succeeds first, then is broken. Exercises the
  transition from working to failed, which is where OBS state machines tend to be wrong.
  Reject this combined with `signature-policy` (irreversible), or document that it is one-way.
- `F7` — toggle blocking at runtime for the two reversible modes.
- Log every hook attempt observed, block applied, and block lifted, with a timestamp, so the app
  log can be diffed against the OBS log.

## 2. More graphics APIs

Promote these from "reserved" to real, since OBS's graphics hook has a separate code path per API
and each is a distinct thing to regress:

- `--api d3d12` — the hook's D3D12 path differs substantially from D3D11.
- `--api vulkan` — the hook layers a Vulkan device; worth having even though it is the largest
  single piece of work here. Keep it behind the same renderer interface.
- `--api d3d9` (optional, lower value) — legacy hook path, still present in `graphics-hook`.

Keep `d3d11` the default and `none` as the no-swapchain case. All existing flags
(`--flip-model`, `--buffers`, `--mode`, hotkeys F1–F4, F6) must work per API where meaningful;
where a flag doesn't apply, log that it was ignored rather than silently accepting it.

## 3. Game profile presets

`--profile <name>` sets executable name, window class, and window title together so a tester can
hit a specific `compatibility.json` entry without knowing the fields. `spawn-as.ps1` should
accept `-Profile` and do the rename too.

Coverage worth shipping, drawn from `obs-studio/plugins/win-capture/data/compatibility.json`.
Match flags are `EXE=1`, `TITLE=2`, `CLASS=4`; title matching is a **prefix** match.

| Profile | exe | class | title | flags | sev | Exercises |
|---|---|---|---|---|---|---|
| `cs2` | `cs2.exe` | — | — | 1 | 1 Warning | exe match, the reported case |
| `minecraft` | `javaw.exe` | — | `Minecraft` | 3 | 0 Normal | **exe + title prefix**; try `Minecraft 1.21` |
| `wuthering` | `Client-Win64-Shipping.exe` | — | `Wuthering Waves` | 3 | 1 Warning | second exe+title case |
| `destiny2` | `destiny2.exe` | — | — | 1 | 2 Error | **error severity** |
| `gta-sa` | `gta-sa.exe` | — | — | 1 | 2 Error | error, no URL in message |
| `chromium-gc` | any | `Chrome_WidgetWin_1` | — | 4 | 2 Error | **class-only match** |
| `gaming-services` | any | `GAMINGSERVICESUI_HOSTING_WINDOW_CLASS` | — | 4 | 2 Error | second class-only case |
| `terraria` | `terraria.exe` | — | — | 1 | 0 Normal | normal severity |
| `roblox` | `RobloxPlayerBeta.exe` | — | — | 1 | 1 Warning | entry present for **both** game and window capture |
| `steam` | any | `SDL_app` | — | 4 | 0 Normal | **window-capture-only** entry |
| `excel` | any | `XLMAIN` | — | 4 | 0 Normal | window-capture-only, Office class |

The last two matter because Window Capture is a different source type with its own compatibility
entries and its own properties UI in Streamlabs Desktop — worth being able to drive from the same
tool. Roughly 20 further window-capture-only entries exist (Office, Adobe, Epic, Ubisoft, UWP,
WinUI 3); add them only if cheap, the two above are representative.

A `--profile` must remain overridable by an explicit `--title` / `--class` on the same command
line, so prefix-matching can be tested.

## 4. QA launcher script

The rename-then-launch dance is real friction: a tester currently has to know the executable
name, the window class, and which flags reproduce which condition. Replace that with one
front-door script, `tools/launch.ps1` (PowerShell — this is a Windows-only tool and the team
works in PowerShell; do not write it in bash).

It must serve two very different callers.

**Interactive (a QA tester, no arguments).** `.\tools\launch.ps1` prints a numbered menu of
profiles annotated with what each one is *for*, not just its name — severity, and crucially
whether capture is expected to succeed:

```
  #  Profile          Match      Severity   Capture expected
  1  cs2              exe        Warning    yes
  2  cs2-blocked      exe        Warning    NO  <- reproduces the real CS2 condition
  3  minecraft        exe+title  Normal     yes
  4  destiny2         exe        Error      yes
  5  chromium-gc      class      Error      yes
 ...
```

Then prompt for graphics API and capture-blocking mode (both defaulted so a tester can press
Enter through), echo the fully resolved command line, and launch. Pressing Enter at every prompt
must produce a working, sensible run.

**Non-interactive (automated tests).** Every choice available as a parameter, no prompts, and
machine-readable output:

```powershell
.\tools\launch.ps1 -Profile cs2 -Api d3d11 -BlockCapture signature-policy -Json
.\tools\launch.ps1 -List -Json          # enumerate profiles without launching
.\tools\launch.ps1 -StopAll             # kill everything this script spawned, clean the spawn dir
```

`-Json` emits one object with at minimum:

```json
{
  "profile": "cs2",
  "pid": 41756,
  "hwnd": "0x0023158C",
  "exePath": "C:\\...\\tools\\_spawn\\cs2\\cs2.exe",
  "exeName": "cs2.exe",
  "windowClass": "FakeGameWindowClass",
  "windowTitle": "Counter-Strike 2",
  "obsWindowSetting": "Counter-Strike 2:FakeGameWindowClass:cs2.exe",
  "captureExpected": false
}
```

`obsWindowSetting` is the payload of the `window` property on an OBS game/window capture source:
`title:class:exe`, with `:` encoded as `#3A` and `#` as `#22` (see `ms_build_window_strings` in
`obs-studio/libobs/util/windows/window-helpers.c`). Emitting it is the single most useful thing
this script can do for automated tests — the Streamlabs e2e harness cannot pick the window from
the properties dropdown (antd virtualizes the option list, so only the first handful of windows
in z-order exist in the DOM) and has to set the setting directly via the API instead.

Further requirements:

- **Single source of truth for profiles.** The app owns the table and exposes
  `fakegame.exe --list-profiles --json`; the script consumes that rather than keeping its own
  copy. Adding a profile to the app must not require editing the script.
- **Spawn into per-profile directories** — `tools/_spawn/<profile>/<exe>` — so several renamed
  copies can run at once without colliding. `tools/_spawn/` goes in `.gitignore`.
- **Exit codes**: 0 when the process started and its window appeared; non-zero with a message on
  stderr otherwise. Do not report success for a process that exited immediately — that failure
  mode (a copied binary that dies silently) is exactly what this tool exists to avoid.
- **Wait for the window**, don't just `Start-Process` and return; poll until the window handle
  exists or a timeout elapses, then report it.
- `-ExitAfter <seconds>` passthrough for unattended runs.

## Acceptance criteria (additions)

Report actual observed results, not intent.

9. `--profile cs2 --block-capture signature-policy`: Game Capture shows the CS2 warning **and
   never captures** — preview stays blank, OBS log shows repeated hook attempts. This is the
   condition round 1 could not reproduce.
10. Same with `--block-capture squat-ipc`, and confirm `F7` restores capture without restarting.
11. `--block-capture-after 15`: capture works, then stops at ~15s; record what the Game Capture
    source does on the transition.
12. `--api d3d12` and `--api vulkan`: captured, live, frame counter advancing.
13. `--profile minecraft --title "Minecraft 1.21"` still matches the Minecraft entry (prefix).
14. `--profile chromium-gc`: matched by **class** with no matching exe name, error severity.
15. `--profile steam` selected in a **Window Capture** source surfaces that entry's message.
16. Confirm `signature-policy` does not break the renderer on a machine with a discrete GPU —
    the driver UMD must already be loaded before the policy is applied.
17. `.\tools\launch.ps1` with no arguments, pressing Enter at every prompt, produces a running,
    capturable window.
18. `.\tools\launch.ps1 -Profile cs2 -Json` emits valid JSON; feeding its `obsWindowSetting`
    straight into a Game Capture source's `window` setting selects that window.
19. `-List -Json` output matches `fakegame.exe --list-profiles --json` exactly — proving the
    script holds no duplicate profile table.
20. Launching two different profiles concurrently works; `-StopAll` terminates both and leaves
    `tools/_spawn/` empty.
21. A deliberately broken launch (e.g. profile whose exe cannot start) exits non-zero and says
    why, rather than reporting success.

---

# Addendum — round 3 (bug found in testing)

Round 2 works: `-Profile cs2-blocked` genuinely refuses capture. Verified against Streamlabs
Desktop — the source stayed `0x0` for 24s while the CS2 compatibility warning rendered, and
`GetProcessMitigationPolicy(pid, ProcessSignaturePolicy)` reports `0x5`
(`MicrosoftSignedOnly | MitigationOptIn`). The mechanism is right.

## Bug: `signature-policy` pops a modal Windows dialog on every hook attempt

When OBS injects, Windows shows a modal **"Bad Image"** message box:

```
Counter-Strike 2: cs2.exe - Bad Image
C:\ProgramData\obs-studio-hook\graphics-hook64.dll is either not designed to run on
Windows or it contains an error. ... Error status 0xc0000428.
```

`0xc0000428` is `STATUS_INVALID_IMAGE_HASH` — the loader rejecting the DLL's signature. That is
the mitigation doing exactly its job, so the block itself is correct. The problem is the UI:

1. **It is not faithful.** Real CS2 with anti-cheat refuses injection *silently*. A tester
   reproducing a support issue should see what a user sees, not an OS error box.
2. **It repeats.** Game Capture retries the hook on an interval forever, so this is not one
   dialog — it is a dialog every few seconds until the source is removed. That makes the blocked
   profile painful for manual QA and can block unattended runs.

### Fix

Call, once at process startup:

```c
SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
```

`SEM_FAILCRITICALERRORS` is what suppresses the loader's hard-error box for
`STATUS_INVALID_IMAGE_HASH`; the load still fails, silently.

Two things to get right:

- **Use `SetErrorMode`, not `SetThreadErrorMode`.** The failing load happens on a thread OBS
  creates inside our process via its injector — we never see that thread and cannot set a
  thread-scoped mode on it. The mode must be process-wide.
- **Set it at startup, not next to the mitigation call.** A hook attempt can land at any time,
  including before `--block-capture-after` fires. Setting the error mode is harmless when no
  blocking is configured, so do it unconditionally in `main`.

Log once that error dialogs are suppressed, so a tester who *wants* to see the failure knows why
it is silent. Optionally add `--show-block-errors` to skip the `SetErrorMode` call for anyone
debugging the block mechanism itself.

### Also worth reconsidering

`squat-ipc` produces no dialog by construction and is reversible with `F7`. Consider making it
the default mechanism for the `cs2-blocked` profile, keeping `signature-policy` as the
higher-fidelity opt-in (it blocks at the loader, which is closer to what anti-cheat does). Either
way `signature-policy` needs the `SetErrorMode` fix, since it is selectable directly.

## Acceptance criteria (round 3)

22. `-Profile cs2-blocked` with a Game Capture source pointed at it, left running for **5
    minutes**: zero Windows error dialogs, and the source stays `0x0` the whole time. One
    suppressed dialog is not sufficient evidence — OBS retries, so the test is about the repeat.
23. The OBS log still shows repeated hook attempts over that period, proving capture is being
    attempted and refused rather than never tried.
24. `--show-block-errors` (if implemented) restores the dialog, confirming the suppression is
    deliberate and not an accident of timing.
25. `squat-ipc` and `unload-hook` produce no dialogs either, before or after an `F7` toggle.
