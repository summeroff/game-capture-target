#include "app.hpp"
#include "config.hpp"
#include "events.hpp"
#include "exit_codes.hpp"
#include "log.hpp"
#include "profiles.hpp"

#include <cstdio>
#include <string>

#include <Windows.h>

namespace
{

void ShowStartupBanner(const Config& cfg)
{
  PrintConfig(cfg);
  Log("hotkeys: F1 mode | F2 resize | F3 swap | F4 device | F5 title | F6 churn | F7 block | Esc "
      "quit");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  Config cfg;
  std::wstring error;
  if (!ParseConfig(argc, argv, &cfg, &error))
  {
    std::fwprintf(stderr, L"error: %s\n\n", error.c_str());
    PrintHelp();
    return static_cast<int>(FgExit::BadArgs);
  }

  if (cfg.help)
  {
    PrintHelp();
    return static_cast<int>(FgExit::Ok);
  }

  if (cfg.version)
  {
    PrintVersion();
    return static_cast<int>(FgExit::Ok);
  }

  if (cfg.listProfiles)
  {
    if (cfg.listProfilesJson)
    {
      const std::string json = ProfilesToJson(true);
      fwrite(json.data(), 1, json.size(), stdout);
    } else
    {
      PrintProfilesTable();
    }
    return static_cast<int>(FgExit::Ok);
  }

  if (cfg.listScenes)
  {
    scene::PrintSceneList();
    return static_cast<int>(FgExit::Ok);
  }

  // Enable NDJSON events before any further logging that harnesses might scrape.
  if (cfg.eventsJson)
    EventsSetEnabled(true);
  if (!cfg.readyFile.empty())
    EventsSetReadyFile(cfg.readyFile);

  if (!cfg.instanceId.empty())
  {
    const bool classOnly = (cfg.profileMatchFlags == 4) ||
                           ((cfg.profileMatchFlags & 4) != 0 && (cfg.profileMatchFlags & 3) == 0);
    if (classOnly)
    {
      Log("instance: title suffix => %s", Narrow(cfg.title).c_str());
      if ((cfg.profileMatchFlags & 2) != 0)
        Log("warn: instance suffix on title may affect title-prefix match; keep id short");
    } else
    {
      Log("instance: class suffix => %s", Narrow(cfg.windowClass).c_str());
      if ((cfg.profileMatchFlags & 4) != 0)
        Log("warn: instance changed class but profile also matches class — may break class match");
    }
  }

  // signature-policy rejects unsigned graphics-hook*.dll (0xc0000428). The failing
  // load runs on OBS's inject thread, so suppression must be process-wide SetErrorMode
  // at startup (covers --block-capture-after). Do not apply for other modes — it also
  // hides crash dialogs (SEM_NOGPFAULTERRORBOX).
  if (cfg.blockCapture == BlockCaptureMode::SignaturePolicy && !cfg.showBlockErrors)
  {
    const UINT prev =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    (void)prev;
    Log("error-mode: hard-error dialogs suppressed for signature-policy "
        "(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX|SEM_NOGPFAULTERRORBOX); "
        "use --show-block-errors to restore");
  } else if (cfg.showBlockErrors)
  {
    Log("error-mode: --show-block-errors set — Windows loader dialogs may appear");
  }

  ShowStartupBanner(cfg);

  App app(cfg);
  if (!app.Initialize(&error))
  {
    std::fwprintf(stderr, L"init failed: %s\n", error.c_str());
    Log("init failed: %s", Narrow(error).c_str());
    const FgExit code = app.LastExit();
    return static_cast<int>(code == FgExit::Ok ? FgExit::GeneralFailure : code);
  }

  return app.Run();
}
