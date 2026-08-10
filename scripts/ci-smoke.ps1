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
Invoke-Smoke -Label "version" -ArgList @("--version") | Out-Null
Write-Host ""
Write-Host "=== version string ===" -ForegroundColor Cyan
$verOut = & $Exe --version
if ($LASTEXITCODE -ne 0) {
  Write-Error "--version exit=$LASTEXITCODE"
  exit 1
}
$verLine = if ($verOut -is [array]) { $verOut[0] } else { [string]$verOut }
Write-Host "version=$verLine"
if ($verLine -notmatch '^\d+\.\d+\.\d+') {
  Write-Error "--version output does not start with X.Y.Z: '$verLine'"
  exit 1
}
Invoke-Smoke -Label "list-profiles" -ArgList @("--list-profiles") | Out-Null
Invoke-Smoke -Label "list-scenes" -ArgList @("--list-scenes") | Out-Null
Invoke-Smoke -Label "bad scene" -ArgList @("--scene", "nope") -ExpectExit 2 | Out-Null

# JSON must be parseable (profiles script contracts on this)
Write-Host ""
Write-Host "=== list-profiles --json ===" -ForegroundColor Cyan
$jsonOut = & $Exe --list-profiles --json
if ($LASTEXITCODE -ne 0) {
  Write-Error "list-profiles --json exit=$LASTEXITCODE"
  exit 1
}
$joined = if ($jsonOut -is [array]) { $jsonOut -join "`n" } else { [string]$jsonOut }
# PS 5.1 ConvertFrom-Json can collapse arrays-of-objects into one object with
# array-valued properties. Wrap so the array is a single property value.
$wrapped = '{"items":' + $joined.Trim() + '}'
$parsed = ConvertFrom-Json -InputObject $wrapped
$profiles = @($parsed.items)
Write-Host "JSON OK ($($joined.Length) chars, $($profiles.Count) profiles)"
$blocked = $profiles | Where-Object { [string]$_.id -eq 'cs2-blocked' } | Select-Object -First 1
if (-not $blocked) {
  Write-Error "cs2-blocked profile missing"
  exit 1
}
if ([string]$blocked.defaultBlock -ne 'signature-policy') {
  Write-Error "cs2-blocked defaultBlock=$($blocked.defaultBlock) expected signature-policy"
  exit 1
}
if (-not $blocked.clientWidth -or -not $blocked.clientHeight) {
  Write-Error "profiles JSON missing clientWidth/clientHeight"
  exit 1
}

# Short live windows — d3d11 falls back to WARP if needed
Invoke-Smoke -Label "d3d11 default" -ArgList @(
  "--api", "d3d11", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d11 scene orbital" -ArgList @(
  "--api", "d3d11", "--scene", "orbital", "--scene-seed", "1",
  "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d11 scene fractal" -ArgList @(
  "--api", "d3d11", "--scene", "fractal", "--scene-seed", "0",
  # Tiny viewport: heavy raymarch is path-covered without WARP multi-minute stalls.
  "--exit-after", "$Seconds", "--width", "160", "--height", "90", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d11 gpu-mem 100" -ArgList @(
  "--api", "d3d11", "--gpu-mem", "100",
  "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d11 flip-model 0" -ArgList @(
  "--api", "d3d11", "--flip-model", "0", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "api none (GDI)" -ArgList @(
  "--api", "none", "--exit-after", "$Seconds", "--width", "640", "--height", "360"
) | Out-Null

Invoke-Smoke -Label "api none orbital" -ArgList @(
  "--api", "none", "--scene", "orbital", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360"
) | Out-Null

# d3d12: required on machines with a working D3D12 stack (GHA windows-2022 usually OK)
Invoke-Smoke -Label "d3d12" -ArgList @(
  "--api", "d3d12", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-Smoke -Label "d3d12 orbital" -ArgList @(
  "--api", "d3d12", "--scene", "orbital", "--scene-seed", "1",
  "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

# block modes: must self-verify (exit 0 + process lived). Prefer --events json + ready-file.
function Invoke-BlockSmoke {
  param(
    [Parameter(Mandatory = $true)][string]$Label,
    [Parameter(Mandatory = $true)][string[]]$ArgList,
    [string]$ExpectBlockMode
  )
  Write-Host ""
  Write-Host "=== $Label ===" -ForegroundColor Cyan
  $dir = Join-Path $env:TEMP ("fg-smoke-" + [guid]::NewGuid().ToString("n"))
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $ready = Join-Path $dir "ready.json"
  $stdout = Join-Path $dir "out.ndjson"
  $stderr = Join-Path $dir "err.log"
  $fullArgs = $ArgList + @("--events", "json", "--ready-file", $ready)
  Write-Host ("> " + $Exe + " " + ($fullArgs -join " "))
  $p = Start-Process -FilePath $Exe -ArgumentList $fullArgs -NoNewWindow -PassThru -Wait `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  $code = $p.ExitCode
  if ($null -eq $code) { $code = -1 }
  Write-Host "exit=$code (expect 0)"
  if ($code -ne 0) {
    if (Test-Path $stderr) { Get-Content $stderr | Select-Object -Last 30 | Write-Host }
    Write-Error "SMOKE FAIL: $Label exited $code"
    exit 1
  }
  if (-not (Test-Path $ready)) {
    Write-Error "SMOKE FAIL: $Label missing ready-file"
    exit 1
  }
  $readyObj = (Get-Content -LiteralPath $ready -Raw).Trim() | ConvertFrom-Json
  if (-not $readyObj.obsWindowSetting) {
    Write-Error "SMOKE FAIL: $Label ready missing obsWindowSetting"
    exit 1
  }
  if (-not $readyObj.clientWidth -or -not $readyObj.clientHeight) {
    Write-Error "SMOKE FAIL: $Label ready missing client size"
    exit 1
  }
  $nd = @()
  if (Test-Path $stdout) {
    $nd = Get-Content -LiteralPath $stdout | Where-Object { $_ -match '^\s*\{' }
  }
  $blockLine = $nd | Where-Object { $_ -match '"event"\s*:\s*"block_active"' } | Select-Object -Last 1
  if ($ExpectBlockMode -and $ExpectBlockMode -ne 'none') {
    if (-not $blockLine) {
      Write-Error "SMOKE FAIL: $Label missing block_active event"
      exit 1
    }
    if ($blockLine -notmatch '"verified"\s*:\s*true') {
      Write-Error "SMOKE FAIL: $Label block_active not verified: $blockLine"
      exit 1
    }
    if ($blockLine -notmatch [regex]::Escape($ExpectBlockMode)) {
      Write-Error "SMOKE FAIL: $Label block mode mismatch: $blockLine"
      exit 1
    }
  }
  Write-Host "ready+block OK obs=$($readyObj.obsWindowSetting)"
  Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
  return $true
}

# block mode that does not need OBS: proves flag + self-verify
Invoke-BlockSmoke -Label "squat-ipc arm+verify" -ExpectBlockMode "squat-ipc" -ArgList @(
  "--api", "d3d11", "--block-capture", "squat-ipc", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-BlockSmoke -Label "signature-policy arm+verify" -ExpectBlockMode "signature-policy" -ArgList @(
  "--api", "d3d11", "--block-capture", "signature-policy", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-BlockSmoke -Label "cs2-blocked profile default" -ExpectBlockMode "signature-policy" -ArgList @(
  "--profile", "cs2-blocked", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

Invoke-BlockSmoke -Label "events ready + instance" -ExpectBlockMode "none" -ArgList @(
  "--profile", "cs2", "--instance", "smoke1", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0", "--block-capture", "none"
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
