<#
.SYNOPSIS
  Copy fakegame.exe under a different name and launch it (OBS match-by-exe testing).

.EXAMPLE
  .\tools\spawn-as.ps1 -As cs2.exe -GameArgs '--title "Counter-Strike 2"'

.EXAMPLE
  .\tools\spawn-as.ps1 -As destiny2.exe -GameArgs '--api d3d11 --width 1920 --height 1080'
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $As,

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
