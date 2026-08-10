#pragma once

#include <string>
#include <vector>

#include <Windows.h>

enum class BlockCaptureMode
{
  None,
  SignaturePolicy,
  SquatIpc,
  UnloadHook,
};

const wchar_t* BlockCaptureModeName(BlockCaptureMode m);
bool ParseBlockCaptureMode(const std::wstring& v, BlockCaptureMode* out);

// Process-lifetime MicrosoftSignedOnly mitigation. Irreversible.
// Call ONLY after D3D/device + shaders + swapchain are up.
bool ApplySignaturePolicy(std::wstring* error);
bool VerifySignaturePolicy();

// Block OBS graphics-hook init:
//  1) Hold graphics_hook_dup_mutex+pid so hook DllMain takes the "duplicate" early-out
//     (this is the reliable refuse path — empty-DACL CaptureHook alone was insufficient
//     under Streamlabs Desktop on v0.1.0).
//  2) Empty-DACL CaptureHook_*+pid objects as defense-in-depth.
// Reversible via ReleaseSquatIpc().
bool ApplySquatIpc(std::wstring* error);
void ReleaseSquatIpc();
bool IsSquatIpcActive();
bool VerifySquatIpc(std::wstring* detail);

// FreeLibrary any loaded graphics-hook*.dll. Call repeatedly while active.
// Returns true if a module was unloaded this call.
bool TryUnloadGraphicsHook();

struct HookModuleState
{
  bool present = false;
  std::string baseName; // e.g. graphics-hook64.dll
};

// Snapshot whether a graphics-hook*.dll is mapped into this process.
HookModuleState QueryHookModule();

// Poll module list; log transitions when logAttempts is true.
void PollHookModules(bool logAttempts);
