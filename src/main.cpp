#include "app.hpp"
#include "config.hpp"
#include "log.hpp"
#include "profiles.hpp"

#include <cstdio>
#include <string>

#include <Windows.h>

namespace {

void ShowStartupBanner(const Config& cfg)
{
  PrintConfig(cfg);
  Log("hotkeys: F1 mode | F2 resize | F3 swap | F4 device | F5 title | F6 churn | F7 block | Esc quit");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Config cfg;
  std::wstring error;
  if (!ParseConfig(argc, argv, &cfg, &error)) {
    std::fwprintf(stderr, L"error: %s\n\n", error.c_str());
    PrintHelp();
    return 2;
  }

  if (cfg.help) {
    PrintHelp();
    return 0;
  }

  if (cfg.listProfiles) {
    if (cfg.listProfilesJson) {
      const std::string json = ProfilesToJson(true);
      fwrite(json.data(), 1, json.size(), stdout);
    } else {
      PrintProfilesTable();
    }
    return 0;
  }

  ShowStartupBanner(cfg);

  App app(cfg);
  if (!app.Initialize(&error)) {
    std::fwprintf(stderr, L"init failed: %s\n", error.c_str());
    Log("init failed: %s", Narrow(error).c_str());
    return 1;
  }

  return app.Run();
}
