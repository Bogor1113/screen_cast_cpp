@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
REM ============================================================
REM  build.bat -- one-click build + package for screen_cast (Qt6 GUI + FFmpeg, x64 Release)
REM  Each run: auto version bump + CMake incremental build + windeployqt deploy + MSI/zip package
REM  Output : build\Release\*.exe (client_gui / teacher_gui / teacher_cli / ...)
REM           dist\ScreenCast_vX.Y.msi + dist\ScreenCast_vX.Y_zip (installer, auto-generated)
REM  Skip version bump     : build.bat --nobump
REM  NOTE: keep this file ASCII-only. cmd parses it under the local codepage;
REM        UTF-8 Chinese comments get misread and break command parsing.
REM ============================================================

set "NOBUMP="
if /i "%~1"=="--nobump" set "NOBUMP=1"

echo ===== [1/5] Version bump =====
if defined NOBUMP (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\version_bump.ps1" -SkipBump
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\version_bump.ps1"
)
if errorlevel 1 ( echo [ERROR] version bump failed & exit /b 1 )

set "SC_VERSION="
for /f "usebackq delims=" %%v in ("%~dp0dist\version.txt") do set "SC_VERSION=%%v"
if not defined SC_VERSION ( echo [ERROR] cannot read dist\version.txt & exit /b 1 )
echo Current version: v%SC_VERSION%

echo ===== [2/5] CMake configure (first run only) =====
if not exist "%~dp0build\CMakeCache.txt" (
    if not defined QT6_ROOT_DIR set "QT6_ROOT_DIR=D:\Qt\6.8.3\msvc2022_64"
    cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 ^
          -DQT6_ROOT_DIR="%QT6_ROOT_DIR%" ^
          -DFFMPEG_ROOT="%~dp0third_party\ffmpeg"
    if errorlevel 1 ( echo [ERROR] cmake configure failed & exit /b 1 )
)

echo ===== [3/5] Build Release =====
cmake --build "%~dp0build" --config Release
if errorlevel 1 ( echo [ERROR] build failed & exit /b 1 )

echo ===== [4/5] Deploy Qt runtime (windeployqt) =====
if not defined QT6_ROOT_DIR set "QT6_ROOT_DIR=D:\Qt\6.8.3\msvc2022_64"
if exist "%QT6_ROOT_DIR%\bin\windeployqt.exe" (
    "%QT6_ROOT_DIR%\bin\windeployqt.exe" --release --dir "%~dp0build\Release" "%~dp0build\Release\client_gui.exe"
    "%QT6_ROOT_DIR%\bin\windeployqt.exe" --release --dir "%~dp0build\Release" "%~dp0build\Release\teacher_gui.exe"
) else (
    echo [WARN] windeployqt.exe not found, skip Qt deploy
)

echo ===== Verify outputs =====
set "MISSING="
for %%e in (client_gui.exe teacher_gui.exe teacher_cli.exe client_console.exe) do (
    if not exist "%~dp0build\Release\%%e" set "MISSING=!MISSING! %%e"
)
if defined MISSING ( echo [ERROR] missing outputs:!MISSING! & exit /b 1 )

echo ===== [5/5] Package MSI + zip =====
REM build.bat already compiled and bumped the version, so call pack.ps1 with
REM -NoBuild (skip rebuild) and -SkipBump (avoid double version bump).
REM Output goes to dist\.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack.ps1" -NoBuild -SkipBump
if errorlevel 1 ( echo [ERROR] package failed & exit /b 1 )

echo.
echo [OK] build + package done v%SC_VERSION% -- outputs in build\Release\ and dist\
exit /b 0
