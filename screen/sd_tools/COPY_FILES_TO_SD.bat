@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0COPY_FILES_TO_SD.ps1" -Drive F:
echo.
pause
