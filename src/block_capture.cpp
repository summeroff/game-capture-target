#include "block_capture.hpp"

#include "log.hpp"

#include <cstring>
#include <vector>

#include <aclapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace {

const wchar_t* kEventBases[] = {
    L"CaptureHook_Restart", L"CaptureHook_Stop",     L"CaptureHook_HookReady",
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
bool g_squatActive = false;

// Empty DACL => deny all subsequent Open/Create by other callers (incl. injected hook).
bool MakeDenyAllSa(SECURITY_ATTRIBUTES* sa, SECURITY_DESCRIPTOR* sd, PACL* aclOut)
{
  *aclOut = nullptr;
  if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
    return false;

  //  sizeof(ACL) is enough for an empty ACL.
  const DWORD aclSize = sizeof(ACL);
  PACL acl = static_cast<PACL>(LocalAlloc(LPTR, aclSize));
  if (!acl)
    return false;
  if (!InitializeAcl(acl, aclSize, ACL_REVISION)) {
    LocalFree(acl);
    return false;
  }
  if (!SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)) {
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

} // namespace

const wchar_t* BlockCaptureModeName(BlockCaptureMode m)
{
  switch (m) {
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
  if (_wcsicmp(v.c_str(), L"none") == 0) {
    *out = BlockCaptureMode::None;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"signature-policy") == 0 || _wcsicmp(v.c_str(), L"signature") == 0) {
    *out = BlockCaptureMode::SignaturePolicy;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"squat-ipc") == 0 || _wcsicmp(v.c_str(), L"squat") == 0) {
    *out = BlockCaptureMode::SquatIpc;
    return true;
  }
  if (_wcsicmp(v.c_str(), L"unload-hook") == 0 || _wcsicmp(v.c_str(), L"unload") == 0) {
    *out = BlockCaptureMode::UnloadHook;
    return true;
  }
  return false;
}

bool ApplySignaturePolicy(std::wstring* error)
{
  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy{};
  policy.MicrosoftSignedOnly = 1;
  if (!SetProcessMitigationPolicy(ProcessSignaturePolicy, &policy, sizeof(policy))) {
    const DWORD err = GetLastError();
    *error = L"SetProcessMitigationPolicy(ProcessSignaturePolicy) failed, GetLastError=" +
             std::to_wstring(err);
    Log("block: signature-policy FAILED err=%lu", err);
    return false;
  }
  Log("block: signature-policy APPLIED (MicrosoftSignedOnly=1) — irreversible for this process");
  return true;
}

bool ApplySquatIpc(std::wstring* error)
{
  if (g_squatActive)
    return true;

  SECURITY_DESCRIPTOR sd{};
  SECURITY_ATTRIBUTES sa{};
  PACL acl = nullptr;
  if (!MakeDenyAllSa(&sa, &sd, &acl)) {
    *error = L"failed to build deny-all SECURITY_ATTRIBUTES";
    return false;
  }

  std::vector<HANDLE> created;
  auto fail = [&](const wchar_t* what) -> bool {
    Log("block: squat-ipc create failed for %s err=%lu", Narrow(what).c_str(), GetLastError());
    for (HANDLE h : created)
      if (h)
        CloseHandle(h);
    if (acl)
      LocalFree(acl);
    *error = std::wstring(L"squat-ipc failed creating ") + what;
    return false;
  };

  wchar_t name[128];
  for (const wchar_t* base : kEventBases) {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateEventW(&sa, FALSE, FALSE, name);
    if (!h)
      return fail(name);
    created.push_back(h);
    Log("block: squat-ipc event %s", Narrow(name).c_str());
  }
  for (const wchar_t* base : kMutexBases) {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateMutexW(&sa, FALSE, name);
    if (!h)
      return fail(name);
    created.push_back(h);
    Log("block: squat-ipc mutex %s", Narrow(name).c_str());
  }
  for (const wchar_t* base : kMapBases) {
    NameWithPid(name, _countof(name), base);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, name);
    if (!h)
      return fail(name);
    created.push_back(h);
    Log("block: squat-ipc mapping %s", Narrow(name).c_str());
  }

  g_squatHandles = std::move(created);
  g_squatActive = true;
  if (acl)
    LocalFree(acl);
  Log("block: squat-ipc APPLIED (empty DACL on hook IPC objects)");
  return true;
}

void ReleaseSquatIpc()
{
  if (!g_squatActive)
    return;
  for (HANDLE h : g_squatHandles) {
    if (h)
      CloseHandle(h);
  }
  g_squatHandles.clear();
  g_squatActive = false;
  Log("block: squat-ipc RELEASED");
}

bool IsSquatIpcActive()
{
  return g_squatActive;
}

bool TryUnloadGraphicsHook()
{
  HMODULE mods[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    return false;

  const unsigned count = needed / sizeof(HMODULE);
  bool unloaded = false;
  for (unsigned i = 0; i < count; ++i) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mods[i], path, MAX_PATH))
      continue;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsnicmp(base, L"graphics-hook", 13) == 0) {
      Log("block: unload-hook FreeLibrary %s", Narrow(base).c_str());
      if (FreeLibrary(mods[i])) {
        unloaded = true;
      } else {
        Log("block: FreeLibrary failed err=%lu", GetLastError());
      }
    }
  }
  return unloaded;
}

void PollHookModules(bool logAttempts)
{
  static bool s_seen = false;
  HMODULE mods[512];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    return;

  bool present = false;
  const unsigned count = needed / sizeof(HMODULE);
  for (unsigned i = 0; i < count; ++i) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(mods[i], path, MAX_PATH))
      continue;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsnicmp(base, L"graphics-hook", 13) == 0) {
      present = true;
      if (logAttempts && !s_seen)
        Log("block: observed hook module loaded: %s", Narrow(base).c_str());
      break;
    }
  }
  if (logAttempts && s_seen && !present)
    Log("block: hook module no longer present");
  s_seen = present;
}
