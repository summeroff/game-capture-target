@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1
cd /d "%~dp0.."
if not exist build-x86 mkdir build-x86
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 || exit /b 1
cmake --build build-x86 --config Release -- /m || exit /b 1
echo.
echo Built: build-x86\bin\fakegame.exe
endlocal
