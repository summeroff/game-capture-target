@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem format.bat [--check]
rem Prefer clang-format 18.x (CI uses Ubuntu clang-format-18 = 18.1.3).
rem Override: set CLANG_FORMAT=C:\path\to\clang-format.exe

set CHECK=0
if /I "%~1"=="--check" set CHECK=1
if /I "%~1"=="-n" set CHECK=1
if /I "%~1"=="--dry-run" set CHECK=1

cd /d "%~dp0.."
if errorlevel 1 exit /b 1

set "CF="
if defined CLANG_FORMAT if exist "%CLANG_FORMAT%" set "CF=%CLANG_FORMAT%"

rem Prefer the real PyPI binary under site-packages (version-pinned install).
rem The Scripts\clang-format.exe shim may resolve to a different major via PYTHONPATH.
if not defined CF if exist "%LOCALAPPDATA%\hermes\hermes-agent\venv\Lib\site-packages\clang_format\data\bin\clang-format.exe" (
  set "CF=%LOCALAPPDATA%\hermes\hermes-agent\venv\Lib\site-packages\clang_format\data\bin\clang-format.exe"
)
if not defined CF if exist "%LOCALAPPDATA%\hermes\hermes-agent\venv\Scripts\clang-format.exe" (
  set "CF=%LOCALAPPDATA%\hermes\hermes-agent\venv\Scripts\clang-format.exe"
)

rem Single where invocation (stderr suppressed); no-ops if not on PATH.
if not defined CF for /f "delims=" %%I in ('where clang-format 2^>nul') do (
  if not defined CF set "CF=%%I"
)
if not defined CF if exist "C:\Program Files\LLVM\bin\clang-format.exe" (
  set "CF=C:\Program Files\LLVM\bin\clang-format.exe"
)
if not defined CF if exist "C:\Program Files (x86)\LLVM\bin\clang-format.exe" (
  set "CF=C:\Program Files (x86)\LLVM\bin\clang-format.exe"
)
if not defined CF (
  echo clang-format not found. For CI parity install 18.x:
  echo   set PYTHONPATH=
  echo   "%%LOCALAPPDATA%%\hermes\hermes-agent\venv\Scripts\python.exe" -m pip install "clang-format==18.1.3"
  echo   then re-run scripts\format.bat --check
  echo Or: winget install LLVM.LLVM  ^(may be a different major — CI wants 18^)
  exit /b 2
)

for /f "usebackq delims=" %%V in (`"%CF%" --version 2^>nul`) do (
  if not defined CFVER set "CFVER=%%V"
)
echo clang-format: !CFVER!
echo using: %CF%

rem Soft warn when not major 18 — CI is pinned to clang-format-18.
echo !CFVER! | findstr /I /C:"version 18." >nul
if errorlevel 1 (
  echo WARNING: CI uses Ubuntu clang-format 18.1.3. Local tool is not 18.x —
  echo          wrap/indent can differ. Prefer: pip install clang-format==18.1.3
  echo          and set CLANG_FORMAT to site-packages\clang_format\data\bin\clang-format.exe
)

set COUNT=0
set FAIL=0

rem Enumerate with dir /s /b. Empty globs print "File Not Found" to *stdout*
rem (not only stderr), so always require the path to exist as a file.
for %%E in (cpp hpp h) do (
  for %%R in (src) do (
    if exist "%%R\." (
      for /f "delims=" %%F in ('dir /s /b "%%R\*.%%E" 2^>nul') do (
        if exist "%%~fF" if not exist "%%~fF\" (
          set /a COUNT+=1
          if !CHECK! EQU 1 (
            "%CF%" --dry-run --Werror --style=file "%%~fF" >nul 2>nul
            if errorlevel 1 (
              echo NEED FORMAT: %%~fF
              rem Show a short diff for local actionability (best-effort).
              "%CF%" --style=file "%%~fF" > "%TEMP%\fg-cf-out.tmp" 2>nul
              if exist "%TEMP%\fg-cf-out.tmp" (
                fc /n "%%~fF" "%TEMP%\fg-cf-out.tmp" 2>nul | more +1
              )
              set FAIL=1
            )
          ) else (
            "%CF%" -i --style=file "%%~fF"
            if errorlevel 1 set FAIL=1
          )
        )
      )
    )
  )
)

if !COUNT! EQU 0 (
  echo No source files found under src\.
  exit /b 1
)

if !CHECK! EQU 1 (
  if !FAIL! EQU 1 (
    echo Format check FAILED ^(!COUNT! files scanned^). Run scripts\format.bat
    exit /b 1
  )
  echo Format check OK ^(!COUNT! files^).
  exit /b 0
)

if !FAIL! EQU 1 (
  echo Format rewrite had errors.
  exit /b 1
)
echo Formatted !COUNT! files.
exit /b 0
