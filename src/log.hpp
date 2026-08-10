#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

#include <Windows.h>

inline std::string Narrow(const std::wstring& w)
{
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                      nullptr);
  return s;
}

// When true (events json mode), human-readable Log lines go to stderr so stdout stays NDJSON.
inline bool& LogToStderrFlag()
{
  static bool v = false;
  return v;
}

inline void SetLogToStderr(bool v)
{
  LogToStderrFlag() = v;
}

inline void Log(const char* fmt, ...)
{
  FILE* out = LogToStderrFlag() ? stderr : stdout;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  std::fprintf(out, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(out, fmt, ap);
  va_end(ap);

  std::fputc('\n', out);
  std::fflush(out);
}
