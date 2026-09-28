@echo off
rem ADS1115 host app launcher - uses the isolated venv in this folder
cd /d %~dp0
if not exist ".venv\Scripts\python.exe" (
    echo [ERROR] venv not found. Run:
    echo   python -m venv .venv
    echo   .venv\Scripts\python.exe -m pip install -r requirements.txt
    pause
    exit /b 1
)
.venv\Scripts\python.exe ads1115_host.py %*
if errorlevel 1 pause
