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

  // Suppress Windows hard-error boxes for failed DLL loads (e.g. signature-policy
  // rejecting graphics-hook*.dll with STATUS_INVALID_IMAGE_HASH / 0xc0000428).
  // Must be process-wide SetErrorMode — the failing load runs on OBS's inject thread.
  // Harmless when no blocking is configured; set at startup so --block-capture-after
  // still covers early inject attempts.
  if (!cfg.showBlockErrors) {
    const UINT prev = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    (void)prev;
    Log("error-mode: hard-error dialogs suppressed "
        "(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX|SEM_NOGPFAULTERRORBOX); "
        "use --show-block-errors to restore");
  } else {
    Log("error-mode: --show-block-errors set — Windows loader dialogs may appear");
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
