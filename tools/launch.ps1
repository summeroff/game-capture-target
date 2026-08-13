<#
.SYNOPSIS
  Front door for fakegame capture QA — interactive menu or non-interactive JSON launch.

.EXAMPLE
  .\tools\launch.ps1
  .\tools\launch.ps1 -Profile cs2 -Api d3d11 -BlockCapture signature-policy -Json
  .\tools\launch.ps1 -Profile cs2-blocked -Json
  .\tools\launch.ps1 -List -Json
  .\tools\launch.ps1 -StopAll
  .\tools\launch.ps1 -Profile cs2 -Instance a -Json
#>
[CmdletBinding()]
param(
  [string] $Profile,
  [ValidateSet('d3d11','d3d12','vulkan','none','')]
  [string] $Api = '',
  [ValidateSet('none','signature-policy','squat-ipc','unload-hook','')]
  [string] $BlockCapture = '',
  [int] $ExitAfter = 0,
  [string] $Title,
  [string] $Class,
  [string] $Instance,
  [string] $Source,
  [switch] $List,
  [switch] $Json,
  [switch] $StopAll,
  [int] $WaitSeconds = 15
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$spawnRoot = Join-Path $repoRoot 'tools\_spawn'

function Find-FakeGame {
  param([string]$Source)
  if ($Source -and (Test-Path -LiteralPath $Source)) { return (Resolve-Path $Source).Path }
  $candidates = @(
    (Join-Path $repoRoot 'build\bin\fakegame.exe'),
    (Join-Path $repoRoot 'build-x86\bin\fakegame.exe'),
    (Join-Path $repoRoot 'build\Release\fakegame.exe')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c) { return (Resolve-Path $c).Path }
  }
  throw "fakegame.exe not found. Build first (scripts\build.bat) or pass -Source."
}

function Get-ProfilesJson {
  param([string]$Exe)
  $lines = & $Exe --list-profiles --json 2>&1
  if ($LASTEXITCODE -ne 0) { throw "fakegame --list-profiles --json failed: $lines" }
  # Do NOT use Out-String — it pad-wraps and corrupts JSON.
  $text = (($lines | ForEach-Object { "$_" }) -join "`n").Trim()
  # PS 5.1: wrap array so ConvertFrom-Json does not collapse object arrays.
  $parsed = ConvertFrom-Json -InputObject ('{"items":' + $text + '}')
  return @($parsed.items)
}

# Write-Error is terminating under $ErrorActionPreference=Stop; use stderr + exit instead.
function Fail-Launch {
  param([string]$Message, [int]$Code = 1)
  [Console]::Error.WriteLine("error: $Message")
  exit $Code
}

# PS 5.1 Start-Process flattens string[] ArgumentList without quoting.
# Build one Windows command-line string with proper quotes.
function Format-WinArgList([string[]]$Parts) {
  ($Parts | ForEach-Object {
    $s = [string]$_
    if ($s -notmatch '[\s"]') { return $s }
    '"' + ($s -replace '(\\*)"','$1$1\"' -replace '(\\+)$','$1$1') + '"'
  }) -join ' '
}

function Stop-AllSpawned {
  if (-not (Test-Path -LiteralPath $spawnRoot)) {
    if ($Json) { @{ stopped = @(); spawnDirCleared = $true } | ConvertTo-Json -Compress; return }
    Write-Host "nothing to stop ($spawnRoot missing)"
    return
  }
  $stopped = @()
  Get-ChildItem -LiteralPath $spawnRoot -Recurse -Filter *.exe -ErrorAction SilentlyContinue | ForEach-Object {
    $name = $_.Name
    Get-CimInstance Win32_Process -Filter "Name='$name'" -ErrorAction SilentlyContinue | ForEach-Object {
      try {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop
        $stopped += $_.ProcessId
      } catch {}
    }
  }
  # Also kill by path under spawn root
  Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ExecutablePath -and $_.ExecutablePath.StartsWith($spawnRoot, [System.StringComparison]::OrdinalIgnoreCase)
  } | ForEach-Object {
    try {
      Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop
      if ($stopped -notcontains $_.ProcessId) { $stopped += $_.ProcessId }
    } catch {}
  }
  Start-Sleep -Milliseconds 300
  Remove-Item -LiteralPath $spawnRoot -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $spawnRoot | Out-Null
  if ($Json) {
    @{ stopped = $stopped; spawnDirCleared = $true } | ConvertTo-Json -Compress
  } else {
    Write-Host "stopped pids: $($stopped -join ', ')"
    Write-Host "cleared $spawnRoot"
  }
}

function Wait-ReadyFile {
  param([string]$Path, [int]$TimeoutSec, [int]$ProcessId)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $Path) {
      try {
        $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        if ($raw -and $raw.Trim().Length -gt 0) {
          return ($raw.Trim() | ConvertFrom-Json)
        }
      } catch {
        # partial write — retry
      }
    }
    $alive = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $alive) { return $null }
    Start-Sleep -Milliseconds 50
  }
  return $null
}

if ($StopAll) {
  Stop-AllSpawned
  exit 0
}

$exe = Find-FakeGame -Source $Source
$profiles = @(Get-ProfilesJson -Exe $exe)

if ($List) {
  if ($Json) {
    # Pass through exact app JSON
    & $exe --list-profiles --json
    exit $LASTEXITCODE
  }
  $profiles | Format-Table id, match, severityName, captureExpected, defaultBlock, clientWidth, clientHeight, notes -AutoSize
  exit 0
}

# Interactive defaults
if (-not $Profile) {
  Write-Host ""
  Write-Host ("{0,3}  {1,-16} {2,-10} {3,-9} {4}" -f '#','Profile','Match','Severity','Capture expected')
  Write-Host ("{0,3}  {1,-16} {2,-10} {3,-9} {4}" -f '---','-------','-----','--------','----------------')
  for ($i = 0; $i -lt $profiles.Count; $i++) {
    $p = $profiles[$i]
    $cap = if ($p.captureExpected) { 'yes' } else { 'NO' }
    Write-Host ("{0,3}  {1,-16} {2,-10} {3,-9} {4}" -f ($i+1), $p.id, $p.match, $p.severityName, $cap)
  }
  Write-Host ""
  $sel = Read-Host "Profile # or id [1]"
  if ([string]::IsNullOrWhiteSpace($sel)) { $sel = '1' }
  if ($sel -match '^\d+$') {
    $idx = [int]$sel - 1
    if ($idx -lt 0 -or $idx -ge $profiles.Count) { throw "invalid selection $sel" }
    $Profile = $profiles[$idx].id
  } else {
    $Profile = $sel.Trim()
  }
}

$p = $profiles | Where-Object { $_.id -eq $Profile } | Select-Object -First 1
if (-not $p) { throw "unknown profile '$Profile' (use -List)" }

if (-not $Api) {
  if (-not $Json) {
    $a = Read-Host "API [d3d11]"
    if ([string]::IsNullOrWhiteSpace($a)) { $a = 'd3d11' }
    $Api = $a
  } else {
    $Api = 'd3d11'
  }
}

if (-not $BlockCapture) {
  $defBlock = if ($p.defaultBlock) { $p.defaultBlock } else { 'none' }
  if (-not $Json) {
    $b = Read-Host "Block capture [$defBlock]"
    if ([string]::IsNullOrWhiteSpace($b)) { $b = $defBlock }
    $BlockCapture = $b
  } else {
    $BlockCapture = $defBlock
  }
}

$exeName = if ($p.exe) { $p.exe } else { 'fakegame.exe' }
if (-not $exeName.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) { $exeName += '.exe' }

$spawnKey = [string]$p.id
if ($Instance) { $spawnKey = "$($p.id)_$Instance" }
$outDir = Join-Path $spawnRoot $spawnKey
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$dest = Join-Path $outDir ([string]$exeName)
Copy-Item -LiteralPath $exe -Destination $dest -Force

$readyPath = Join-Path $outDir 'ready.json'
if (Test-Path -LiteralPath $readyPath) { Remove-Item -LiteralPath $readyPath -Force }

$argList = @(
  "--profile", ([string]$p.id),
  "--api", $Api,
  "--block-capture", $BlockCapture,
  "--events", "json",
  "--ready-file", $readyPath
)
if ($ExitAfter -gt 0) { $argList += @('--exit-after', "$ExitAfter") }
if ($Title) { $argList += @('--title', $Title) }
if ($Class) { $argList += @('--class', $Class) }
if ($Instance) { $argList += @('--instance', $Instance) }

$cmdDisplay = "`"$dest`" " + (Format-WinArgList $argList)

if (-not $Json) {
  Write-Host "launch: $cmdDisplay"
}

# Redirect stdout (NDJSON) / stderr (human log) into spawn dir for debugging.
$stdoutLog = Join-Path $outDir 'stdout.ndjson'
$stderrLog = Join-Path $outDir 'stderr.log'
$argString = Format-WinArgList $argList
$proc = Start-Process -FilePath $dest -ArgumentList $argString -WorkingDirectory $outDir -PassThru `
  -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
if (-not $proc) { throw "Start-Process failed" }

# Fail fast if process dies immediately
Start-Sleep -Milliseconds 400
$alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
if (-not $alive) {
  if (Test-Path $stderrLog) { Get-Content $stderrLog | Write-Host }
  Fail-Launch -Message "process exited immediately (pid $($proc.Id)). This is the silent-death case the tool exists to catch." -Code 3
}

# App-owned ready JSON is the contract (obsWindowSetting computed once in the exe).
$ready = Wait-ReadyFile -Path $readyPath -TimeoutSec $WaitSeconds -ProcessId $proc.Id
if (-not $ready) {
  $alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
  if (Test-Path $stderrLog) { Get-Content $stderrLog | Select-Object -Last 40 | Write-Host }
  if (-not $alive) {
    Fail-Launch -Message "process exited before ready (pid $($proc.Id))" -Code 4
  }
  Fail-Launch -Message "timed out waiting for ready file (pid $($proc.Id), ${WaitSeconds}s)" -Code 5
}

$captureExpected = if ($null -ne $ready.captureExpected) {
  [bool]$ready.captureExpected
} elseif ($BlockCapture -eq 'none') {
  [bool]$p.captureExpected
} else {
  $false
}
$result = [ordered]@{
  profile           = if ($ready.profile) { $ready.profile } else { $p.id }
  pid               = if ($ready.pid) { [int]$ready.pid } else { $proc.Id }
  hwnd              = if ($ready.hwnd) { [string]$ready.hwnd } else { '' }
  exePath           = $dest
  exeName           = if ($ready.exe) { [string]$ready.exe } else { $exeName }
  windowClass       = if ($ready.windowClass) { [string]$ready.windowClass } else { '' }
  windowTitle       = if ($ready.windowTitle) { [string]$ready.windowTitle } else { '' }
  api               = if ($ready.api) { [string]$ready.api } else { $Api }
  blockCapture      = if ($ready.blockCapture) { [string]$ready.blockCapture } else { $BlockCapture }
  obsWindowSetting  = if ($ready.obsWindowSetting) { [string]$ready.obsWindowSetting } else { '' }
  captureExpected   = $captureExpected
  clientWidth       = if ($ready.clientWidth) { [int]$ready.clientWidth } else { 1280 }
  clientHeight      = if ($ready.clientHeight) { [int]$ready.clientHeight } else { 720 }
}
if ($Instance) { $result.instance = $Instance }
if ($ready.instance) { $result.instance = [string]$ready.instance }

# Still alive?
$alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
if (-not $alive) {
  if (Test-Path $stderrLog) { Get-Content $stderrLog | Select-Object -Last 40 | Write-Host }
  Fail-Launch -Message "process exited after ready (pid $($proc.Id)) - check block verify / stderr.log" -Code 6
}

if ($Json) {
  $result | ConvertTo-Json -Compress
} else {
  Write-Host ("pid={0} hwnd={1}" -f $result.pid, $result.hwnd)
  Write-Host ("obsWindowSetting={0}" -f $result.obsWindowSetting)
  Write-Host ("captureExpected={0} block={1}" -f $result.captureExpected, $result.blockCapture)
}

exit 0
