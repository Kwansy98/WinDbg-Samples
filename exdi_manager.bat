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
if %errorlevel%==0 (
    start "" pyw -3 "%SCRIPT%"
    exit /b 0
)

where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw "%SCRIPT%"
    exit /b 0
)

where py >nul 2>nul
if %errorlevel%==0 (
    start "" py -3 "%SCRIPT%"
    if errorlevel 1 pause
    exit /b %errorlevel%
)

where python >nul 2>nul
if %errorlevel%==0 (
    start "" python "%SCRIPT%"
    if errorlevel 1 pause
    exit /b %errorlevel%
)

echo Python 3 was not found. Install Python 3 or enable the Windows Python launcher.
pause
exit /b 1
