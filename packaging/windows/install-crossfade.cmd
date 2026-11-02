@echo off
rem ============================================================
rem  crossfade - one-click Windows installer
rem  Double-click this file. It downloads and runs the real
rem  installer (install.ps1) from the crossfade release page.
rem  Everything installs per-user; no admin rights needed.
rem ============================================================
setlocal
echo.
echo ==== crossfade installer ====
echo Downloading installer ...
set "PS1=%TEMP%\crossfade-install.ps1"
curl -sL -o "%PS1%" https://github.com/XBoom31/crossfade/releases/download/v0.1.0/install.ps1
if errorlevel 1 (
    echo.
    echo Could not download the installer - check your internet connection.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
