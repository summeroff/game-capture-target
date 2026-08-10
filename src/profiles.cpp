#include "profiles.hpp"

#include "log.hpp"

#include <cstdio>
#include <sstream>

namespace
{

// Drawn from obs-studio/plugins/win-capture/data/compatibility.json
const Profile kProfiles[] = {
    {"cs2", "Counter-Strike 2", "cs2.exe", L"FakeGameWindowClass", L"Counter-Strike 2", 1, 1, true,
     false, true, BlockCaptureMode::None, "exe match; Warning + allow_third_party_software msg"},

    {"cs2-blocked", "CS2 (capture blocked)", "cs2.exe", L"FakeGameWindowClass", L"Counter-Strike 2",
     1, 1, true, false, false, BlockCaptureMode::SignaturePolicy,
     "same as cs2 + signature-policy (verified refuse; SetErrorMode silences Bad Image). "
     "squat-ipc remains available via --block-capture squat-ipc (F7 reversible)"},

    {"minecraft", "Minecraft: Java Edition", "javaw.exe", L"FakeGameWindowClass", L"Minecraft", 3,
     0, true, false, true, BlockCaptureMode::None,
     "exe+title prefix (try --title \"Minecraft 1.21\")"},

    {"wuthering", "Wuthering Waves", "Client-Win64-Shipping.exe", L"FakeGameWindowClass",
     L"Wuthering Waves", 3, 1, true, false, true, BlockCaptureMode::None,
     "exe+title; admin warning"},

    {"destiny2", "Destiny 2", "destiny2.exe", L"FakeGameWindowClass", L"Destiny 2", 1, 2, true,
     false, true, BlockCaptureMode::None, "exe; Error severity"},

    {"gta-sa", "GTA San Andreas", "gta-sa.exe", L"FakeGameWindowClass", L"GTA San Andreas", 1, 2,
     true, false, true, BlockCaptureMode::None, "exe; Error; no URL in message"},

    {"chromium-gc", "Chromium (Game Capture)", "fakegame.exe", L"Chrome_WidgetWin_1",
     L"Chromium GC", 4, 2, true, false, true, BlockCaptureMode::None,
     "class-only Game Capture block"},

    {"gaming-services", "Gaming Services (GC)", "fakegame.exe",
     L"GAMINGSERVICESUI_HOSTING_WINDOW_CLASS", L"Gaming Services", 4, 2, true, false, true,
     BlockCaptureMode::None, "class-only Game Capture block"},

    {"terraria", "Terraria", "terraria.exe", L"FakeGameWindowClass", L"Terraria", 1, 0, true, false,
     true, BlockCaptureMode::None, "exe; Normal (wrong-GPU message)"},

    {"roblox", "Roblox", "RobloxPlayerBeta.exe", L"FakeGameWindowClass", L"Roblox", 1, 1, true,
     true, true, BlockCaptureMode::None, "exe; both game + window capture entries"},

    {"steam", "Steam (Window Capture)", "fakegame.exe", L"SDL_app", L"Steam", 4, 0, false, true,
     true, BlockCaptureMode::None, "class-only; Window Capture BitBlt warning"},

    {"excel", "Microsoft Excel (Window Capture)", "fakegame.exe", L"XLMAIN", L"Microsoft Excel", 4,
     0, false, true, true, BlockCaptureMode::None, "class-only; Window Capture BitBlt warning"},
};

std::string JsonEscape(const char* s)
{
  std::string o;
  if (!s)
    return o;
  for (const char* p = s; *p; ++p)
  {
    const unsigned char c = static_cast<unsigned char>(*p);
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

std::string NarrowW(const wchar_t* w)
{
  if (!w)
    return {};
  return Narrow(w);
}

const char* SevName(int s)
{
  switch (s)
  {
  case 0:
    return "Normal";
  case 1:
    return "Warning";
  case 2:
    return "Error";
  default:
    return "?";
  }
}

const char* MatchName(int f)
{
  switch (f)
  {
  case 1:
    return "exe";
  case 2:
    return "title";
  case 3:
    return "exe+title";
  case 4:
    return "class";
  case 5:
    return "exe+class";
  case 6:
    return "title+class";
  case 7:
    return "exe+title+class";
  default:
    return "?";
  }
}

} // namespace

const std::vector<Profile>& GetProfiles()
{
  static std::vector<Profile> v(std::begin(kProfiles), std::end(kProfiles));
  return v;
}

const Profile* FindProfile(const std::wstring& id)
{
  for (const auto& p : GetProfiles())
  {
    // compare id case-insensitive
    const std::wstring wid = std::wstring(p.id, p.id + strlen(p.id));
    if (_wcsicmp(wid.c_str(), id.c_str()) == 0)
      return &p;
  }
  return nullptr;
}

void ApplyProfile(const Profile& p, Config* cfg)
{
  cfg->profileId = p.id;
  if (p.windowTitle && *p.windowTitle)
    cfg->title = p.windowTitle;
  if (p.windowClass && *p.windowClass)
    cfg->windowClass = p.windowClass;
  if (p.exeName && *p.exeName)
    cfg->profileExeName = p.exeName;
  if (p.windowClass && *p.windowClass)
    cfg->profileExpectedClass = p.windowClass;
  if (p.windowTitle && *p.windowTitle)
    cfg->profileExpectedTitle = p.windowTitle;
  cfg->blockCapture = p.defaultBlock;
  cfg->captureExpected = p.captureExpected;
  cfg->profileSeverity = p.severity;
  cfg->profileMatchFlags = p.matchFlags;
  cfg->profileGameCapture = p.gameCapture;
  cfg->profileWindowCapture = p.windowCapture;
}

std::string ProfilesToJson(bool pretty)
{
  const char* nl = pretty ? "\n" : "";
  const char* sp = pretty ? "  " : "";
  const char* sp2 = pretty ? "    " : "";
  std::ostringstream o;
  o << "[" << nl;
  const auto& list = GetProfiles();
  for (size_t i = 0; i < list.size(); ++i)
  {
    const auto& p = list[i];
    o << sp << "{" << nl;
    o << sp2 << "\"id\":\"" << JsonEscape(p.id) << "\"," << nl;
    o << sp2 << "\"displayName\":\"" << JsonEscape(p.displayName) << "\"," << nl;
    o << sp2 << "\"exe\":\"" << JsonEscape(p.exeName ? p.exeName : "") << "\"," << nl;
    o << sp2 << "\"windowClass\":\"" << JsonEscape(NarrowW(p.windowClass).c_str()) << "\"," << nl;
    o << sp2 << "\"windowTitle\":\"" << JsonEscape(NarrowW(p.windowTitle).c_str()) << "\"," << nl;
    o << sp2 << "\"matchFlags\":" << p.matchFlags << "," << nl;
    o << sp2 << "\"match\":\"" << MatchName(p.matchFlags) << "\"," << nl;
    o << sp2 << "\"severity\":" << p.severity << "," << nl;
    o << sp2 << "\"severityName\":\"" << SevName(p.severity) << "\"," << nl;
    o << sp2 << "\"gameCapture\":" << (p.gameCapture ? "true" : "false") << "," << nl;
    o << sp2 << "\"windowCapture\":" << (p.windowCapture ? "true" : "false") << "," << nl;
    o << sp2 << "\"captureExpected\":" << (p.captureExpected ? "true" : "false") << "," << nl;
    o << sp2 << "\"defaultBlock\":\"" << Narrow(BlockCaptureModeName(p.defaultBlock)) << "\","
      << nl;
    // Default client size before launch overrides (harness pre-size).
    o << sp2 << "\"clientWidth\":1280," << nl;
    o << sp2 << "\"clientHeight\":720," << nl;
    o << sp2 << "\"notes\":\"" << JsonEscape(p.notes ? p.notes : "") << "\"" << nl;
    o << sp << "}" << (i + 1 < list.size() ? "," : "") << nl;
  }
  o << "]" << nl;
  return o.str();
}

void PrintProfilesTable()
{
  std::printf("%-16s %-10s %-8s %-8s %s\n", "Profile", "Match", "Severity", "Capture", "Notes");
  std::printf("%-16s %-10s %-8s %-8s %s\n", "-------", "-----", "--------", "-------", "-----");
  for (const auto& p : GetProfiles())
  {
    std::printf("%-16s %-10s %-8s %-8s %s\n", p.id, MatchName(p.matchFlags), SevName(p.severity),
                p.captureExpected ? "yes" : "NO", p.notes ? p.notes : "");
  }
}
