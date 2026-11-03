@echo off
rem Uninstall: double-click to put Reality Mixer's original openvr_api.dll back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
echo.
pause
