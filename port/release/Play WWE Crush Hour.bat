@echo off
rem WWE Crush Hour - native PC port launcher.
rem Runs from the GameFiles folder so settings (ps2x_settings.ini) and saves (mc0) are found.
cd /d "%~dp0GameFiles"
start "" "%~dp0WWECrushHour.exe" "%~dp0GameFiles\SLUS_203.85"
