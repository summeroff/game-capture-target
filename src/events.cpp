#include "events.hpp"

#include "log.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace
{

bool g_events = false;
std::wstring g_readyFile;
std::mutex g_mu;

} // namespace

void EventsSetEnabled(bool enabled)
{
  g_events = enabled;
  SetLogToStderr(enabled);
}

bool EventsEnabled()
{
  return g_events;
}

void EventsSetReadyFile(const std::wstring& path)
{
  g_readyFile = path;
}

void EventsClearReadyFile()
{
  g_readyFile.clear();
}

std::string EventsTimestamp()
{
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[64];
  sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

std::string JsonEscapeUtf8(const std::string& s)
{
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s)
  {
    if (c == '"' || c == '\\')
    {
      o.push_back('\\');
      o.push_back(static_cast<char>(c));
    } else if (c < 0x20)
    {
      char buf[8];
      sprintf_s(buf, "\\u%04x", c);
      o += buf;
    } else
    {
      o.push_back(static_cast<char>(c));
    }
  }
  return o;
}

std::string EncodeObsWindowPart(const std::string& s)
{
  std::string o;
  o.reserve(s.size() + 8);
  for (char ch : s)
  {
    if (ch == '#')
      o += "#22";
    else if (ch == ':')
      o += "#3A";
    else
      o.push_back(ch);
  }
  return o;
}

std::string BuildObsWindowSetting(const std::string& title, const std::string& windowClass,
                                  const std::string& exeName)
{
  return EncodeObsWindowPart(title) + ":" + EncodeObsWindowPart(windowClass) + ":" +
         EncodeObsWindowPart(exeName);
}

std::string CurrentExeBaseName()
{
  wchar_t path[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
    return "fakegame.exe";
  const wchar_t* base = wcsrchr(path, L'\\');
  base = base ? base + 1 : path;
  return Narrow(base);
}

std::string FormatHwnd(HWND hwnd)
{
  char buf[32];
  const unsigned long long v = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd));
  sprintf_s(buf, "0x%08llX", v);
  return buf;
}

void EmitEventJson(const std::string& jsonObject)
{
  if (!g_events && g_readyFile.empty())
    return;

  std::string line = jsonObject;
  if (line.empty())
    return;
  if (line.front() != '{')
    line = "{" + line + "}";
  // single line
  for (char& c : line)
  {
    if (c == '\n' || c == '\r')
      c = ' ';
  }

  std::lock_guard<std::mutex> lock(g_mu);
  if (g_events)
  {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  // ready-file: write only the ready event (full replace), and also append-all if name ends with
  // .ndjson — simple rule: if path set, write latest matching "ready" as the whole file content
  // when event is ready; always no-op for non-ready unless file ends with .ndjson (append).
  if (!g_readyFile.empty())
  {
    const bool isReady = line.find("\"event\":\"ready\"") != std::string::npos;
    const bool appendAll =
        g_readyFile.size() >= 7 &&
        _wcsicmp(g_readyFile.c_str() + (g_readyFile.size() - 7), L".ndjson") == 0;
    if (isReady || appendAll)
    {
      const std::ios::openmode mode =
          appendAll ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc);
      std::ofstream out(g_readyFile, mode);
      if (out)
      {
        out << line << '\n';
      } else
      {
        Log("events: failed to write ready-file");
      }
    }
  }
}
