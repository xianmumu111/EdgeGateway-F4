@echo off
REM ============================================================
REM  Modbus slave PC simulation - build and run
REM  NOTE: this file must stay pure ASCII. cmd.exe reads .bat
REM        using GBK by default, so any UTF-8 Chinese text here
REM        will be mangled into garbage commands.
REM        Chinese docs live in README.md (UTF-8, open with VS Code).
REM ============================================================
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] gcc not found in PATH.
    echo.
    echo Fix: install MinGW-w64, then add its bin folder to PATH.
    echo      e.g.  D:\MinGW\mingw64\bin
    echo.
    pause
    exit /b 1
)

del /q mb.o harness.o mbtest.exe 2>nul

echo [1/3] compiling modbus_slave.c ...
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 -Dprintf=mbtest_printf -D_INC_STDIO -I. -I..\Core\Inc -c ..\Core\Src\modbus_slave.c -o mb.o
if errorlevel 1 (
    echo.
    echo [BUILD FAILED] see errors above.
    pause
    exit /b 1
)

echo [2/3] compiling harness.c ...
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 -I. -I..\Core\Inc -c harness.c -o harness.o
if errorlevel 1 (
    echo.
    echo [BUILD FAILED] see errors above.
    pause
    exit /b 1
)

echo [3/3] linking ...
gcc mb.o harness.o -o mbtest.exe
if errorlevel 1 (
    echo.
    echo [LINK FAILED]
    pause
    exit /b 1
)

echo.
mbtest.exe
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo ############## ALL PASS ##############) else (echo ############## %RC% CASE^(S^) FAILED ##############)
echo.
pause
