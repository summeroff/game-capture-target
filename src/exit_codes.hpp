#pragma once

// Distinct process exit codes for harnesses (addendum 4).
enum class FgExit : int
{
  Ok = 0,
  GeneralFailure = 1,
  BadArgs = 2,
  WindowCreateFailed = 3,
  RendererInitFailed = 4,
  BlockUnverified = 5,
};
