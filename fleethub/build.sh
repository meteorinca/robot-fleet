#!/usr/bin/env bash
# FleetHub Multi-Platform Build Script (Bash)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================================="
echo "       FleetHub Mothership — Multi-Target Build"
echo "=========================================================="

mkdir -p pi3_deploy

echo ""
echo "[1/3] Building Local PC Binary..."
CGO_ENABLED=0 go build -ldflags="-s -w" -o fleethub.exe ./cmd/fleethub
echo "  -> Built fleethub.exe"

echo ""
echo "[2/3] Building Raspberry Pi 32-bit Binary (ARMv7)..."
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi-32bit ./cmd/fleethub
echo "  -> Built pi3_deploy/fleethub-pi-32bit"

echo ""
echo "[3/3] Building Raspberry Pi 64-bit Binary (ARM64)..."
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags="-s -w" -o pi3_deploy/fleethub-pi ./cmd/fleethub
echo "  -> Built pi3_deploy/fleethub-pi"

echo ""
echo "[Config] Syncing fleethub_config.json to pi3_deploy/..."
cp -f fleethub_config.json pi3_deploy/fleethub_config.json
echo "  -> Synced configuration"

echo ""
echo "=========================================================="
echo "  ALL BUILDS COMPLETE AND READY TO DEPLOY!"
echo "  - Windows PC:   ./fleethub.exe"
echo "  - Raspberry Pi: Copy pi3_deploy to Pi and run: sudo bash setup.sh"
echo "=========================================================="
