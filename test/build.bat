@echo off
REM =============================================================
REM  EdgeGateway-F4  ::  unit test runner
REM  Each test_*.c has its own main(), so they are compiled one by one.
REM  Double-click this file, or run it from a command prompt.
REM =============================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set CFLAGS=-Wall -Wextra -std=c11 -O2 -finput-charset=UTF-8 -I..\Drivers\ringbuf -I..\Drivers\crc16 -I..\Drivers\modbus
set DRV=..\Drivers\ringbuf\ringbuf.c ..\Drivers\crc16\crc16.c ..\Drivers\modbus\modbus_rtu.c
set FAILED=0

for %%f in (test_*.c) do (
    echo.
    echo ==================== %%~nxf ====================
    echo [BUILD] gcc %%~nxf
    gcc %CFLAGS% %%f %DRV% -o %%~nf.exe
    if !errorlevel! NEQ 0 (
        echo [BUILD FAILED] %%~nxf
        set FAILED=1
    ) else (
        %%~nf.exe
        if !errorlevel! NEQ 0 (
            echo [TEST FAILED] %%~nxf
            set FAILED=1
        ) else (
            echo [OK] %%~nxf passed
        )
    )
)

echo.
if "!FAILED!"=="0" (echo ***** ALL TESTS PASSED *****) else (echo ***** SOME TESTS FAILED *****)
endlocal & exit /b %FAILED%
