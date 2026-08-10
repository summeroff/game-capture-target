#include "block_capture.hpp"

#include "log.hpp"

#include <cstring>
#include <vector>

#include <aclapi.h>
#include <psapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace
{

// OBS graphics-hook DllMain duplicate guard (graphics-hook.c HOOK_NAME).
const wchar_t* kDupMutexBase = L"graphics_hook_dup_mutex";

const wchar_t* kEventBases[] = {
    L"CaptureHook_Restart", L"CaptureHook_Stop",       L"CaptureHook_HookReady",
    L"CaptureHook_Exit",    L"CaptureHook_Initialize",
};

const wchar_t* kMutexBases[] = {
    L"CaptureHook_TextureMutex1",
    L"CaptureHook_TextureMutex2",
};

const wchar_t* kMapBases[] = {
    L"CaptureHook_HookInfo",
};

std::vector<HANDLE> g_squatHandles;
HANDLE g_dupMutex = nullptr;
bool g_squatActive = false;

// Empty DACL => deny all subsequent Open/Create by name (incl. injected hook, same process).
bool MakeDenyAllSa(SECURITY_ATTRIBUTES* sa, SECURITY_DESCRIPTOR* sd, PACL* aclOut)
{
  *aclOut = nullptr;
  if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
    return false;

  const DWORD aclSize = sizeof(ACL);
  PACL acl = static_cast<PACL>(LocalAlloc(LPTR, aclSize));
  if (!acl)
    return false;
  if (!InitializeAcl(acl, aclSize, ACL_REVISION))
  {
    LocalFree(acl);
    return false;
  }
  if (!SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE))
  {
    LocalFree(acl);
    return false;
  }

  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = sd;
  sa->bInheritHandle = FALSE;
  *aclOut = acl;
  return true;
}

void NameWithPid(wchar_t* out, size_t cch, const wchar_t* base)
{
  swprintf_s(out, cch, L"%s%lu", base, GetCurrentProcessId());
}

unsigned ModuleCountClamped(DWORD neededBytes, unsigned capacity)
{
  const unsigned n = neededBytes / sizeof(HMODULE);
  return n > capacity ? capacity : n;
}

// Open by name must fail with ACCESS_DENIED for empty-DACL squat objects.
bool ExpectOpenDenied(const wchar_t* name, HANDLE (*opener)(const wchar_t*), const char* kind,
                      std::wstring* detail)
{
  SetLastError(0);
  HANDLE h = opener(name);
  if (h)
  {
    CloseHandle(h);
    if (detail)
      *detail = std::wstring(L"Open allowed on ") + name;
    Log("block: squat-ipc verify FAIL %s Open allowed on %s", kind, Narrow(name).c_str());
    return false;
  }
  const DWORD err = GetLastError();
  // ACCESS_DENIED (5) is ideal; FILE_NOT_FOUND means object missing (also bad for squat).
  if (err != ERROR_ACCESS_DENIED)
  {
    if (detail)
      *detail = std::wstring(L"Open ") + name + L" err=" + std::to_wstring(err) +
                L" (want ACCESS_DENIED)";
    Log("block: squat-ipc verify FAIL %s Open err=%lu on %s", kind, err, Narrow(name).c_str());
    return false;
  }
  return true;
}

HANDLE OpenEventSync(const wchar_t* name)
{
  return OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
}

HANDLE OpenMutexSync(const wchar_t* name)
{
  return OpenMutexW(SYNCHRONIZE, FALSE, name);
}

HANDLE OpenMapRw(const wchar_t* name)
{
  return OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
}

bool ExpectCreateEventDenied(const wchar_t* name, std::wstring* detail)
{
  SetLastError(0);
  HANDLE h = CreateEventW(nullptr, FALSE, FALSE, name);
  const DWORD err = GetLastError();
  if (h)
  {
    CloseHandle(h);
    if (err == ERROR_ALREADY_EXISTS)
    {
      if (detail)
        *detail = std::wstring(L"CreateEvent allowed on ") + name;
      Log("block: squat-ipc verify FAIL CreateEvent allowed on %s", Narrow(name).c_str());
      return false;
    }
    // Created a new object - squat handle lost.
    if (detail)
      *detail = std::wstring(L"CreateEvent created new object for ") + name;
    Log("block: squat-ipc verify FAIL CreateEvent created new %s", Narrow(name).c_str());
    return false;
  }
  if (err != ERROR_ACCESS_DENIED)
  {
    if (detail)
      *detail = L"CreateEvent failed with unexpected error " + std::to_wstring(err);
    Log("block: squat-ipc verify FAIL CreateEvent err=%lu on %s", err, Narrow(name).c_str());
    return false;
  }
  return true;
}

} // namespace

const wchar_t* BlockCaptureModeName(BlockCaptureMode m)
{
  switch (m)
  {
  case BlockCaptureMode::None:
    return L"none";
  case BlockCaptureMode::SignaturePolicy:
    return L"signature-policy";
  case BlockCaptureMode::SquatIpc:
    return L"squat-ipc";
  case BlockCaptureMode::UnloadHook:
    return L"unload-hook";
  }
  return L"?";
}

bool ParseBlockCaptureMode(const std::wstring& v, BlockCaptureMode* out)
{
  if (_wcsicmp(v.c_str(), L"none") == 0)
  {
    *out = BlockCaptureMode::None;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"signature-policy") == 0 || _wcsicmp(v.c_str(), L"signature") == 0)
  {
    *out = BlockCaptureMode::SignaturePolicy;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"squat-ipc") == 0 || _wcsicmp(v.c_str(), L"squat") == 0)
  {
    *out = BlockCaptureMode::SquatIpc;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"unload-hook") == 0 || _wcsicmp(v.c_str(), L"unload") == 0)
  {
    *out = BlockCaptureMode::UnloadHook;
    return true;
  }
  return false;
}

bool ApplySignaturePolicy(std::wstring* error)
{
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
  policy.MicrosoftSignedOnly = 1;
  if (!SetProcessMitigationPolicy(ProcessSignaturePolicy, &policy, sizeof(policy)))
  {
    const DWORD err = GetLastError();
    *error = L"SetProcessMitigationPolicy(ProcessSignaturePolicy) failed, GetLastError=" +
             std::to_wstring(err);
    Log("block: signature-policy FAILED err=%lu", err);
    return false;
  }
  Log("block: signature-policy APPLIED (MicrosoftSignedOnly=1) - irreversible for this process");
  return true;
}

bool VerifySignaturePolicy()
{
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
  if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessSignaturePolicy, &policy,
                                  sizeof(policy)))
  {
    Log("block: signature-policy verify GetProcessMitigationPolicy failed err=%lu", GetLastError());
    return false;
  }
  const bool ok = policy.MicrosoftSignedOnly == 1;
  Log("block: signature-policy verify MicrosoftSignedOnly=%u => %s", policy.MicrosoftSignedOnly,
      ok ? "OK" : "FAIL");
  return ok;
}

bool ApplySquatIpc(std::wstring* error)
{
  if (g_squatActive)
    return true;

  std::vector<HANDLE> created;

  auto fail = [&](const wchar_t* what) -> bool {
    Log("block: squat-ipc create failed for %s err=%lu", Narrow(what).c_str(), GetLastError());
    for (HANDLE h : created)
      if (h)
        CloseHandle(h);
    if (g_dupMutex)
    {
      CloseHandle(g_dupMutex);
      g_dupMutex = nullptr;
    }
    *error = std::wstring(L"squat-ipc failed creating ") + what;
    return false;
  };

  wchar_t name[128];

  // (1) Duplicate-hook mutex - OBS graphics-hook DllMain:
  //     open_mutex(graphics_hook_dup_mutex+pid) succeeds -> refuse load as duplicate.
  NameWithPid(name, _countof(name), kDupMutexBase);
  g_dupMutex = CreateMutexW(nullptr, FALSE, name);
  if (!g_dupMutex)
    return fail(name);
  Log("block: squat-ipc dup-mutex %s (hook DllMain early-out)", Narrow(name).c_str());

  // (2) Empty-DACL CaptureHook_* objects - defense in depth.
  SECURITY_DESCRIPTOR sd{};
  SECURITY_ATTRIBUTES sa{};
  PACL acl = nullptr;
  if (!MakeDenyAllSa(&sa, &sd, &acl))
  {
    CloseHandle(g_dupMutex);
    g_dupMutex = nullptr;
    *error = L"failed to build deny-all SECURITY_ATTRIBUTES";
    return false;
  }

  for (const wchar_t* base : kEventBases)
  {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateEventW(&sa, FALSE, FALSE, name);
    if (!h)
    {
      if (acl)
        LocalFree(acl);
      return fail(name);
    }
    created.push_back(h);
    Log("block: squat-ipc event %s", Narrow(name).c_str());
  }
  for (const wchar_t* base : kMutexBases)
  {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateMutexW(&sa, FALSE, name);
    if (!h)
    {
      if (acl)
        LocalFree(acl);
      return fail(name);
    }
    created.push_back(h);
    Log("block: squat-ipc mutex %s", Narrow(name).c_str());
  }
  for (const wchar_t* base : kMapBases)
  {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, name);
    if (!h)
    {
      if (acl)
        LocalFree(acl);
      return fail(name);
    }
    created.push_back(h);
    Log("block: squat-ipc mapping %s", Narrow(name).c_str());
  }

  g_squatHandles = std::move(created);
  g_squatActive = true;
  if (acl)
    LocalFree(acl);
  Log("block: squat-ipc APPLIED (dup-mutex + empty DACL CaptureHook_*)");
  return true;
}

void ReleaseSquatIpc()
{
  if (!g_squatActive && !g_dupMutex)
    return;
  for (HANDLE h : g_squatHandles)
  {
    if (h)
      CloseHandle(h);
  }
  g_squatHandles.clear();
  if (g_dupMutex)
  {
    CloseHandle(g_dupMutex);
    g_dupMutex = nullptr;
  }
  g_squatActive = false;
  Log("block: squat-ipc RELEASED");
}

bool IsSquatIpcActive()
{
  return g_squatActive;
}

bool VerifySquatIpc(std::wstring* detail)
{
  if (!g_squatActive || !g_dupMutex)
  {
    if (detail)
      *detail = L"squat not active";
    return false;
  }

  wchar_t name[128];

  // Dup mutex must be openable (hook takes duplicate path).
  NameWithPid(name, _countof(name), kDupMutexBase);
  HANDLE hOpen = OpenMutexW(SYNCHRONIZE, FALSE, name);
  if (!hOpen)
  {
    if (detail)
      *detail = L"dup-mutex OpenMutex failed (hook would not see duplicate)";
    Log("block: squat-ipc verify FAIL dup-mutex open err=%lu", GetLastError());
    return false;
  }
  CloseHandle(hOpen);

  // Every CaptureHook_* object: Open denied + CreateEvent denied for events.
  for (const wchar_t* base : kEventBases)
  {
    NameWithPid(name, _countof(name), base);
    if (!ExpectCreateEventDenied(name, detail))
      return false;
    if (!ExpectOpenDenied(name, OpenEventSync, "event", detail))
      return false;
  }
  for (const wchar_t* base : kMutexBases)
  {
    NameWithPid(name, _countof(name), base);
    if (!ExpectOpenDenied(name, OpenMutexSync, "mutex", detail))
      return false;
  }
  for (const wchar_t* base : kMapBases)
  {
    NameWithPid(name, _countof(name), base);
    if (!ExpectOpenDenied(name, OpenMapRw, "mapping", detail))
      return false;
  }

  if (detail)
    *detail = L"dup-mutex held; all CaptureHook_* Open/Create denied";
  Log("block: squat-ipc verify OK (%s)", detail ? Narrow(*detail).c_str() : "ok");
  return true;
}

bool TryUnloadGraphicsHook()
{
  HMODULE mods[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    return false;

  const unsigned count = ModuleCountClamped(needed, _countof(mods));
  bool unloaded = false;
  for (unsigned i = 0; i < count; ++i)
  {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mods[i], path, MAX_PATH))
      continue;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsnicmp(base, L"graphics-hook", 13) == 0)
    {
      Log("block: unload-hook FreeLibrary %s", Narrow(base).c_str());
      if (FreeLibrary(mods[i]))
      {
        unloaded = true;
      } else
      {
        Log("block: FreeLibrary failed err=%lu", GetLastError());
      }
    }
  }
  return unloaded;
}

HookModuleState QueryHookModule()
{
  HookModuleState st;
  HMODULE mods[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    return st;

  const unsigned count = ModuleCountClamped(needed, _countof(mods));
  for (unsigned i = 0; i < count; ++i)
  {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mods[i], path, MAX_PATH))
      continue;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsnicmp(base, L"graphics-hook", 13) == 0)
    {
      st.present = true;
      st.baseName = Narrow(base);
      break;
    }
  }
  return st;
}

bool IsHookIpcReadyPresent()
{
  wchar_t name[128];
  NameWithPid(name, _countof(name), L"CaptureHook_HookReady");
  HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
  if (!h)
    return false;
  CloseHandle(h);
  return true;
}

void PollHookModules(bool logAttempts)
{
  static bool s_seen = false;
  const HookModuleState st = QueryHookModule();
  if (logAttempts && st.present && !s_seen)
    Log("block: observed hook module loaded: %s", st.baseName.c_str());
  if (logAttempts && s_seen && !st.present)
    Log("block: hook module no longer present");
  s_seen = st.present;
}
