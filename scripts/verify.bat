@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
echo === dependents (x64) ===
dumpbin /dependents "%~dp0..\build\bin\fakegame.exe"
echo.
echo === run renamed cs2.exe 2s ===
mkdir "%~dp0_verify_empty" 2>nul
copy /Y "%~dp0..\build\bin\fakegame.exe" "%~dp0_verify_empty\cs2.exe" >nul
cd /d "%~dp0_verify_empty"
cs2.exe --exit-after 2 --title "Counter-Strike 2"
echo exit=%ERRORLEVEL%
echo.
echo === run --api none 2s ===
copy /Y "%~dp0..\build\bin\fakegame.exe" "%~dp0_verify_empty\fakegame.exe" >nul
fakegame.exe --api none --exit-after 2
echo exit=%ERRORLEVEL%
echo.
echo === run flip-model 0 2s ===
fakegame.exe --flip-model 0 --exit-after 2
echo exit=%ERRORLEVEL%
endlocal
