<#
.SYNOPSIS
  Copy fakegame.exe under a different name and launch it (OBS match-by-exe testing).

.EXAMPLE
  .\tools\spawn-as.ps1 -As cs2.exe -GameArgs '--title "Counter-Strike 2"'

.EXAMPLE
  .\tools\spawn-as.ps1 -Profile cs2-blocked

.EXAMPLE
  .\tools\spawn-as.ps1 -As destiny2.exe -GameArgs '--api d3d11 --width 1920 --height 1080'
#>
[CmdletBinding()]
param(
  [Parameter()]
  [string] $As,

  [Parameter()]
  [string] $Profile,

  [Parameter()]
  [Alias('Args')]
  [string] $GameArgs = "",

  [Parameter()]
  [string] $Source,

  [Parameter()]
  [string] $OutDir
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Source) {
  $candidates = @(
    (Join-Path $repoRoot "build\bin\fakegame.exe"),
    (Join-Path $repoRoot "build\bin\Release\fakegame.exe"),
    (Join-Path $repoRoot "build-x86\bin\fakegame.exe"),
    (Join-Path $repoRoot "build\Release\fakegame.exe")
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c) {
      $Source = $c
      break
    }
  }
}

if (-not $Source -or -not (Test-Path -LiteralPath $Source)) {
  throw "fakegame.exe not found. Build first (scripts\build.bat) or pass -Source."
}

if ($Profile) {
  # Prefer tools/launch.ps1 for profile-aware QA (waits for window, JSON, etc.).
  # This path still renames by profile exe and forwards --profile.
  $lines = & $Source --list-profiles --json 2>&1
  if ($LASTEXITCODE -ne 0) { throw "fakegame --list-profiles --json failed: $lines" }
  $text = ($lines | ForEach-Object { "$_" }) -join "`n"
  $profiles = @($text.Trim() | ConvertFrom-Json)
  $p = $profiles | Where-Object { $_.id -eq $Profile } | Select-Object -First 1
  if (-not $p) { throw "unknown profile '$Profile'" }
  if (-not $As) { $As = [string]$p.exe }
  if (-not $GameArgs) {
    $GameArgs = "--profile $Profile"
  } elseif ($GameArgs -notmatch '--profile\b') {
    $GameArgs = "--profile $Profile $GameArgs"
  }
  if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot "tools\_spawn\$Profile"
  }
}

if (-not $As) {
  throw "Pass -As <exeName> or -Profile <name>."
}

if (-not $OutDir) {
  $OutDir = Join-Path $repoRoot "tools\_spawn"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Bare filename only; strip any path the caller accidentally passed.
$targetName = [System.IO.Path]::GetFileName($As)
if (-not $targetName.EndsWith(".exe", [System.StringComparison]::OrdinalIgnoreCase)) {
  $targetName = "$targetName.exe"
}

$dest = Join-Path $OutDir $targetName
Copy-Item -LiteralPath $Source -Destination $dest -Force

Write-Host "spawn: $dest"
if ($GameArgs) {
  Write-Host "args : $GameArgs"
}

Start-Process -FilePath $dest -ArgumentList $GameArgs -WorkingDirectory $OutDir
