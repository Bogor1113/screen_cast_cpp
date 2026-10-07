@echo off
rem ============================================================
rem  One-click package script for screen_cast
rem  Double-click to: build + auto version bump + generate installer
rem  Output goes to dist\ directory
rem ============================================================
chcp 65001 >nul
cd /d "%~dp0"
echo Packaging screen_cast ...
echo Will: build Release, bump version, generate MSI installer (student/teacher selectable)
echo.
powershell -ExecutionPolicy Bypass -NoProfile -File "%~dp0pack.ps1"
if errorlevel 1 (
    echo.
    echo [PACK FAILED] Check the error messages above.
    pause
    exit /b 1
)
echo.
echo [DONE] MSI + zip generated in dist\ directory.
echo.
rem keep the console open briefly so the user sees the result (double-click case);
rem ignore failure in automated/redirected contexts where timeout has no console
ping -n 3 127.0.0.1 >nul 2>&1
exit /b 0