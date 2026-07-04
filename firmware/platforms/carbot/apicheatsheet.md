# CarBot API Cheat Sheet

This document lists the available HTTP GET/POST endpoints for CarBot.

**Base URL**: `http://carbot1.local:80` (replace `1` with your device number)

---

## 🚗 Motor Control

### Combined Drive Command
- **POST** `/drive` — Body: `{"throttle": -100..100, "steer": 0..100}`
  - `throttle`: negative = reverse, 0 = brake, positive = forward
  - `steer`: 0 = full left, 50 = center, 100 = full right
- **GET** `/drive?t=N&s=N` — Same but as GET query params

### Steering Only
- **GET** `/steer?pos=N` — Set steering position (0–100, 50=center)
- **GET** `/steer_center` — Reset estimated position to center without moving

### Drive (Throttle) Only
- **GET** `/throttle?v=N` — Set throttle (-100 to +100)
- **GET** `/brake` — Active brake (IN3+IN4 both HIGH)
- **GET** `/coast` — Free-wheel (IN3+IN4 both LOW)

### Motor State
- **GET** `/motor_state` — Returns `{"throttle":N,"steer":N,"braking":bool}`

---

## 📡 Ultrasonic Sensor
- **GET** `/us_on` — Enable sensor + switch OLED to ultrasonic view
- **GET** `/us_off` — Disable sensor + return OLED to normal
- **GET** `/us_data` — Returns `{"active":bool,"dist":N.N}`

---

## 💡 LED Control
- **GET** `/l1on` — Built-in LED ON
- **GET** `/l1off` — Built-in LED OFF
- **GET** `/toggle` — Toggle built-in LED

---

## 🎵 Buzzer
- **GET** `/tone?f=1000&d=100` — Play tone (f=Hz, d=duration ms)
- **GET** `/demo?type=coin` — Play demo melody
  - Types: `coin`, `gameover`, `siren`, `laser`, `mario`, `1up`

---

## 🖥️ OLED Display
- **GET** `/oled?text=Hello` — Show text on OLED for 4s
- **GET** `/show_ip` — Display device IP on OLED

---

## 🕒 Schedule (Synchronized Fleet Events)
- **GET** `/schedule?action=brake&delay=5` — Run action in 5 seconds
- **GET** `/schedule?action=coast&at=1714000000` — Run at exact Unix timestamp
- **GET** `/schedule?action=steer_center&delay=3` — Center steering in 3s

**Available scheduled actions**: `brake`, `coast`, `steer_center`, `us_on`, `us_off`, `l1on`, `l1off`, `toggle`, `show_ip`

---

## ⚙️ System
- **GET** `/status` — Firmware version, time sync, motor state JSON
- **GET** `/time` — NTP time JSON
- **GET** `/sync_time?epoch=N` — Set time from browser

---

## 📶 WiFi Provisioning
- **GET** `/wifi` — List saved networks + AP mode status
- **POST** `/wifi` — Body: `{"ssid":"...","pass":"..."}` → save and reboot
- **DELETE** `/wifi?delete=N` — Delete saved credential N
- **GET** `/wifi_scan` — Scan visible SSIDs

---

## 🔄 OTA Update
- **POST** `/ota` — Upload raw `.bin` file as body
  ```python
  import requests
  with open('carbot.bin','rb') as f:
      requests.post('http://carbot1.local/ota', data=f,
                    headers={'Content-Type':'application/octet-stream'})
  ```

---

## 🎮 Web UI Features
The built-in web UI (`http://carbot1.local/`) includes:
- **Throttle slider** (vertical, spring-back to brake on release)
- **Steering slider** (horizontal, spring-back to center on release)
- **D-pad buttons** for quick forward/back/left/right
- **Precision steering** — ±1, ±10 nudge buttons + fine slider
- **Keyboard control** — WASD / Arrow keys + Space=brake
- **Max speed limiter** slider
- **Live status** — throttle, steer position, distance, brake state
- **Ultrasonic** sensor enable/disable + live distance bar
- **OLED** text sender
- **Buzzer** demos + mini piano
- **WiFi** network manager
- **OTA** firmware update

---

*Tip: Use keyboard WASD or arrow keys in the web UI for direct driving!*
