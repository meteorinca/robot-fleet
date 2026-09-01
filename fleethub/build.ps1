# FleetHub Multi-Platform Build Script (PowerShell)
# Builds for:
#   1. Local PC (Windows fleethub.exe)
#   2. Raspberry Pi 32-bit (pi3_deploy/fleethub-pi-32bit)
#   3. Raspberry Pi 64-bit (pi3_deploy/fleethub-pi)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "       FleetHub Mothership -- Multi-Target Build" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

Set-Location $ScriptDir

# Ensure output deploy directory exists
$DeployDir = Join-Path $ScriptDir "pi3_deploy"
if (-not (Test-Path $DeployDir)) {
    New-Item -ItemType Directory -Path $DeployDir | Out-Null
}

# 1. Build Windows PC Binary
Write-Host "`n[1/3] Building Local PC Binary (fleethub.exe)..." -ForegroundColor Yellow
$env:CGO_ENABLED = "0"
$env:GOOS = "windows"
$env:GOARCH = "amd64"
$env:GOARM = ""
go build -ldflags="-s -w" -o fleethub.exe ./cmd/fleethub
if ($LASTEXITCODE -eq 0) {
    $bytes = (Get-Item "fleethub.exe").Length
    $pcSize = [math]::Round($bytes / 1MB, 2)
    Write-Host "  Success: fleethub.exe built ($pcSize MB)" -ForegroundColor Green
} else {
    Write-Host "  Error: Failed building fleethub.exe" -ForegroundColor Red
    exit 1
}

# 2. Build Raspberry Pi 32-bit Binary (Raspbian / ARMv7)
Write-Host "`n[2/3] Building Raspberry Pi 32-bit Binary (pi3_deploy/fleethub-pi-32bit)..." -ForegroundColor Yellow
$env:CGO_ENABLED = "0"
$env:GOOS = "linux"
$env:GOARCH = "arm"
$env:GOARM = "7"
go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi-32bit ./cmd/fleethub
if ($LASTEXITCODE -eq 0) {
    $bytes = (Get-Item "pi3_deploy/fleethub-pi-32bit").Length
    $pi32Size = [math]::Round($bytes / 1MB, 2)
    Write-Host "  Success: pi3_deploy/fleethub-pi-32bit built ($pi32Size MB)" -ForegroundColor Green
} else {
    Write-Host "  Error: Failed building 32-bit Pi binary" -ForegroundColor Red
    exit 1
}

# 3. Build Raspberry Pi 64-bit Binary (aarch64 / ARM64)
Write-Host "`n[3/3] Building Raspberry Pi 64-bit Binary (pi3_deploy/fleethub-pi)..." -ForegroundColor Yellow
$env:CGO_ENABLED = "0"
$env:GOOS = "linux"
$env:GOARCH = "arm64"
$env:GOARM = ""
go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi ./cmd/fleethub
if ($LASTEXITCODE -eq 0) {
    $bytes = (Get-Item "pi3_deploy/fleethub-pi").Length
    $pi64Size = [math]::Round($bytes / 1MB, 2)
    Write-Host "  Success: pi3_deploy/fleethub-pi built ($pi64Size MB)" -ForegroundColor Green
} else {
    Write-Host "  Error: Failed building 64-bit Pi binary" -ForegroundColor Red
    exit 1
}

# Sync config to pi3_deploy
Write-Host "`n[Config] Syncing fleethub_config.json and known_devices.json to pi3_deploy/..." -ForegroundColor Yellow
Copy-Item "fleethub_config.json" "pi3_deploy/fleethub_config.json" -Force
Copy-Item "known_devices.json" "pi3_deploy/known_devices.json" -Force
Write-Host "  Success: Configuration synced" -ForegroundColor Green

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "  ALL BUILDS COMPLETE AND READY TO DEPLOY!" -ForegroundColor Green
Write-Host "  - Windows PC:   .\fleethub.exe"
Write-Host "  - Raspberry Pi: Copy pi3_deploy/ to Pi and run: sudo bash setup.sh"
Write-Host "==========================================================" -ForegroundColor Cyan
