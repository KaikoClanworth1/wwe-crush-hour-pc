@echo off
rem WWE Crush Hour - native PC port installer: copies the game data from your own disc image (ISO) or disc drive
rem into the GameFiles folder next to this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Installer\install.ps1" %*
pause
