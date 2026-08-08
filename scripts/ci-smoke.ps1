#Requires -Version 5.1
<#
.SYNOPSIS
  Headless smoke for CI: CLI surface + short --exit-after runs across a few APIs/modes.

.PARAMETER Exe
  Path to fakegame.exe (default: build\bin\fakegame.exe relative to repo root).

.PARAMETER Seconds
  --exit-after duration per interactive-ish run (default 2).
#>
[CmdletBinding()]
param(
  [string]$Exe = "",
  [int]$Seconds = 2
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
  $Exe = Join-Path $Root "build\bin\fakegame.exe"
}
if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "exe not found: $Exe"
  exit 2
}

function Invoke-Smoke {
  param(
    [Parameter(Mandatory = $true)][string]$Label,
    [Parameter(Mandatory = $true)][string[]]$ArgList,
    [int]$ExpectExit = 0,
    [switch]$Optional
  )
  Write-Host ""
  Write-Host "=== $Label ===" -ForegroundColor Cyan
  Write-Host ("> " + $Exe + " " + ($ArgList -join " "))
  # Avoid -ArgumentList $null / empty: Start-Process rejects null elements.
  $p = Start-Process -FilePath $Exe -ArgumentList $ArgList -NoNewWindow -PassThru -Wait
  $code = $p.ExitCode
  if ($null -eq $code) { $code = -1 }
  Write-Host "exit=$code (expect $ExpectExit)"
  if ($code -ne $ExpectExit) {
    if ($Optional) {
      Write-Host "OPTIONAL FAIL (ignored): $Label" -ForegroundColor Yellow
      return $false
    }
    Write-Error "SMOKE FAIL: $Label exited $code, expected $ExpectExit"
    exit 1
  }
  return $true
}

# Pure CLI (no window loop)
Invoke-Smoke -Label "help" -ArgList @("--help") | Out-Null
Invoke-Smoke -Label "list-profiles" -ArgList @("--list-profiles") | Out-Null

# JSON must be parseable (profiles script contracts on this)
Write-Host ""
Write-Host "=== list-profiles --json ===" -ForegroundColor Cyan
$jsonOut = & $Exe --list-profiles --json
if ($LASTEXITCODE -ne 0) {
  Write-Error "list-profiles --json exit=$LASTEXITCODE"
  exit 1
}
$joined = if ($jsonOut -is [array]) { $jsonOut -join "`n" } else { [string]$jsonOut }
try {
  $null = $joined | ConvertFrom-Json
  Write-Host "JSON OK ($($joined.Length) chars)"
} catch {
  Write-Error "list-profiles --json is not valid JSON: $_"
  exit 1
}

# Short live windows — d3d11 falls back to WARP if needed
Invoke-Smoke -Label "d3d11 default" -ArgList @(
  "--api", "d3d11", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d11 flip-model 0" -ArgList @(
  "--api", "d3d11", "--flip-model", "0", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "api none (GDI)" -ArgList @(
  "--api", "none", "--exit-after", "$Seconds", "--width", "640", "--height", "360"
) | Out-Null

# d3d12: required on machines with a working D3D12 stack (GHA windows-2022 usually OK)
Invoke-Smoke -Label "d3d12" -ArgList @(
  "--api", "d3d12", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

# block mode that does not need OBS: just proves flag parses + process lives
Invoke-Smoke -Label "squat-ipc arm" -ArgList @(
  "--api", "d3d11", "--block-capture", "squat-ipc", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

# rename-like title/class only (no file rename needed for this smoke)
Invoke-Smoke -Label "cs2-ish identity" -ArgList @(
  "--profile", "cs2", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

# vulkan is optional — runner may lack vulkan-1.dll
$vk = Invoke-Smoke -Label "vulkan (optional)" -Optional -ArgList @(
  "--api", "vulkan", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
)

Write-Host ""
Write-Host "ci-smoke OK (vulkan optional ran=$vk)" -ForegroundColor Green
exit 0
