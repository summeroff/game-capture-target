#pragma once

#include <string>

#include <Windows.h>

enum class BlockCaptureMode {
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

// Create OBS hook IPC objects with empty DACL so hook Create/Open fails.
// Reversible via ReleaseSquatIpc().
bool ApplySquatIpc(std::wstring* error);
void ReleaseSquatIpc();
bool IsSquatIpcActive();

// FreeLibrary any loaded graphics-hook*.dll. Call repeatedly while active.
// Returns true if a module was unloaded this call.
bool TryUnloadGraphicsHook();

// Poll module list; log when hook appears/disappears. Call every frame when interested.
void PollHookModules(bool logAttempts);
