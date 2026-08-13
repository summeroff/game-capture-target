#include "app.hpp"

#include "block_capture.hpp"
#include "events.hpp"
#include "log.hpp"

#include <cmath>
#include <sstream>

#include <dwmapi.h>

#include "resource.h"

#pragma comment(lib, "dwmapi.lib")

namespace
{

constexpr UINT_PTR kTimerId = 1;

struct StylePack
{
  DWORD style;
  DWORD exStyle;
};

StylePack StylesForMode(WindowMode mode)
{
  StylePack s{};
  switch (mode)
  {
  case WindowMode::Windowed:
    s.style = WS_OVERLAPPEDWINDOW;
    s.exStyle = WS_EX_APPWINDOW;
    break;
  case WindowMode::Borderless:
  case WindowMode::FullscreenExclusive:
    s.style = WS_POPUP;
    s.exStyle = WS_EX_APPWINDOW;
    break;
  }
  return s;
}

} // namespace

App::App(Config cfg) : cfg_(std::move(cfg))
{
  windowedW_ = cfg_.width;
  windowedH_ = cfg_.height;
  windowedPlacement_.length = sizeof(windowedPlacement_);
  QueryPerformanceFrequency(&qpcFreq_);
  frameBudgetSec_ = cfg_.fps > 0 ? (1.0 / static_cast<double>(cfg_.fps)) : (1.0 / 60.0);
  if (cfg_.churnHz > 0)
  {
    churn_ = true;
    churnPeriodSec_ = 1.0 / cfg_.churnHz;
  }
  nextSwapchainAfter_ =
      cfg_.recreateSwapchainAfter.afterHooked ? 0 : cfg_.recreateSwapchainAfter.afterSec;
  nextDeviceAfter_ = cfg_.recreateDeviceAfter.afterHooked ? 0 : cfg_.recreateDeviceAfter.afterSec;
  nextResizeAfter_ = cfg_.resizeAfter.afterHooked ? 0 : cfg_.resizeAfter.afterSec;
  nextModeAfter_ = cfg_.modeCycleAfter.afterHooked ? 0 : cfg_.modeCycleAfter.afterSec;
  waitHookSwapchain_ = cfg_.recreateSwapchainAfter.afterHooked;
  waitHookDevice_ = cfg_.recreateDeviceAfter.afterHooked;
  waitHookResize_ = cfg_.resizeAfter.afterHooked;
  waitHookMode_ = cfg_.modeCycleAfter.afterHooked;
}

App::~App()
{
  unloadHookArmed_ = false;
  ReleaseSquatIpc();
  // Teardown via unique_ptr dtor only - each renderer destructor calls Shutdown().
  // Avoid an extra explicit Shutdown() here (double-teardown risk if not idempotent).
  renderer_.reset();
  if (hwnd_)
  {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (classOwned_)
  {
    UnregisterClassW(cfg_.windowClass.c_str(), GetModuleHandleW(nullptr));
    classOwned_ = false;
    atom_ = 0;
  }
}

App* App::FromHwnd(HWND hwnd)
{
  return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool App::Initialize(std::wstring* error)
{
  if (!RegisterWindowClass(error))
  {
    exitCode_ = FgExit::WindowCreateFailed;
    return false;
  }
  if (!CreateMainWindow(error))
  {
    exitCode_ = FgExit::WindowCreateFailed;
    return false;
  }
  if (!CreateRenderer(error))
  {
    exitCode_ = FgExit::RendererInitFailed;
    return false;
  }
  InitScene();

  // Capture block AFTER renderer is fully up (signature-policy needs driver UMDs loaded).
  if (cfg_.blockCapture != BlockCaptureMode::None)
  {
    if (cfg_.blockCaptureAfterSeconds > 0)
    {
      blockPending_ = true;
      Log("block: will apply %s after %d s",
          Narrow(BlockCaptureModeName(cfg_.blockCapture)).c_str(), cfg_.blockCaptureAfterSeconds);
    } else
    {
      if (!ApplyBlockNow(error))
      {
        exitCode_ = FgExit::BlockUnverified;
        return false;
      }
    }
  }

  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);

  // Ensure not cloaked / visible for OBS check_window_valid.
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
  {
    Log("warn: window is cloaked at startup");
  }

  QueryPerformanceCounter(&qpcStart_);
  qpcLast_ = qpcStart_;

  // ~1ms timer as a wake-up; real pacing is in Frame().
  SetTimer(hwnd_, kTimerId, 1, nullptr);

  Log("app: hwnd=%p pid=%lu class=%s", hwnd_, GetCurrentProcessId(),
      Narrow(cfg_.windowClass).c_str());

  EmitReady();
  return true;
}

bool App::RegisterWindowClass(std::wstring* error)
{
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &App::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr; // we paint fully each frame
  wc.lpszClassName = cfg_.windowClass.c_str();
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
  if (!wc.hIcon)
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;

  atom_ = RegisterClassExW(&wc);
  if (atom_)
  {
    classOwned_ = true;
    return true;
  }
  const DWORD err = GetLastError();
  if (err == ERROR_CLASS_ALREADY_EXISTS)
  {
    // Same-process reuse (e.g. a leftover class). Do not UnregisterClass — we did not register it.
    classOwned_ = false;
    atom_ = 0;
    return true;
  }
  *error = L"RegisterClassEx failed, GetLastError=" + std::to_wstring(err);
  return false;
}

bool App::CreateMainWindow(std::wstring* error)
{
  const StylePack sp = StylesForMode(cfg_.mode);

  DWORD ex = sp.exStyle;
  if (cfg_.topmost)
    ex |= WS_EX_TOPMOST;
  // Never WS_EX_TOOLWINDOW - OBS rejects those.

  RECT rc{0, 0, cfg_.width, cfg_.height};
  AdjustWindowRectEx(&rc, sp.style, FALSE, ex);
  const int ww = rc.right - rc.left;
  const int wh = rc.bottom - rc.top;

  hwnd_ = CreateWindowExW(ex, cfg_.windowClass.c_str(), cfg_.title.c_str(), sp.style, CW_USEDEFAULT,
                          CW_USEDEFAULT, ww, wh, nullptr, nullptr, GetModuleHandleW(nullptr), this);
  if (!hwnd_)
  {
    *error = L"CreateWindowEx failed, GetLastError=" + std::to_wstring(GetLastError());
    return false;
  }

  ApplyWindowMode(cfg_.mode, true);
  return true;
}

bool App::CreateRenderer(std::wstring* error)
{
  switch (cfg_.api)
  {
  case GraphicsApi::D3D11:
    renderer_ = CreateD3D11Renderer();
    break;
  case GraphicsApi::D3D12:
    renderer_ = CreateD3D12Renderer();
    break;
  case GraphicsApi::Vulkan:
    renderer_ = CreateVulkanRenderer();
    break;
  case GraphicsApi::None:
    renderer_ = CreateNoneRenderer();
    break;
  default:
    *error = L"unsupported api";
    return false;
  }

  if (!renderer_->Init(hwnd_, cfg_, error))
  {
    renderer_.reset();
    return false;
  }

  // Flags that don't apply: log rather than silent accept.
  if (cfg_.api == GraphicsApi::None || cfg_.api == GraphicsApi::Vulkan)
  {
    if (!cfg_.flipModel)
      Log("note: --flip-model ignored for api=%s", Narrow(GraphicsApiName(cfg_.api)).c_str());
  }
  return true;
}

void App::InitScene()
{
  // Draw-list scenes are d3d11 / d3d12 / GDI. Vulkan has its own default visual
  // (cycling clear + title frame). --scene is accepted but not rendered there.
  if (cfg_.api == GraphicsApi::Vulkan)
  {
    Log("scene: vulkan native visual (cycling clear + title frame); --scene %s ignored",
        Narrow(scene::SceneIdName(cfg_.scene)).c_str());
    return;
  }

  scene_ = scene::CreateScene(cfg_.scene);
  scene::SceneConfig sc;
  sc.id = cfg_.scene;
  sc.seed = cfg_.sceneSeed;
  sc.intensity = 1.f;
  int cw = cfg_.width, ch = cfg_.height;
  if (hwnd_)
    ClientSize(&cw, &ch);
  scene_->Reset(sc, cw, ch);
  Log("scene: %s seed=0x%08X (draw-list; d3d11 reference, d3d12/GDI approximate)",
      Narrow(scene_->Name()).c_str(), cfg_.sceneSeed);
}

bool App::ApplyBlockNow(std::wstring* error)
{
  const std::string modeStr = Narrow(BlockCaptureModeName(cfg_.blockCapture));

  switch (cfg_.blockCapture)
  {
  case BlockCaptureMode::None:
    return true;
  case BlockCaptureMode::SignaturePolicy:
    if (!ApplySignaturePolicy(error))
    {
      const std::string det = error ? Narrow(*error) : std::string("apply failed");
      EmitBlockActive(modeStr.c_str(), false, det.c_str());
      return false;
    }
    if (!VerifySignaturePolicy())
    {
      if (error)
        *error = L"signature-policy applied but GetProcessMitigationPolicy verify failed";
      EmitBlockActive(modeStr.c_str(), false, "verify failed");
      return false;
    }
    blockActive_ = true;
    cfg_.captureExpected = false;
    // Do not emit hook_blocked here - only when an inject attempt is observed.
    EmitBlockActive(modeStr.c_str(), true, "MicrosoftSignedOnly=1");
    return true;
  case BlockCaptureMode::SquatIpc:
    if (!ApplySquatIpc(error))
    {
      const std::string det = error ? Narrow(*error) : std::string("apply failed");
      EmitBlockActive(modeStr.c_str(), false, det.c_str());
      return false;
    }
    {
      std::wstring detail;
      if (!VerifySquatIpc(&detail))
      {
        if (error)
          *error = L"squat-ipc verify failed: " + detail;
        const std::string det = Narrow(detail);
        EmitBlockActive(modeStr.c_str(), false, det.c_str());
        ReleaseSquatIpc();
        return false;
      }
      blockActive_ = true;
      reversibleMode_ = BlockCaptureMode::SquatIpc;
      cfg_.captureExpected = false;
      const std::string det = Narrow(detail);
      // Do not emit hook_blocked here - only when an inject attempt is observed.
      EmitBlockActive(modeStr.c_str(), true, det.c_str());
    }
    return true;
  case BlockCaptureMode::UnloadHook:
    unloadHookArmed_ = true;
    blockActive_ = true;
    reversibleMode_ = BlockCaptureMode::UnloadHook;
    cfg_.captureExpected = false;
    Log("block: unload-hook ARMED (will FreeLibrary graphics-hook*.dll when seen)");
    // Armed is not verified unload success - verified flips true on first unload.
    EmitBlockActive(modeStr.c_str(), false, "armed");
    return true;
  }
  return true;
}

void App::EmitBlockActive(const char* mode, bool verified, const char* detail)
{
  std::ostringstream o;
  o << "{\"event\":\"block_active\",\"mode\":\"" << JsonEscapeUtf8(mode ? mode : "")
    << "\",\"verified\":" << (verified ? "true" : "false") << ",\"ts\":\"" << EventsTimestamp()
    << "\"";
  if (detail && *detail)
    o << ",\"detail\":\"" << JsonEscapeUtf8(detail) << "\"";
  o << "}";
  EmitEventJson(o.str());
  Log("block_active: mode=%s verified=%s %s", mode ? mode : "?", verified ? "true" : "false",
      detail ? detail : "");
}

void App::EmitReady()
{
  if (readyEmitted_ || !hwnd_)
    return;
  readyEmitted_ = true;

  int cw = 0, ch = 0;
  ClientSize(&cw, &ch);
  if (cw <= 0)
    cw = cfg_.width;
  if (ch <= 0)
    ch = cfg_.height;

  const std::string exe = CurrentExeBaseName();
  const std::string title = Narrow(cfg_.title);
  const std::string cls = Narrow(cfg_.windowClass);
  const std::string obs = BuildObsWindowSetting(title, cls, exe);
  const std::string api = Narrow(GraphicsApiName(cfg_.api));
  const std::string block = Narrow(BlockCaptureModeName(cfg_.blockCapture));

  std::ostringstream o;
  o << "{\"event\":\"ready\"" << ",\"pid\":" << GetCurrentProcessId() << ",\"hwnd\":\""
    << FormatHwnd(hwnd_) << "\"" << ",\"exe\":\"" << JsonEscapeUtf8(exe) << "\""
    << ",\"windowClass\":\"" << JsonEscapeUtf8(cls) << "\"" << ",\"windowTitle\":\""
    << JsonEscapeUtf8(title) << "\"" << ",\"clientWidth\":" << cw << ",\"clientHeight\":" << ch
    << ",\"api\":\"" << JsonEscapeUtf8(api) << "\"" << ",\"blockCapture\":\""
    << JsonEscapeUtf8(block) << "\""
    << ",\"captureExpected\":" << (cfg_.captureExpected ? "true" : "false")
    << ",\"obsWindowSetting\":\"" << JsonEscapeUtf8(obs) << "\"";
  if (!cfg_.profileId.empty())
    o << ",\"profile\":\"" << JsonEscapeUtf8(cfg_.profileId) << "\"";
  if (!cfg_.instanceId.empty())
    o << ",\"instance\":\"" << JsonEscapeUtf8(Narrow(cfg_.instanceId)) << "\"";
  o << ",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
  // Always log human-readable ready line for non-events consumers / correlation.
  Log("ready: hwnd=%s pid=%lu obsWindowSetting=%s client=%dx%d", FormatHwnd(hwnd_).c_str(),
      GetCurrentProcessId(), obs.c_str(), cw, ch);

  // Quiet failure mode: profile identity without rename / override that breaks OBS match.
  EmitProfileMatchWarnings();
}

void App::EmitProfileMatchWarnings()
{
  if (cfg_.profileId.empty() || cfg_.profileMatchFlags == 0)
    return;

  const int mf = cfg_.profileMatchFlags;

  auto emitMismatch = [&](const char* code, const std::string& expected, const std::string& actual,
                          const char* detail) {
    std::ostringstream o;
    o << "{\"event\":\"warning\",\"code\":\"" << code << "\"" << ",\"expected\":\""
      << JsonEscapeUtf8(expected) << "\"" << ",\"actual\":\"" << JsonEscapeUtf8(actual) << "\""
      << ",\"profile\":\"" << JsonEscapeUtf8(cfg_.profileId) << "\"" << ",\"detail\":\""
      << JsonEscapeUtf8(detail) << "\"" << ",\"ts\":\"" << EventsTimestamp() << "\"}";
    EmitEventJson(o.str());
    Log("warning: %s expected=%s actual=%s (profile %s)", code, expected.c_str(), actual.c_str(),
        cfg_.profileId.c_str());
  };

  // EXE-matched profiles need the process image name to match the compatibility entry.
  if ((mf & 1) != 0 && !cfg_.profileExeName.empty())
  {
    const std::string actual = CurrentExeBaseName();
    if (_stricmp(actual.c_str(), cfg_.profileExeName.c_str()) != 0)
    {
      emitMismatch("exe_mismatch", cfg_.profileExeName, actual,
                   "profile matches on exe; rename the binary (launch.ps1 / spawn-as.ps1) "
                   "or the compatibility entry will not apply");
    }
  }

  // Title-matched (often as prefix, e.g. Minecraft "Minecraft 1.21").
  if ((mf & 2) != 0 && !cfg_.profileExpectedTitle.empty())
  {
    const std::wstring& exp = cfg_.profileExpectedTitle;
    const std::wstring& act = cfg_.title;
    const bool prefixOk =
        act.size() >= exp.size() && _wcsnicmp(act.c_str(), exp.c_str(), exp.size()) == 0;
    if (!prefixOk)
    {
      emitMismatch("title_mismatch", Narrow(exp), Narrow(act),
                   "profile matches on title prefix; set --title to start with the profile title "
                   "or the compatibility entry will not apply");
    }
  }

  // Class-matched: warn if CLI/--instance path left a different class than the profile declared.
  if ((mf & 4) != 0 && !cfg_.profileExpectedClass.empty())
  {
    if (_wcsicmp(cfg_.windowClass.c_str(), cfg_.profileExpectedClass.c_str()) != 0)
    {
      emitMismatch("class_mismatch", Narrow(cfg_.profileExpectedClass), Narrow(cfg_.windowClass),
                   "profile matches on window class; class override will not match the "
                   "compatibility entry");
    }
  }
}

void App::LiftReversibleBlock()
{
  if (reversibleMode_ == BlockCaptureMode::SquatIpc)
  {
    ReleaseSquatIpc();
  }
  unloadHookArmed_ = false;
  blockActive_ = false;
  reversibleMode_ = BlockCaptureMode::None;
  cfg_.captureExpected = true;
  hookBlockedEmitted_ = false;
  hookedEmitted_ = false;
  Log("block: reversible block LIFTED - capture may succeed again");
}

void App::EmitHookBlocked(const char* reason)
{
  if (hookBlockedEmitted_)
    return;
  hookBlockedEmitted_ = true;
  std::ostringstream o;
  o << "{\"event\":\"hook_blocked\",\"reason\":\"" << JsonEscapeUtf8(reason ? reason : "")
    << "\",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
  Log("hook_blocked: reason=%s", reason ? reason : "?");
}

void App::EmitUnhooked(const char* reason)
{
  std::ostringstream o;
  o << "{\"event\":\"unhooked\",\"reason\":\"" << JsonEscapeUtf8(reason ? reason : "")
    << "\",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
  Log("unhooked: reason=%s", reason ? reason : "?");
}

void App::TickHookEvents()
{
  const HookModuleState st = QueryHookModule();
  if (st.present && !hookPresent_)
  {
    hookPresent_ = true;
    std::ostringstream o;
    o << "{\"event\":\"hook_attempt\",\"module\":\"" << JsonEscapeUtf8(st.baseName)
      << "\",\"ts\":\"" << EventsTimestamp() << "\"}";
    EmitEventJson(o.str());
    Log("hook_attempt: %s", st.baseName.c_str());
  } else if (!st.present && hookPresent_)
  {
    hookPresent_ = false;
  }

  const bool hookReady = IsHookIpcReadyPresent();

  // Capture-complete signal: module present + HookReady IPC object exists.
  // (Stronger than DLL-mapped alone; DllMain can load then fail init_signals.)
  if (st.present && !blockActive_ && !hookedEmitted_ && hookReady)
  {
    hookedEmitted_ = true;
    if (firstHookedAtSec_ <= 0)
      firstHookedAtSec_ = elapsedSec_;
    std::ostringstream h;
    h << "{\"event\":\"hooked\",\"module\":\"" << JsonEscapeUtf8(st.baseName) << "\",\"ts\":\""
      << EventsTimestamp() << "\"}";
    EmitEventJson(h.str());
    Log("hooked: %s (HookReady present)", st.baseName.c_str());
  }

  // After a successful hooked, HookReady going away is hook teardown
  // (reason hook_ready_gone if the module is still mapped, module_gone if not).
  // Common cause: unload-hook / FreeLibrary. In-process F3/F4/F6 recreate does
  // NOT drop HookReady — OBS keeps graphics-hook loaded and re-acquires.
  // Churn tests wait on swapchain_recreated / device_recreated, not unhooked.
  if (hookedEmitted_ && !hookReady)
  {
    hookedEmitted_ = false;
    EmitUnhooked(st.present ? "hook_ready_gone" : "module_gone");
  }

  // Observed inject while a block is armed => hook_blocked (real attempt, not arm-time).
  if (st.present && blockActive_)
  {
    if (cfg_.blockCapture == BlockCaptureMode::SignaturePolicy)
      EmitHookBlocked("signature-policy");
    else if (reversibleMode_ == BlockCaptureMode::SquatIpc)
      EmitHookBlocked("squat-ipc");
    // unload-hook emits on successful FreeLibrary in TickBlockCapture
  }
}

void App::TickBlockCapture()
{
  if (blockPending_ && cfg_.blockCaptureAfterSeconds > 0 &&
      elapsedSec_ >= static_cast<double>(cfg_.blockCaptureAfterSeconds))
  {
    blockPending_ = false;
    std::wstring err;
    Log("block: block-capture-after %d reached - applying %s", cfg_.blockCaptureAfterSeconds,
        Narrow(BlockCaptureModeName(cfg_.blockCapture)).c_str());
    if (!ApplyBlockNow(&err))
    {
      Log("block: apply failed: %s", Narrow(err).c_str());
      exitCode_ = FgExit::BlockUnverified;
      RequestQuit();
      return;
    }
  }

  PollHookModules(true);
  TickHookEvents();

  if (unloadHookArmed_)
  {
    if (TryUnloadGraphicsHook())
    {
      EmitHookBlocked("unload-hook");
      // First successful unload promotes block_active to verified.
      EmitBlockActive("unload-hook", true, "unloaded");
      hookPresent_ = false;
      // Do not just clear hookedEmitted_ — a prior hooked (e.g. --block-capture-after)
      // must emit unhooked when HookReady is torn down by FreeLibrary.
      if (hookedEmitted_)
      {
        hookedEmitted_ = false;
        EmitUnhooked("unload-hook");
      }
      // keep armed - OBS will reinject
    }
  }
}

void App::OnHotkeyBlockToggle()
{
  Log("hotkey F7: toggle capture block");
  if (cfg_.blockCapture == BlockCaptureMode::SignaturePolicy ||
      (blockActive_ && reversibleMode_ == BlockCaptureMode::None &&
       cfg_.blockCapture == BlockCaptureMode::SignaturePolicy))
  {
    Log("block: signature-policy is irreversible - F7 ignored");
    return;
  }

  if (blockActive_ && reversibleMode_ != BlockCaptureMode::None)
  {
    LiftReversibleBlock();
    return;
  }

  // Arm current configured reversible mode, defaulting to squat-ipc if none.
  if (cfg_.blockCapture == BlockCaptureMode::None ||
      cfg_.blockCapture == BlockCaptureMode::SignaturePolicy)
  {
    cfg_.blockCapture = BlockCaptureMode::SquatIpc;
  }
  std::wstring err;
  if (!ApplyBlockNow(&err))
  {
    Log("block: F7 apply failed: %s", Narrow(err).c_str());
    exitCode_ = FgExit::BlockUnverified;
    RequestQuit();
  }
}

void App::GetMonitorRect(RECT* out) const
{
  HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi))
  {
    *out = mi.rcMonitor;
  } else
  {
    out->left = 0;
    out->top = 0;
    out->right = GetSystemMetrics(SM_CXSCREEN);
    out->bottom = GetSystemMetrics(SM_CYSCREEN);
  }
}

void App::ClientSize(int* w, int* h) const
{
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  *w = rc.right - rc.left;
  *h = rc.bottom - rc.top;
}

void App::ApplyWindowMode(WindowMode mode, bool initial)
{
  cfg_.mode = mode;
  const StylePack sp = StylesForMode(mode);

  DWORD ex = sp.exStyle;
  if (cfg_.topmost)
    ex |= WS_EX_TOPMOST;

  SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(sp.style));
  SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(ex));

  if (mode == WindowMode::Windowed)
  {
    int w = windowedW_;
    int h = windowedH_;
    RECT rc{0, 0, w, h};
    AdjustWindowRectEx(&rc, sp.style, FALSE, ex);
    const int ww = rc.right - rc.left;
    const int wh = rc.bottom - rc.top;

    UINT flags = SWP_FRAMECHANGED | SWP_SHOWWINDOW;
    HWND insertAfter = cfg_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST;

    if (!initial && windowedPlacement_.length == sizeof(windowedPlacement_) &&
        windowedPlacement_.rcNormalPosition.right > windowedPlacement_.rcNormalPosition.left)
    {
      const RECT& r = windowedPlacement_.rcNormalPosition;
      SetWindowPos(hwnd_, insertAfter, r.left, r.top, r.right - r.left, r.bottom - r.top, flags);
    } else
    {
      SetWindowPos(hwnd_, insertAfter, 100, 100, ww, wh, flags);
    }
  } else
  {
    // Save windowed placement before going borderless/exclusive.
    if (!initial && (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_OVERLAPPEDWINDOW))
    {
      windowedPlacement_.length = sizeof(windowedPlacement_);
      GetWindowPlacement(hwnd_, &windowedPlacement_);
      ClientSize(&windowedW_, &windowedH_);
    }

    RECT mon{};
    GetMonitorRect(&mon);
    UINT flags = SWP_FRAMECHANGED | SWP_SHOWWINDOW;
    HWND insertAfter = cfg_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd_, insertAfter, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                 flags);
  }

  std::wstring err;
  int cw = 0, ch = 0;
  ClientSize(&cw, &ch);
  if (renderer_)
  {
    if (cw > 0 && ch > 0)
      renderer_->Resize(cw, ch, &err);
    renderer_->SetMode(mode, &err);
  }

  ClientSize(&cw, &ch);
  Log("mode -> %s client=%dx%d", Narrow(WindowModeName(mode)).c_str(), cw, ch);
}

int App::Run()
{
  MSG msg{};
  while (running_)
  {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT)
      {
        running_ = false;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running_)
      break;

    Frame();
  }
  return static_cast<int>(exitCode_);
}

void App::RequestQuit()
{
  running_ = false;
  if (hwnd_)
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void App::Frame()
{
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const double dt = static_cast<double>(now.QuadPart - qpcLast_.QuadPart) /
                    static_cast<double>(qpcFreq_.QuadPart);

  if (dt < frameBudgetSec_ && cfg_.vsync == false)
  {
    // Light spin/yield to hit fps target when vsync off.
    if (frameBudgetSec_ - dt > 0.002)
      Sleep(1);
    return;
  }
  // With vsync on, Present blocks; still gate on budget to avoid overwork when tabbed away.
  if (dt < frameBudgetSec_ * 0.5 && cfg_.vsync)
  {
    Sleep(0);
    return;
  }

  qpcLast_ = now;
  const double prevElapsed = elapsedSec_;
  elapsedSec_ = static_cast<double>(now.QuadPart - qpcStart_.QuadPart) /
                static_cast<double>(qpcFreq_.QuadPart);
  lastDt_ = static_cast<float>(elapsedSec_ - prevElapsed);
  if (lastDt_ < 0.f)
    lastDt_ = 0.f;
  if (lastDt_ > 0.1f)
    lastDt_ = 0.1f;

  if (cfg_.exitAfterSeconds > 0 && elapsedSec_ >= cfg_.exitAfterSeconds)
  {
    Log("exit-after %d reached - quitting", cfg_.exitAfterSeconds);
    RequestQuit();
    return;
  }

  if (churn_)
    TickChurn();

  TickBlockCapture();
  TickScheduled();

  int cw = 0, ch = 0;
  ClientSize(&cw, &ch);

  if (scene_)
  {
    if (cw > 0 && ch > 0)
      scene_->Resize(cw, ch);
    scene_->Update(elapsedSec_, lastDt_, cw > 0 ? cw : cfg_.width, ch > 0 ? ch : cfg_.height);
    // Keep prims capacity; only reset draw-list payload fields.
    // Default backdrop is Solid - never Aurora (avoids nebula PS if Emit forgets to set it).
    sceneDraw_.backdrop = scene::BackdropId::Solid;
    sceneDraw_.clearR = 0.01f;
    sceneDraw_.clearG = 0.01f;
    sceneDraw_.clearB = 0.03f;
    sceneDraw_.flashR = 1.f;
    sceneDraw_.flashG = 0.55f;
    sceneDraw_.flashB = 0.2f;
    sceneDraw_.flashA = 0.f;
    sceneDraw_.hud = {};
    sceneDraw_.prims.clear();
    scene_->Emit(sceneDraw_);

    // Emit-path diagnostics (not just CLI selection).
    const bool logNow =
        cfg_.verbose && ((frameIndex_ == 0) || (frameIndex_ == 1) || (frameIndex_ % 120 == 0));
    if (logNow)
    {
      const scene::PrimCounts pc = scene::CountPrims(sceneDraw_.prims);
      Log("scene-emit: name=%s id=%d backdrop=%s(%d) clear=%.3f,%.3f,%.3f flashA=%.2f "
          "prims=%d [solid=%d orb=%d ring=%d line=%d tri=%d circle=%d] hud1=\"%s\"",
          Narrow(scene_->Name()).c_str(), static_cast<int>(scene_->Id()),
          Narrow(scene::BackdropIdName(sceneDraw_.backdrop)).c_str(),
          static_cast<int>(sceneDraw_.backdrop), sceneDraw_.clearR, sceneDraw_.clearG,
          sceneDraw_.clearB, sceneDraw_.flashA, pc.total, pc.solid, pc.orb, pc.ring, pc.line,
          pc.tri, pc.circle, Narrow(sceneDraw_.hud.line1).c_str());
      if (!sceneDraw_.prims.empty())
      {
        const auto& p0 = sceneDraw_.prims.front();
        const auto& pN = sceneDraw_.prims.back();
        Log("scene-emit: first kind=%s xy=%.1f,%.1f wh=%.1f,%.1f  last kind=%s xy=%.1f,%.1f",
            Narrow(scene::PrimKindName(p0.kind)).c_str(), p0.x, p0.y, p0.w, p0.h,
            Narrow(scene::PrimKindName(pN.kind)).c_str(), pN.x, pN.y);
      }
    }
  }

  FrameInfo fi{};
  fi.frameIndex = frameIndex_;
  fi.elapsedSec = elapsedSec_;
  fi.clientW = cw;
  fi.clientH = ch;
  fi.pid = GetCurrentProcessId();
  fi.windowClass = cfg_.windowClass;
  fi.windowTitle = cfg_.title;
  fi.mode = cfg_.mode;
  fi.vsync = cfg_.vsync;
  fi.flipModel = cfg_.flipModel;
  fi.buffers = cfg_.buffers;
  fi.noHud = cfg_.noHud;
  fi.verbose = cfg_.verbose;
  fi.sceneName = scene_ ? scene_->Name() : scene::SceneIdName(cfg_.scene);
  fi.sceneSeed = cfg_.sceneSeed;
  fi.sceneDraw = scene_ ? &sceneDraw_ : nullptr;
  fi.dumpFramePath = cfg_.dumpFramePath.empty() ? nullptr : cfg_.dumpFramePath.c_str();

  if (renderer_)
    renderer_->Render(fi);

  ++frameIndex_;
}

void App::TickChurn()
{
  churnAccum_ += lastDt_;
  if (churnAccum_ < churnPeriodSec_)
    return;
  churnAccum_ = 0.0;

  std::wstring err;
  if ((churnPhase_ % 2) == 0)
  {
    const int w = (churnPhase_ % 4 == 0) ? 1024 : 1600;
    const int h = (churnPhase_ % 4 == 0) ? 576 : 900;
    Log("churn: resize swapchain -> %dx%d", w, h);
    if (cfg_.mode == WindowMode::Windowed)
    {
      windowedW_ = w;
      windowedH_ = h;
      cfg_.width = w;
      cfg_.height = h;
      const StylePack sp = StylesForMode(WindowMode::Windowed);
      RECT rc{0, 0, w, h};
      DWORD ex = sp.exStyle | (cfg_.topmost ? WS_EX_TOPMOST : 0);
      AdjustWindowRectEx(&rc, sp.style, FALSE, ex);
      SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (renderer_ && !renderer_->Resize(w, h, &err))
    {
      Log("churn: resize failed: %s", Narrow(err).c_str());
    } else
    {
      int cw = 0, ch = 0;
      ClientSize(&cw, &ch);
      std::ostringstream o;
      o << "{\"event\":\"resized\",\"clientWidth\":" << cw << ",\"clientHeight\":" << ch
        << ",\"ts\":\"" << EventsTimestamp() << "\"}";
      EmitEventJson(o.str());
    }
  } else
  {
    Log("churn: recreate swapchain");
    if (renderer_ && !renderer_->RecreateSwapchain(&err))
    {
      Log("churn: recreate swapchain failed: %s", Narrow(err).c_str());
    } else if (renderer_)
    {
      std::ostringstream o;
      o << "{\"event\":\"swapchain_recreated\",\"ts\":\"" << EventsTimestamp() << "\"}";
      EmitEventJson(o.str());
    }
  }
  ++churnPhase_;
}

void App::TickScheduled()
{
  auto fire = [&](double* next, bool* waitHook, const ScheduledAfter& spec,
                  void (App::*fn)(const char*)) {
    if (*waitHook)
    {
      if (firstHookedAtSec_ <= 0)
        return;
      *next = firstHookedAtSec_ + spec.afterSec;
      *waitHook = false;
    }
    if (*next <= 0 || elapsedSec_ < *next)
      return;
    (this->*fn)("scheduled");
    if (spec.repeat)
      *next += spec.afterSec;
    else
      *next = 0;
  };
  fire(&nextModeAfter_, &waitHookMode_, cfg_.modeCycleAfter, &App::DoModeCycle);
  fire(&nextResizeAfter_, &waitHookResize_, cfg_.resizeAfter, &App::DoResizePreset);
  fire(&nextSwapchainAfter_, &waitHookSwapchain_, cfg_.recreateSwapchainAfter,
       &App::DoRecreateSwapchain);
  fire(&nextDeviceAfter_, &waitHookDevice_, cfg_.recreateDeviceAfter, &App::DoRecreateDevice);
}

void App::DoModeCycle(const char* why)
{
  const WindowMode next = NextWindowMode(cfg_.mode);
  Log("%s: mode %s -> %s", why, Narrow(WindowModeName(cfg_.mode)).c_str(),
      Narrow(WindowModeName(next)).c_str());
  ApplyWindowMode(next, false);
  int cw = 0, ch = 0;
  ClientSize(&cw, &ch);
  std::ostringstream o;
  o << "{\"event\":\"mode_changed\",\"mode\":\""
    << JsonEscapeUtf8(Narrow(WindowModeName(cfg_.mode))) << "\",\"clientWidth\":" << cw
    << ",\"clientHeight\":" << ch << ",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
}

void App::DoResizePreset(const char* why)
{
  static const int kSizes[][2] = {{1280, 720}, {1920, 1080}, {1024, 576}, {1600, 900}};
  resizeToggle_ = (resizeToggle_ + 1) % 4;
  const int w = kSizes[resizeToggle_][0];
  const int h = kSizes[resizeToggle_][1];
  Log("%s: resize -> %dx%d", why, w, h);

  cfg_.width = w;
  cfg_.height = h;
  if (cfg_.mode == WindowMode::Windowed)
  {
    windowedW_ = w;
    windowedH_ = h;
    const StylePack sp = StylesForMode(WindowMode::Windowed);
    RECT rc{0, 0, w, h};
    DWORD ex = sp.exStyle | (cfg_.topmost ? WS_EX_TOPMOST : 0);
    AdjustWindowRectEx(&rc, sp.style, FALSE, ex);
    SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
  }

  std::wstring err;
  if (renderer_ && !renderer_->Resize(w, h, &err))
  {
    Log("resize failed: %s", Narrow(err).c_str());
    return;
  }
  int cw = 0, ch = 0;
  ClientSize(&cw, &ch);
  std::ostringstream o;
  o << "{\"event\":\"resized\",\"clientWidth\":" << cw << ",\"clientHeight\":" << ch << ",\"ts\":\""
    << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
}

void App::DoRecreateSwapchain(const char* why)
{
  Log("%s: recreate swapchain", why);
  std::wstring err;
  if (!renderer_ || !renderer_->RecreateSwapchain(&err))
  {
    Log("recreate swapchain failed: %s", Narrow(err).c_str());
    return;
  }
  std::ostringstream o;
  o << "{\"event\":\"swapchain_recreated\",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
}

void App::DoRecreateDevice(const char* why)
{
  Log("%s: recreate device", why);
  std::wstring err;
  if (!renderer_ || !renderer_->RecreateDevice(&err))
  {
    Log("recreate device failed: %s", Narrow(err).c_str());
    return;
  }
  std::ostringstream o;
  o << "{\"event\":\"device_recreated\",\"ts\":\"" << EventsTimestamp() << "\"}";
  EmitEventJson(o.str());
}

void App::OnHotkeyMode()
{
  DoModeCycle("hotkey F1");
}

void App::OnHotkeyResize()
{
  DoResizePreset("hotkey F2");
}

void App::OnHotkeyRecreateSwapchain()
{
  DoRecreateSwapchain("hotkey F3");
}

void App::OnHotkeyRecreateDevice()
{
  DoRecreateDevice("hotkey F4");
}

void App::OnHotkeyTitle()
{
  ++titleCounter_;
  cfg_.title += L" #";
  cfg_.title += std::to_wstring(titleCounter_);
  SetWindowTextW(hwnd_, cfg_.title.c_str());
  Log("hotkey F5: title -> %s", Narrow(cfg_.title).c_str());
  EmitProfileMatchWarnings();
}

void App::OnHotkeyChurn()
{
  churn_ = !churn_;
  churnAccum_ = 0.0;
  Log("hotkey F6: churn %s", churn_ ? "ON" : "OFF");
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_NCCREATE)
  {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    auto* self = static_cast<App*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  App* self = FromHwnd(hwnd);
  if (!self)
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  return self->HandleMessage(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
  case WM_KEYDOWN:
    switch (wParam)
    {
    case VK_ESCAPE:
      Log("hotkey Esc: quit");
      RequestQuit();
      return 0;
    case VK_F1:
      OnHotkeyMode();
      return 0;
    case VK_F2:
      OnHotkeyResize();
      return 0;
    case VK_F3:
      OnHotkeyRecreateSwapchain();
      return 0;
    case VK_F4:
      OnHotkeyRecreateDevice();
      return 0;
    case VK_F5:
      OnHotkeyTitle();
      return 0;
    case VK_F6:
      OnHotkeyChurn();
      return 0;
    case VK_F7:
      OnHotkeyBlockToggle();
      return 0;
    default:
      break;
    }
    break;

  case WM_SIZE:
    if (wParam != SIZE_MINIMIZED && renderer_)
    {
      const int w = LOWORD(lParam);
      const int h = HIWORD(lParam);
      if (w > 0 && h > 0)
      {
        std::wstring err;
        renderer_->Resize(w, h, &err);
        if (cfg_.mode == WindowMode::Windowed)
        {
          windowedW_ = w;
          windowedH_ = h;
          cfg_.width = w;
          cfg_.height = h;
        }
      }
    }
    return 0;

  case WM_TIMER:
    // Wake message loop; Frame() does the work.
    return 0;

  case WM_ERASEBKGND:
    return 1;

  case WM_PAINT: {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_CLOSE:
    KillTimer(hwnd, kTimerId);
    running_ = false;
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    hwnd_ = nullptr;
    PostQuitMessage(0);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}
