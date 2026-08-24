#!/bin/bash
set -e

VERSION="1.3.0"
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
RUN_USER="$(whoami)"

echo "========================================================"
echo "   RoboHub Mothership — Raspberry Pi 3 Installer"
echo "   Version: $VERSION"
echo "========================================================"

# Determine architecture & userland bitness
ARCH=$(uname -m)
BITNESS=$(getconf LONG_BIT 2>/dev/null || echo "32")
echo "--> Detected System Architecture: $ARCH ($BITNESS-bit userland)"
echo "--> Installation Directory: $SCRIPT_DIR"

chmod +x "$SCRIPT_DIR/robohub-pi" "$SCRIPT_DIR/robohub-pi-32bit" "$SCRIPT_DIR/fleethub-pi" "$SCRIPT_DIR/fleethub-pi-32bit" 2>/dev/null || true

if [ "$BITNESS" = "64" ] && [ "$ARCH" = "aarch64" ]; then
    echo "--> Using 64-bit binary"
    if [ -f "$SCRIPT_DIR/robohub-pi" ]; then
        cp "$SCRIPT_DIR/robohub-pi" "$SCRIPT_DIR/robohub-bin"
    else
        cp "$SCRIPT_DIR/fleethub-pi" "$SCRIPT_DIR/robohub-bin"
    fi
else
    echo "--> Using 32-bit binary"
    if [ -f "$SCRIPT_DIR/robohub-pi-32bit" ]; then
        cp "$SCRIPT_DIR/robohub-pi-32bit" "$SCRIPT_DIR/robohub-bin"
    else
        cp "$SCRIPT_DIR/fleethub-pi-32bit" "$SCRIPT_DIR/robohub-bin"
    fi
fi

chmod +x "$SCRIPT_DIR/robohub-bin"

# Update existing config if present
if [ -f "$SCRIPT_DIR/fleethub_config.json" ]; then
    echo "--> Migrating http_port in existing fleethub_config.json to 8126"
    sed -i 's/"http_port": 8080/"http_port": 8126/g' "$SCRIPT_DIR/fleethub_config.json"
fi
if [ -f "$SCRIPT_DIR/robohub_config.json" ]; then
    echo "--> Migrating http_port in existing robohub_config.json to 8126"
    sed -i 's/"http_port": 8080/"http_port": 8126/g' "$SCRIPT_DIR/robohub_config.json"
fi

echo "--> Installing systemd service..."
cat <<EOF | sudo tee /etc/systemd/system/robohub.service > /dev/null
[Unit]
Description=RoboHub Mothership Daemon & RF Mesh Gateway
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$SCRIPT_DIR
ExecStart=$SCRIPT_DIR/robohub-bin -homekit -port 8126
Restart=always
RestartSec=5s
LimitNOFILE=65536

StandardOutput=journal
StandardError=journal
SyslogIdentifier=robohub

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable robohub.service
sudo systemctl restart robohub.service

echo ""
echo "========================================================"
echo "   RoboHub is ACTIVE and Running on Raspberry Pi 3!"
echo "   Access Dashboard: http://$(hostname -I | awk '{print $1}'):8126"
echo "   View Live Logs:   sudo journalctl -u robohub -f"
echo "   Check Status:     sudo systemctl status robohub"
echo "========================================================"
