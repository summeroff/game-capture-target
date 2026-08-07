#include "config.hpp"

#include "log.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace {

std::wstring Trim(const std::wstring& s)
{
  size_t b = 0;
  while (b < s.size() && iswspace(s[b]))
    ++b;
  size_t e = s.size();
  while (e > b && iswspace(s[e - 1]))
    --e;
  return s.substr(b, e - b);
}

bool EqI(const std::wstring& a, const wchar_t* b)
{
  return _wcsicmp(a.c_str(), b) == 0;
}

bool ParseBool(const std::wstring& v, bool* out)
{
  if (EqI(v, L"1") || EqI(v, L"true") || EqI(v, L"yes") || EqI(v, L"on")) {
    *out = true;
    return true;
  }
  if (EqI(v, L"0") || EqI(v, L"false") || EqI(v, L"no") || EqI(v, L"off")) {
    *out = false;
    return true;
  }
  return false;
}

bool ParseInt(const std::wstring& v, int* out)
{
  wchar_t* end = nullptr;
  const long n = wcstol(v.c_str(), &end, 10);
  if (end == v.c_str() || (end && *end != L'\0'))
    return false;
  *out = static_cast<int>(n);
  return true;
}

bool ParseMode(const std::wstring& v, WindowMode* out)
{
  if (EqI(v, L"windowed")) {
    *out = WindowMode::Windowed;
    return true;
  }
  if (EqI(v, L"borderless")) {
    *out = WindowMode::Borderless;
    return true;
  }
  if (EqI(v, L"fullscreen-exclusive") || EqI(v, L"fullscreen") || EqI(v, L"exclusive")) {
    *out = WindowMode::FullscreenExclusive;
    return true;
  }
  return false;
}

bool ParseApi(const std::wstring& v, GraphicsApi* out)
{
  if (EqI(v, L"d3d11") || EqI(v, L"dx11")) {
    *out = GraphicsApi::D3D11;
    return true;
  }
  if (EqI(v, L"d3d12") || EqI(v, L"dx12")) {
    *out = GraphicsApi::D3D12;
    return true;
  }
  if (EqI(v, L"opengl") || EqI(v, L"gl")) {
    *out = GraphicsApi::OpenGL;
    return true;
  }
  if (EqI(v, L"none") || EqI(v, L"gdi")) {
    *out = GraphicsApi::None;
    return true;
  }
  return false;
}

bool ApplyKeyValue(Config* c, const std::wstring& key, const std::wstring& value, std::wstring* error)
{
  if (EqI(key, L"title")) {
    c->title = value;
    return true;
  }
  if (EqI(key, L"class") || EqI(key, L"window_class")) {
    c->windowClass = value;
    return true;
  }
  if (EqI(key, L"width")) {
    if (!ParseInt(value, &c->width) || c->width < 64) {
      *error = L"invalid width";
      return false;
    }
    return true;
  }
  if (EqI(key, L"height")) {
    if (!ParseInt(value, &c->height) || c->height < 64) {
      *error = L"invalid height";
      return false;
    }
    return true;
  }
  if (EqI(key, L"mode")) {
    if (!ParseMode(value, &c->mode)) {
      *error = L"invalid mode (windowed|borderless|fullscreen-exclusive)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"api")) {
    if (!ParseApi(value, &c->api)) {
      *error = L"invalid api (d3d11|d3d12|opengl|none)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"fps")) {
    if (!ParseInt(value, &c->fps) || c->fps < 1 || c->fps > 1000) {
      *error = L"invalid fps";
      return false;
    }
    return true;
  }
  if (EqI(key, L"vsync")) {
    if (!ParseBool(value, &c->vsync)) {
      *error = L"invalid vsync";
      return false;
    }
    return true;
  }
  if (EqI(key, L"flip-model") || EqI(key, L"flip_model")) {
    if (!ParseBool(value, &c->flipModel)) {
      *error = L"invalid flip-model";
      return false;
    }
    return true;
  }
  if (EqI(key, L"buffers")) {
    if (!ParseInt(value, &c->buffers) || c->buffers < 2 || c->buffers > 8) {
      *error = L"invalid buffers (2-8)";
      return false;
    }
    return true;
  }
  if (EqI(key, L"exit-after") || EqI(key, L"exit_after")) {
    if (!ParseInt(value, &c->exitAfterSeconds) || c->exitAfterSeconds < 0) {
      *error = L"invalid exit-after";
      return false;
    }
    return true;
  }
  if (EqI(key, L"topmost")) {
    if (!ParseBool(value, &c->topmost)) {
      *error = L"invalid topmost";
      return false;
    }
    return true;
  }
  if (EqI(key, L"no-hud") || EqI(key, L"no_hud")) {
    if (!ParseBool(value, &c->noHud)) {
      *error = L"invalid no-hud";
      return false;
    }
    return true;
  }

  *error = L"unknown config key: " + key;
  return false;
}

bool LoadIni(const std::wstring& path, Config* c, std::wstring* error)
{
  // UTF-8 or ANSI file; convert line-by-line.
  std::ifstream in(path);
  if (!in) {
    *error = L"cannot open config file: " + path;
    return false;
  }

  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    // strip UTF-8 BOM on first line
    if (lineNo == 1 && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }

    // comments
    const auto hash = line.find(';');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    const auto hash2 = line.find('#');
    if (hash2 != std::string::npos)
      line = line.substr(0, hash2);

    // trim
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
      ++b;
    line = line.substr(b);
    if (line.empty())
      continue;
    if (line.front() == '[')
      continue; // section headers ignored

    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    std::string k = line.substr(0, eq);
    std::string v = line.substr(eq + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
      k.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
      v.erase(v.begin());

    // strip optional quotes
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
      v = v.substr(1, v.size() - 2);

    const int kn = MultiByteToWideChar(CP_UTF8, 0, k.c_str(), -1, nullptr, 0);
    const int vn = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, nullptr, 0);
    std::wstring wk(static_cast<size_t>(kn > 0 ? kn - 1 : 0), L'\0');
    std::wstring wv(static_cast<size_t>(vn > 0 ? vn - 1 : 0), L'\0');
    if (kn > 1)
      MultiByteToWideChar(CP_UTF8, 0, k.c_str(), -1, wk.data(), kn);
    if (vn > 1)
      MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, wv.data(), vn);

    std::wstring err;
    if (!ApplyKeyValue(c, wk, wv, &err)) {
      *error = L"config line " + std::to_wstring(lineNo) + L": " + err;
      return false;
    }
  }
  return true;
}

bool ConsumeValue(int argc, wchar_t** argv, int* i, std::wstring* out, std::wstring* error, const wchar_t* flag)
{
  if (*i + 1 >= argc) {
    *error = std::wstring(L"missing value for ") + flag;
    return false;
  }
  ++(*i);
  *out = argv[*i];
  return true;
}

} // namespace

const wchar_t* WindowModeName(WindowMode m)
{
  switch (m) {
  case WindowMode::Windowed:
    return L"windowed";
  case WindowMode::Borderless:
    return L"borderless";
  case WindowMode::FullscreenExclusive:
    return L"fullscreen-exclusive";
  }
  return L"?";
}

const wchar_t* GraphicsApiName(GraphicsApi a)
{
  switch (a) {
  case GraphicsApi::D3D11:
    return L"d3d11";
  case GraphicsApi::D3D12:
    return L"d3d12";
  case GraphicsApi::OpenGL:
    return L"opengl";
  case GraphicsApi::None:
    return L"none";
  }
  return L"?";
}

WindowMode NextWindowMode(WindowMode m)
{
  switch (m) {
  case WindowMode::Windowed:
    return WindowMode::Borderless;
  case WindowMode::Borderless:
    return WindowMode::FullscreenExclusive;
  case WindowMode::FullscreenExclusive:
    return WindowMode::Windowed;
  }
  return WindowMode::Windowed;
}

void PrintConfig(const Config& c)
{
  Log("=== resolved config ===");
  Log("  title       = %s", Narrow(c.title).c_str());
  Log("  class       = %s", Narrow(c.windowClass).c_str());
  Log("  size        = %dx%d", c.width, c.height);
  Log("  mode        = %s", Narrow(WindowModeName(c.mode)).c_str());
  Log("  api         = %s", Narrow(GraphicsApiName(c.api)).c_str());
  Log("  fps         = %d", c.fps);
  Log("  vsync       = %d", c.vsync ? 1 : 0);
  Log("  flip-model  = %d", c.flipModel ? 1 : 0);
  Log("  buffers     = %d", c.buffers);
  Log("  exit-after  = %d", c.exitAfterSeconds);
  Log("  topmost     = %d", c.topmost ? 1 : 0);
  Log("  no-hud      = %d", c.noHud ? 1 : 0);
  Log("=======================");
}

bool ParseConfig(int argc, wchar_t** argv, Config* out, std::wstring* error)
{
  Config c;

  // First pass: find --config so file loads before flags.
  std::wstring configPath;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"--config") == 0) {
      if (i + 1 >= argc) {
        *error = L"missing value for --config";
        return false;
      }
      configPath = argv[i + 1];
      break;
    }
  }
  if (!configPath.empty()) {
    if (!LoadIni(configPath, &c, error))
      return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];

    if (EqI(a, L"--config")) {
      ++i; // value already applied
      continue;
    }
    if (EqI(a, L"--title")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--title"))
        return false;
      c.title = v;
      continue;
    }
    if (EqI(a, L"--class")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--class"))
        return false;
      c.windowClass = v;
      continue;
    }
    if (EqI(a, L"--width")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--width"))
        return false;
      if (!ApplyKeyValue(&c, L"width", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--height")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--height"))
        return false;
      if (!ApplyKeyValue(&c, L"height", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--mode")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--mode"))
        return false;
      if (!ApplyKeyValue(&c, L"mode", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--api")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--api"))
        return false;
      if (!ApplyKeyValue(&c, L"api", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--fps")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--fps"))
        return false;
      if (!ApplyKeyValue(&c, L"fps", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--vsync")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--vsync"))
        return false;
      if (!ApplyKeyValue(&c, L"vsync", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--flip-model")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--flip-model"))
        return false;
      if (!ApplyKeyValue(&c, L"flip-model", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--buffers")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--buffers"))
        return false;
      if (!ApplyKeyValue(&c, L"buffers", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--exit-after")) {
      std::wstring v;
      if (!ConsumeValue(argc, argv, &i, &v, error, L"--exit-after"))
        return false;
      if (!ApplyKeyValue(&c, L"exit-after", v, error))
        return false;
      continue;
    }
    if (EqI(a, L"--topmost")) {
      c.topmost = true;
      continue;
    }
    if (EqI(a, L"--no-hud")) {
      c.noHud = true;
      continue;
    }
    if (EqI(a, L"--help") || EqI(a, L"-h") || EqI(a, L"/?")) {
      *error = L"help";
      return false;
    }

    *error = L"unknown argument: " + a;
    return false;
  }

  if (c.api == GraphicsApi::D3D12 || c.api == GraphicsApi::OpenGL) {
    *error = std::wstring(L"api '") + GraphicsApiName(c.api) +
             L"' is a stretch goal and not implemented yet; use d3d11 or none";
    return false;
  }

  *out = std::move(c);
  return true;
}
