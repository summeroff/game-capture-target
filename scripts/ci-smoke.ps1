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
Invoke-Smoke -Label "deleted --config" -ArgList @("--config", "x.ini") -ExpectExit 2 | Out-Null
Invoke-Smoke -Label "after trailing comma" -ArgList @("--resize-after", "5,") -ExpectExit 2 | Out-Null
Invoke-Smoke -Label "churn nan" -ArgList @("--churn", "nan") -ExpectExit 2 | Out-Null
Invoke-Smoke -Label "after nan" -ArgList @("--recreate-swapchain-after", "nan") -ExpectExit 2 | Out-Null
Invoke-Smoke -Label "hooked+2 parse" -ArgList @(
  "--api", "none", "--recreate-swapchain-after", "hooked+2",
  "--exit-after", "1", "--width", "640", "--height", "360"
) | Out-Null

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

$dumpDir = Join-Path $env:TEMP ("fg-dump-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
$dump11 = Join-Path $dumpDir "d3d11.bmp"
$dump12 = Join-Path $dumpDir "d3d12.bmp"
Invoke-Smoke -Label "d3d11 dump-frame" -ArgList @(
  "--api", "d3d11", "--dump-frame", $dump11,
  "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null
if (-not (Test-Path -LiteralPath $dump11) -or ((Get-Item $dump11).Length -lt 1000)) {
  Write-Error "d3d11 dump-frame missing or tiny: $dump11"
  exit 1
}
Write-Host "d3d11 dump-frame OK $((Get-Item $dump11).Length) bytes"
Invoke-Smoke -Label "d3d12 dump-frame" -ArgList @(
  "--api", "d3d12", "--dump-frame", $dump12,
  "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null
if (-not (Test-Path -LiteralPath $dump12) -or ((Get-Item $dump12).Length -lt 1000)) {
  Write-Error "d3d12 dump-frame missing or tiny: $dump12"
  exit 1
}
Write-Host "d3d12 dump-frame OK $((Get-Item $dump12).Length) bytes"
Remove-Item -LiteralPath $dumpDir -Recurse -Force -ErrorAction SilentlyContinue

# CLI churn + events (no OBS). --verbose is the only path that should emit scene-emit.
Write-Host ""
Write-Host "=== recreate-swapchain-after event ===" -ForegroundColor Cyan
$churnDir = Join-Path $env:TEMP ("fg-churn-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Force -Path $churnDir | Out-Null
$churnOut = Join-Path $churnDir "out.ndjson"
$churnErr = Join-Path $churnDir "err.log"
$churnArgs = @(
  "--api", "d3d11", "--recreate-swapchain-after", "1",
  "--exit-after", "3", "--width", "640", "--height", "360", "--vsync", "0",
  "--events", "json"
)
Write-Host ("> " + $Exe + " " + ($churnArgs -join " "))
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $Exe @churnArgs 1>$churnOut 2>$churnErr
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($code -ne 0) {
  if (Test-Path $churnErr) { Get-Content $churnErr | Select-Object -Last 20 | Write-Host }
  Write-Error "recreate-swapchain-after exited $code"
  exit 1
}
$nd = @()
if (Test-Path $churnOut) {
  $nd = @(Get-Content -LiteralPath $churnOut | Where-Object { $_ -match '^\s*\{' })
}
if (-not ($nd | Where-Object { $_ -match '"event"\s*:\s*"swapchain_recreated"' })) {
  Write-Error "missing swapchain_recreated event"
  exit 1
}
if (Test-Path $churnErr) {
  $errTxt = Get-Content -LiteralPath $churnErr -Raw
  if ($errTxt -match 'scene-emit:') {
    Write-Error "scene-emit leaked without --verbose"
    exit 1
  }
}
Write-Host "swapchain_recreated OK; scene-emit quiet by default"
Remove-Item -LiteralPath $churnDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== --verbose scene-emit ===" -ForegroundColor Cyan
$verbDir = Join-Path $env:TEMP ("fg-verb-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Force -Path $verbDir | Out-Null
$verbErr = Join-Path $verbDir "err.log"
$verbArgs = @(
  "--api", "d3d11", "--verbose", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
)
Write-Host ("> " + $Exe + " " + ($verbArgs -join " "))
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $Exe @verbArgs 1>$null 2>$verbErr
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($code -ne 0) {
  Write-Error "--verbose exited $code"
  exit 1
}
$verbTxt = if (Test-Path $verbErr) { Get-Content -LiteralPath $verbErr -Raw } else { "" }
if ($verbTxt -notmatch 'scene-emit:') {
  Write-Error "--verbose missing scene-emit"
  exit 1
}
Write-Host "--verbose scene-emit OK"
Remove-Item -LiteralPath $verbDir -Recurse -Force -ErrorAction SilentlyContinue

# block modes: must self-verify (exit 0 + process lived). Prefer --events json + ready-file.
function Invoke-BlockSmoke {
  param(
    [Parameter(Mandatory = $true)][string]$Label,
    [Parameter(Mandatory = $true)][string[]]$ArgList,
    [string]$ExpectBlockMode,
    [string]$ExpectInstance = ''
  )
  Write-Host ""
  Write-Host "=== $Label ===" -ForegroundColor Cyan
  $dir = Join-Path $env:TEMP ("fg-smoke-" + [guid]::NewGuid().ToString("n"))
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $ready = Join-Path $dir "ready.json"
  $stdout = Join-Path $dir "out.ndjson"
  $stderr = Join-Path $dir "err.log"
  $fullArgs = @($ArgList) + @("--events", "json", "--ready-file", $ready)
  # Prefer call operator: Start-Process ArgumentList array is unquoted on PS 5.1.
  Write-Host ("> " + $Exe + " " + ($fullArgs -join " "))
  $prevEap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  & $Exe @fullArgs 1>$stdout 2>$stderr
  $code = $LASTEXITCODE
  $ErrorActionPreference = $prevEap
  if ($null -eq $code) { $code = -1 }
  Write-Host "exit=$code (expect 0)"
  if ($code -ne 0) {
    if (Test-Path $stderr) { Get-Content $stderr | Select-Object -Last 30 | Write-Host }
    Write-Host "SMOKE FAIL: $Label exited $code" -ForegroundColor Red
    exit 1
  }
  if (-not (Test-Path $ready)) {
    Write-Host "SMOKE FAIL: $Label missing ready-file" -ForegroundColor Red
    exit 1
  }
  $readyObj = (Get-Content -LiteralPath $ready -Raw).Trim() | ConvertFrom-Json
  if (-not $readyObj.obsWindowSetting) {
    Write-Host "SMOKE FAIL: $Label ready missing obsWindowSetting" -ForegroundColor Red
    exit 1
  }
  if (-not $readyObj.clientWidth -or -not $readyObj.clientHeight) {
    Write-Host "SMOKE FAIL: $Label ready missing client size" -ForegroundColor Red
    exit 1
  }
  $nd = @()
  if (Test-Path $stdout) {
    $nd = @(Get-Content -LiteralPath $stdout | Where-Object { $_ -match '^\s*\{' })
  }
  $readyLine = $nd | Where-Object { $_ -match '"event"\s*:\s*"ready"' } | Select-Object -First 1
  if (-not $readyLine) {
    Write-Host "SMOKE FAIL: $Label missing ready event on stdout NDJSON (lines=$($nd.Count))" -ForegroundColor Red
    exit 1
  }
  $blockLine = $nd | Where-Object { $_ -match '"event"\s*:\s*"block_active"' } | Select-Object -Last 1
  if ($ExpectBlockMode -and $ExpectBlockMode -ne 'none') {
    if (-not $blockLine) {
      Write-Host "SMOKE FAIL: $Label missing block_active event" -ForegroundColor Red
      exit 1
    }
    if ($blockLine -notmatch '"verified"\s*:\s*true') {
      Write-Host "SMOKE FAIL: $Label block_active not verified: $blockLine" -ForegroundColor Red
      exit 1
    }
    if ($blockLine -notmatch [regex]::Escape($ExpectBlockMode)) {
      Write-Host "SMOKE FAIL: $Label block mode mismatch: $blockLine" -ForegroundColor Red
      exit 1
    }
  }
  if ($ExpectInstance) {
    $cls = [string]$readyObj.windowClass
    $instField = [string]$readyObj.instance
    $okInst = ($instField -eq $ExpectInstance) -or ($cls -like "*_${ExpectInstance}")
    if (-not $okInst) {
      Write-Host "SMOKE FAIL: $Label instance not applied class=$cls instance=$instField expect=$ExpectInstance" -ForegroundColor Red
      exit 1
    }
    Write-Host "instance OK class=$cls"
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

Invoke-BlockSmoke -Label "events ready + instance" -ExpectBlockMode "none" -ExpectInstance "smoke1" -ArgList @(
  "--profile", "cs2", "--instance", "smoke1", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0", "--block-capture", "none"
) | Out-Null

# Two concurrent --instance targets: distinct classes, both stay alive.
Write-Host ""
Write-Host "=== concurrent --instance a+b ===" -ForegroundColor Cyan
$pairDir = Join-Path $env:TEMP ("fg-smoke-pair-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Force -Path $pairDir | Out-Null
$pair = @()
foreach ($id in @('a', 'b')) {
  $ready = Join-Path $pairDir "ready-$id.json"
  $err = Join-Path $pairDir "err-$id.log"
  $out = Join-Path $pairDir "out-$id.ndjson"
  $argStr = "--api none --profile cs2 --instance $id --exit-after 4 --width 320 --height 180 --block-capture none --events json --ready-file `"$ready`""
  $p = Start-Process -FilePath $Exe -ArgumentList $argStr -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $out -RedirectStandardError $err
  $pair += [pscustomobject]@{ Id = $id; Proc = $p; Ready = $ready; Err = $err }
}
$deadline = (Get-Date).AddSeconds(8)
$readyObjs = @{}
foreach ($item in $pair) {
  while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $item.Ready) {
      try {
        $raw = Get-Content -LiteralPath $item.Ready -Raw -ErrorAction Stop
        if ($raw -and $raw.Trim().Length -gt 0) {
          $readyObjs[$item.Id] = ($raw.Trim() | ConvertFrom-Json)
          break
        }
      } catch {}
    }
    if ($item.Proc.HasExited) { break }
    Start-Sleep -Milliseconds 50
  }
}
$failPair = $false
foreach ($item in $pair) {
  if (-not $readyObjs.ContainsKey($item.Id)) {
    Write-Host "SMOKE FAIL: concurrent instance $($item.Id) no ready (exited=$($item.Proc.HasExited))" -ForegroundColor Red
    if (Test-Path $item.Err) { Get-Content $item.Err | Select-Object -Last 20 | Write-Host }
    $failPair = $true
  }
}
if (-not $failPair) {
  $ca = [string]$readyObjs['a'].windowClass
  $cb = [string]$readyObjs['b'].windowClass
  if ($ca -eq $cb -or $ca -notlike '*_a' -or $cb -notlike '*_b') {
    Write-Host "SMOKE FAIL: concurrent classes not distinct a=$ca b=$cb" -ForegroundColor Red
    $failPair = $true
  }
}
foreach ($item in $pair) {
  if (-not $item.Proc.HasExited) {
    try { Stop-Process -Id $item.Proc.Id -Force -ErrorAction SilentlyContinue } catch {}
  }
}
Remove-Item -LiteralPath $pairDir -Recurse -Force -ErrorAction SilentlyContinue
if ($failPair) { exit 1 }
Write-Host "concurrent instance OK class_a=$($readyObjs['a'].windowClass) class_b=$($readyObjs['b'].windowClass)"

# Unrenamed exe-matched profile must emit warning.exe_mismatch (harness quiet-fail guard).
Write-Host ""
Write-Host "=== exe_mismatch warning (unrenamed cs2 profile) ===" -ForegroundColor Cyan
$mmDir = Join-Path $env:TEMP ("fg-smoke-mm-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Force -Path $mmDir | Out-Null
$mmOut = Join-Path $mmDir "out.ndjson"
$mmErr = Join-Path $mmDir "err.log"
$mmReady = Join-Path $mmDir "ready.json"
$prevEa = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $Exe --profile cs2 --events json --ready-file $mmReady --exit-after $Seconds --width 640 --height 360 --vsync 0 --block-capture none 1>$mmOut 2>$mmErr
$mmCode = $LASTEXITCODE
$ErrorActionPreference = $prevEa
if ($mmCode -ne 0) {
  Write-Error "exe_mismatch smoke exit=$mmCode"
  if (Test-Path $mmErr) { Get-Content $mmErr | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" } }
  exit 1
}
$mmLines = @()
if (Test-Path $mmOut) { $mmLines = @(Get-Content $mmOut | Where-Object { $_ -match '^\s*\{' }) }
$mmWarn = $mmLines | Where-Object { $_ -match '"event"\s*:\s*"warning"' -and $_ -match 'exe_mismatch' } | Select-Object -First 1
if (-not $mmWarn) {
  Write-Error "expected warning exe_mismatch on stdout when --profile cs2 without rename"
  Write-Host "ndjson lines: $($mmLines.Count)"
  $mmLines | ForEach-Object { Write-Host "  $_" }
  exit 1
}
if ($mmWarn -notmatch 'cs2\.exe' -or $mmWarn -notmatch 'fakegame\.exe') {
  Write-Error "exe_mismatch payload missing expected/actual: $mmWarn"
  exit 1
}
Write-Host "exe_mismatch OK" -ForegroundColor Green
Remove-Item $mmDir -Recurse -Force -ErrorAction SilentlyContinue

# rename-like title/class only (no file rename needed for this smoke)
Invoke-Smoke -Label "cs2-ish identity" -ArgList @(
  "--profile", "cs2", "--exit-after", "$Seconds",
  "--width", "640", "--height", "360", "--vsync", "0"
) | Out-Null

# vulkan is optional - runner may lack vulkan-1.dll
$vk = Invoke-Smoke -Label "vulkan (optional)" -Optional -ArgList @(
  "--api", "vulkan", "--exit-after", "$Seconds", "--width", "640", "--height", "360", "--vsync", "0"
)

Write-Host ""
Write-Host "ci-smoke OK (vulkan optional ran=$vk)" -ForegroundColor Green
exit 0
