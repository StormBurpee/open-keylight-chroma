@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-open-keylight.ps1" %*
set "installer_result=%errorlevel%"
if not "%installer_result%"=="0" pause
exit /b %installer_result%
