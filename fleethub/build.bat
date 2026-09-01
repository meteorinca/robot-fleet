@echo off
REM FleetHub Multi-Platform Build Script (Batch)
REM Double-click or run from terminal to build PC and Raspberry Pi binaries

title FleetHub Multi-Target Build
echo ==========================================================
echo        FleetHub Mothership - Multi-Target Build
echo ==========================================================

cd /d "%~dp0"

if not exist "pi3_deploy" mkdir "pi3_deploy"

echo.
echo [1/3] Building Local PC Binary (fleethub.exe)...
set CGO_ENABLED=0
set GOOS=windows
set GOARCH=amd64
set GOARM=
go build -ldflags="-s -w" -o fleethub.exe ./cmd/fleethub
if errorlevel 1 (
    echo [ERROR] Failed to build fleethub.exe!
    pause
    exit /b 1
)
echo   -^> fleethub.exe built successfully.

echo.
echo [2/3] Building Raspberry Pi 32-bit Binary (pi3_deploy/fleethub-pi-32bit)...
set CGO_ENABLED=0
set GOOS=linux
set GOARCH=arm
set GOARM=7
go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi-32bit ./cmd/fleethub
if errorlevel 1 (
    echo [ERROR] Failed to build 32-bit Pi binary!
    pause
    exit /b 1
)
echo   -^> pi3_deploy/fleethub-pi-32bit built successfully.

echo.
echo [3/3] Building Raspberry Pi 64-bit Binary (pi3_deploy/fleethub-pi)...
set CGO_ENABLED=0
set GOOS=linux
set GOARCH=arm64
set GOARM=
go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi ./cmd/fleethub
if errorlevel 1 (
    echo [ERROR] Failed to build 64-bit Pi binary!
    pause
    exit /b 1
)
echo   -^> pi3_deploy/fleethub-pi built successfully.

echo.
echo [Config] Syncing fleethub_config.json to pi3_deploy/...
copy /y "fleethub_config.json" "pi3_deploy\fleethub_config.json" >nul
echo   -^> fleethub_config.json synced.

echo.
echo ==========================================================
echo   ALL BUILDS COMPLETE AND READY TO DEPLOY!
echo   - Windows PC:   fleethub.exe
echo   - Raspberry Pi: Copy pi3_deploy folder to Pi and run:
echo                   sudo bash setup.sh
echo ==========================================================
echo.
pause
