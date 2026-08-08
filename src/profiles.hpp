#pragma once

#include "config.hpp"

#include <string>
#include <vector>

struct Profile
{
  const char* id = nullptr;
  const char* displayName = nullptr;
  const char* exeName = nullptr; // bare filename; empty/null => keep real exe name
  const wchar_t* windowClass = nullptr;
  const wchar_t* windowTitle = nullptr;
  int matchFlags = 1; // EXE=1 TITLE=2 CLASS=4
  int severity = 0;   // 0 Normal, 1 Warning, 2 Error
  bool gameCapture = true;
  bool windowCapture = false;
  bool captureExpected = true;
  BlockCaptureMode defaultBlock = BlockCaptureMode::None;
  const char* notes = nullptr;
};

const std::vector<Profile>& GetProfiles();
const Profile* FindProfile(const std::wstring& id);

// Apply profile defaults into cfg (does not override already-set explicit flags —
// caller passes a base cfg and then re-applies CLI overrides).
void ApplyProfile(const Profile& p, Config* cfg);

// JSON array of profile objects (UTF-8). Pretty=false => compact one line-ish.
std::string ProfilesToJson(bool pretty);

// Human table for --list-profiles (no json).
void PrintProfilesTable();
