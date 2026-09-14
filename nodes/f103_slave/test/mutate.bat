@echo off
REM ============================================================
REM  Mutation testing - verify the tests can actually fail.
REM  NOTE: keep this file pure ASCII (cmd.exe reads .bat as GBK).
REM ============================================================
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] python not found in PATH.
    echo.
    echo Fix: install Python 3 and add it to PATH, then run this again.
    echo      Or run manually:  python mutate.py
    echo.
    pause
    exit /b 1
)

python mutate.py
echo.
pause
