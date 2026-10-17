@echo off
rem WWE Crush Hour - native PC port launcher.
rem WWECrushHour.exe also starts on its own (it finds GameFiles, the settings and the saves itself);
rem this only checks that the game data is installed first.
if not exist "%~dp0GameFiles\SLUS_203.85" (
  echo The game data is not installed yet. Run "Install WWE Crush Hour.bat" first.
  pause
  exit /b 1
)
cd /d "%~dp0GameFiles"
start "" "%~dp0WWECrushHour.exe" "%~dp0GameFiles\SLUS_203.85"
