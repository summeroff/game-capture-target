#pragma once

#include <string>

#include <Windows.h>

// Structured NDJSON events for test harnesses (--events json).
// When enabled, human Log() lines go to stderr; events go to stdout (one JSON object/line).

void EventsSetEnabled(bool enabled);
bool EventsEnabled();

// Optional path: each EmitEvent also appends one line (same JSON).
void EventsSetReadyFile(const std::wstring& path);
void EventsClearReadyFile();

// ISO-ish local timestamp for "ts" fields.
std::string EventsTimestamp();

// Emit one NDJSON object line. `jsonObjectBody` is the inside of `{...}` without braces,
// or a full object starting with '{'. Prefer full object form.
void EmitEventJson(const std::string& jsonObject);

// JSON string escape (UTF-8).
std::string JsonEscapeUtf8(const std::string& s);

// OBS window property encoding: title:class:exe with # → #22, : → #3A
// (ms_build_window_strings / window-helpers.c).
std::string EncodeObsWindowPart(const std::string& s);
std::string BuildObsWindowSetting(const std::string& title, const std::string& windowClass,
                                  const std::string& exeName);

// Bare exe filename of this process (e.g. cs2.exe).
std::string CurrentExeBaseName();

// HWND as 0x%08X (low 32 bits) for stable harness strings; 64-bit full if needed.
std::string FormatHwnd(HWND hwnd);
