#!/bin/bash
set -e

echo "========================================================"
echo "   FleetHub Mothership — Raspberry Pi 3 Installer"
echo "========================================================"

# Determine architecture
ARCH=$(uname -m)
echo "--> Detected System Architecture: $ARCH"

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "x86_64" ]; then
    echo "--> Using 64-bit binary (fleethub-pi)"
    chmod +x fleethub-pi
    cp fleethub-pi fleethub-bin
else
    echo "--> Using 32-bit binary (fleethub-pi-32bit)"
    chmod +x fleethub-pi-32bit
    cp fleethub-pi-32bit fleethub-bin
fi

chmod +x fleethub-bin

echo "--> Installing systemd service..."
sudo cp fleethub.service /etc/systemd/system/fleethub.service
sudo systemctl daemon-reload
sudo systemctl enable fleethub.service
sudo systemctl restart fleethub.service

echo ""
echo "========================================================"
echo "   FleetHub is ACTIVE and Running on Raspberry Pi 3!"
echo "   Access Dashboard: http://$(hostname -I | awk '{print $1}'):8080"
echo "   View Live Logs:   sudo journalctl -u fleethub -f"
echo "========================================================"
