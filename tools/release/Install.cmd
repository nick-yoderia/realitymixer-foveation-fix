@echo off
rem Install: double-click to run. Closes and restarts Reality Mixer if it is running.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
echo.
pause
