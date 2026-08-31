# CamBot API Cheat Sheet

WiFi camera over HTTP — ESP32S3 Sense + OV2640.  
**Base URL**: `http://cambot1.local` (replace `1` with your device number)

---

## 📹 Camera — Stream

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/stream` | **MJPEG multipart stream** — open in `<img src>` or browser tab. Loops at max framerate (~15fps at SVGA). Returns 503 if stream is off. |
| `GET` | `/cam_on` | Start streaming → `{"streaming":true}` |
| `GET` | `/cam_off` | Stop streaming → `{"streaming":false}` |
| `GET` | `/cam_status` | Query state → `{"streaming":true\|false}` |
| `GET` | `/snapshot` | Capture one JPEG frame → `image/jpeg` (download) |

### Examples
```bash
# Start stream
curl http://cambot1.local/cam_on

# View stream in browser
open http://cambot1.local/stream

# Save a snapshot
curl http://cambot1.local/snapshot -o snap.jpg

# Check stream state
curl http://cambot1.local/cam_status
```

```python
import requests

# Grab a snapshot
r = requests.get('http://cambot1.local/snapshot')
with open('snap.jpg', 'wb') as f:
    f.write(r.content)

# Stream frames in Python (OpenCV)
import cv2
cap = cv2.VideoCapture('http://cambot1.local/stream')
```

---

## 💡 LED Control

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/l1on` | LED ON |
| `GET` | `/l1off` | LED OFF |
| `GET` | `/toggle` | Toggle LED |

---

## 🕒 Scheduling (Synchronized Actions)

Schedule any action at an exact Unix timestamp or relative delay.

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/schedule?action=cam_on&delay=5` | Start stream in 5 seconds |
| `GET` | `/schedule?action=cam_off&delay=30` | Stop stream in 30 seconds |
| `GET` | `/schedule?action=cam_on&at=1714000000` | Start at exact epoch timestamp |
| `GET` | `/schedule?action=l1on&delay=10` | LED on in 10 seconds |

**Parameters:**
- `action` — required: `cam_on`, `cam_off`, `l1on`, `l1off`, `toggle`, `stop`
- `delay` — seconds from now
- `delay_ms` — milliseconds from now (combine with `delay`)
- `at` — Unix epoch seconds (absolute time, for fleet sync)
- `ms` — sub-second offset in ms (use with `at=`)

**Fleet sync example (Python):**
```python
import requests, time
t = int(time.time()) + 10  # 10 seconds from now
for n in range(1, 4):
    requests.get(f'http://cambot{n}.local/schedule?action=cam_on&at={t}')
```

Returns JSON: `{"ok":true,"action":"cam_on","at":1714000010,"ms":0}`

---

## ⏱️ System, Status & Diagnostics

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/status` | Firmware version, streaming state, NTP sync |
| `GET` | `/time` | NTP epoch + formatted time |
| `GET` | `/sync_time?epoch=<epoch>` | Set device clock from browser |
| `GET` | `/diag` | **Deep diagnostic benchmark** (`?frames=20&bench=1`): camera capture timings (min/avg/max/p95), frame sizes, heap/PSRAM memory, WiFi RSSI & channel, live stream metrics |
| `GET` | `/diag_stream` | **Instrumented stream** — multipart MJPEG with per-frame header timing (`X-Cap-Time-Us`, `X-Frame-Num`) |

```bash
# Run 20-frame camera capture benchmark
curl http://cambot1.local/diag?frames=20

# Query system & live stream stats only without running camera bench
curl http://cambot1.local/diag?bench=0
```

---

## ⬆️ OTA Firmware Update

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/ota` | Upload raw `.bin` firmware — device reboots on success |

```bash
# Shell
curl -X POST http://cambot1.local/ota \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @cambot.bin

# Python
import requests
with open('cambot.bin', 'rb') as f:
    requests.post('http://cambot1.local/ota', data=f,
                  headers={'Content-Type': 'application/octet-stream'})
```

Or use the OTA section of the web UI at `http://cambot1.local`.

---

## 🏠 Web UI

Open `http://cambot1.local` in any browser:
- **Full-screen RC car controller** — camera feed + joystick side-by-side
- Live MJPEG feed with auto-reconnect
- **On-screen joystick** (drag to steer + drive simultaneously)
- **D-pad arrow buttons** (tap on mobile)
- **WASD / Arrow keys** (keyboard control on desktop)
- **Gamepad / controller support** (plug in any USB/Bluetooth gamepad)
- ⛔ **Emergency STOP** button (also: Spacebar)
- **CAM ON / CAM OFF** stream toggle
- 📷 Snapshot download
- 💡 LED toggle
- ⬆ OTA firmware upload (modal drawer)
- Real-time NTP clock

---

## 🚗 Motor Control

L298N dual H-bridge — Steering (IN1/IN2) + Drive (IN3/IN4).

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/drive?steer=<n>&drive=<n>` | Set motor values. `n` = -100…+100 (negative=reverse/right, positive=forward/left) |
| `GET` | `/drive?stop=1` | **Emergency stop** — both motors off immediately |
| `GET` | `/drive` | Query current motor state → `{"steer":<n>,"drive":<n>}` |

**Steer values:** `-100` = full right, `0` = centre, `+100` = full left  
**Drive values:** `-100` = full reverse, `0` = stop, `+100` = full forward

### Examples
```bash
# Drive forward, centre steering
curl "http://cambot1.local/drive?steer=0&drive=100"

# Steer hard left while driving
curl "http://cambot1.local/drive?steer=100&drive=80"

# Emergency stop
curl "http://cambot1.local/drive?stop=1"

# Check current motor state
curl http://cambot1.local/drive
```

```python
import requests

base = 'http://cambot1.local'

# Forward
requests.get(f'{base}/drive?steer=0&drive=100')

# Steer right while reversing
requests.get(f'{base}/drive?steer=-60&drive=-80')

# Stop
requests.get(f'{base}/drive?stop=1')
```

> **Note:** ENA/ENB are jumpered HIGH — full power only (bang-bang).
> Remove jumpers and add PWM on ENA/ENB for variable speed control.

---

*Build: `idf.py set-target esp32s3 build`*  
*Flash: `idf.py -p COM<N> flash monitor`*  
*Device number: `idf.py -DDEVICE_NUMBER=2 set-target esp32s3 build flash`*
