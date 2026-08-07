@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake --build build --config Release -- /t:Rebuild /p:WarningLevel=4 /p:TreatWarningAsError=true /m
endlocal
