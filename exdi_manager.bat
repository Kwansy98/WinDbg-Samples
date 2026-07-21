@echo off
setlocal

cd /d "%~dp0"
set "SCRIPT=%~dp0exdi_manager.py"

if not exist "%SCRIPT%" (
    echo Cannot find "%SCRIPT%".
    pause
    exit /b 1
)

where pyw >nul 2>nul
if errorlevel 1 (
    echo Python 3 Windows launcher ^(pyw.exe^) was not found.
    echo Install Python 3 with tkinter and the Python launcher.
    pause
    exit /b 1
)

start "" pyw -3 "%SCRIPT%"
exit /b 0
