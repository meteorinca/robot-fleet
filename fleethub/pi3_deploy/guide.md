# FleetHub — Raspberry Pi Quick Run Guide

**NO Go compiler, Node.js, or development tools are required on the Pi.**

FleetHub is packaged as a standalone binary with all web interface assets, icons, fonts, 3D engines, and REST endpoints baked inside. It runs out of the box on any Raspberry Pi OS (32-bit or 64-bit), DietPi, or Ubuntu for Pi.

---

## ⚡ 3-Step Setup (No Dev Tools Required)

### 1. Pull the code onto your Pi
```bash
git pull
cd pi3_deploy
```

### 2. Make the installer executable
```bash
chmod +x setup.sh
```

### 3. Run the automated installer
```bash
./setup.sh
```

`setup.sh` automatically detects if your Pi OS is 32-bit or 64-bit, configures permissions, creates the systemd background service, and launches FleetHub!

---

## 🌐 Accessing FleetHub

Open your web browser on any phone, tablet, or PC on the same network:
```text
http://<YOUR-PI-IP>:8080
```
*(Example: `http://192.168.1.150:8080` or `http://raspberrypi.local:8080`)*

---

## 🛠 Useful Service Commands

FleetHub runs continuously in the background as a systemd service.

- **Check status & IP**:
  ```bash
  sudo systemctl status fleethub
  ```
- **View real-time logs**:
  ```bash
  sudo journalctl -u fleethub -f
  ```
- **Restart FleetHub**:
  ```bash
  sudo systemctl restart fleethub
  ```
- **Stop FleetHub**:
  ```bash
  sudo systemctl stop fleethub
  ```

---

## 📋 Supported Pi Operating Systems
- **Raspberry Pi OS (64-bit)**
- **Raspberry Pi OS (32-bit / Legacy)**
- **DietPi**
- **Ubuntu Server / Desktop for Raspberry Pi**
- **Armbian / OSMC**
