@echo off
rem Update dist exe from the staged v2.2 release (close the running app first!)
cd /d %~dp0
copy /y "releases\20260928_v2.2\ADS1115Host.exe" "dist\ADS1115Host.exe"
if errorlevel 1 (
  echo [ERROR] Update failed - is ADS1115Host.exe still running?
) else (
  echo Update done.
)
pause
