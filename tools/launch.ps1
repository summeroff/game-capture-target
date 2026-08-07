<#
.SYNOPSIS
  Front door for fakegame capture QA — interactive menu or non-interactive JSON launch.

.EXAMPLE
  .\tools\launch.ps1
  .\tools\launch.ps1 -Profile cs2 -Api d3d11 -BlockCapture signature-policy -Json
  .\tools\launch.ps1 -List -Json
  .\tools\launch.ps1 -StopAll
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
  $text = ($lines | ForEach-Object { "$_" }) -join "`n"
  $parsed = $text.Trim() | ConvertFrom-Json
  # PS5 may return a single object for 1-element arrays; always normalize to array.
  return @($parsed)
}

function Encode-ObsPart([string]$s) {
  if ($null -eq $s) { return '' }
  return (($s -replace '#','#22') -replace ':','#3A')
}

function Get-ObsWindowSetting([string]$title, [string]$cls, [string]$exeName) {
  return "$(Encode-ObsPart $title):$(Encode-ObsPart $cls):$(Encode-ObsPart $exeName)"
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

function Wait-ProcessWindow {
  param([int]$ProcessId, [int]$TimeoutSec, [string]$ClassHint)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  if (-not ("FakeGameNative.User32" -as [type])) {
    Add-Type -Namespace FakeGameNative -Name User32 -MemberDefinition @"
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc lp, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr h, out RECT r);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int L,T,R,B; }
"@
  }
  while ((Get-Date) -lt $deadline) {
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $proc) { return $null }
    $state = @{ Found = [IntPtr]::Zero; Class = ''; Title = '' }
    $cb = [FakeGameNative.User32+EnumProc]{
      param([IntPtr]$h, [IntPtr]$l)
      $wpid = [uint32]0
      [void][FakeGameNative.User32]::GetWindowThreadProcessId($h, [ref]$wpid)
      if ($wpid -ne [uint32]$ProcessId) { return $true }
      if (-not [FakeGameNative.User32]::IsWindowVisible($h)) { return $true }
      $sb = New-Object System.Text.StringBuilder 256
      [void][FakeGameNative.User32]::GetClassName($h, $sb, 256)
      $c = $sb.ToString()
      if ($ClassHint -and $c -ne $ClassHint) { return $true }
      $r = New-Object FakeGameNative.User32+RECT
      [void][FakeGameNative.User32]::GetClientRect($h, [ref]$r)
      if (($r.R - $r.L) -le 0 -or ($r.B - $r.T) -le 0) { return $true }
      $tb = New-Object System.Text.StringBuilder 512
      [void][FakeGameNative.User32]::GetWindowText($h, $tb, 512)
      $state.Found = $h
      $state.Class = $c
      $state.Title = $tb.ToString()
      return $false
    }
    [void][FakeGameNative.User32]::EnumWindows($cb, [IntPtr]::Zero)
    if ($state.Found -ne [IntPtr]::Zero) {
      return [pscustomobject]@{ Hwnd = $state.Found; Class = $state.Class; Title = $state.Title }
    }
    Start-Sleep -Milliseconds 100
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
  $profiles | Format-Table id, match, severityName, captureExpected, defaultBlock, notes -AutoSize
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

$outDir = Join-Path $spawnRoot ([string]$p.id)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$dest = Join-Path $outDir ([string]$exeName)
Copy-Item -LiteralPath $exe -Destination $dest -Force

$argList = @("--profile", ([string]$p.id), "--api", $Api, "--block-capture", $BlockCapture)
if ($ExitAfter -gt 0) { $argList += @('--exit-after', "$ExitAfter") }
if ($Title) { $argList += @('--title', $Title) }
if ($Class) { $argList += @('--class', $Class) }

$cmdDisplay = "`"$dest`" " + ($argList | ForEach-Object {
  if ($_ -match '\s') { "'$_'" } else { $_ }
}) -join ' '

if (-not $Json) {
  Write-Host "launch: $cmdDisplay"
}

$proc = Start-Process -FilePath $dest -ArgumentList $argList -WorkingDirectory $outDir -PassThru
if (-not $proc) { throw "Start-Process failed" }

# Fail fast if process dies immediately
Start-Sleep -Milliseconds 400
$alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
if (-not $alive) {
  Write-Error "process exited immediately (pid $($proc.Id)). This is the silent-death case the tool exists to catch."
  exit 3
}

$classHint = if ($Class) { $Class } elseif ($p.windowClass) { $p.windowClass } else { $null }
$win = Wait-ProcessWindow -ProcessId $proc.Id -TimeoutSec $WaitSeconds -ClassHint $classHint
if (-not $win) {
  # still alive but no window?
  $alive = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
  if (-not $alive) {
    Write-Error "process exited before window appeared (pid $($proc.Id))"
    exit 4
  }
  Write-Error "timed out waiting for window (pid $($proc.Id), ${WaitSeconds}s)"
  exit 5
}

$titleOut = if ($win.Title) { $win.Title } elseif ($Title) { $Title } else { $p.windowTitle }
$classOut = if ($win.Class) { $win.Class } elseif ($Class) { $Class } else { $p.windowClass }
$obs = Get-ObsWindowSetting -title $titleOut -cls $classOut -exeName $exeName
$captureExpected = if ($BlockCapture -eq 'none') { [bool]$p.captureExpected } else { $false }

$result = [ordered]@{
  profile           = $p.id
  pid               = $proc.Id
  hwnd              = ('0x{0:X8}' -f $win.Hwnd.ToInt64())
  exePath           = $dest
  exeName           = $exeName
  windowClass       = $classOut
  windowTitle       = $titleOut
  api               = $Api
  blockCapture      = $BlockCapture
  obsWindowSetting  = $obs
  captureExpected   = $captureExpected
}

if ($Json) {
  $result | ConvertTo-Json -Compress
} else {
  Write-Host "pid=$($result.pid) hwnd=$($result.hwnd)"
  Write-Host "obsWindowSetting=$($result.obsWindowSetting)"
  Write-Host "captureExpected=$($result.captureExpected)"
}

exit 0
