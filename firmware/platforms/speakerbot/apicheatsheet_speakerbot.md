# SpeakerBot v1.0 API Cheat Sheet & Wiring Guide

SpeakerBot is an upgraded version of MyBot equipped with an **I2S PDM Speaker**, animated SSD1306 OLED eyes/games, single SG90 servo motor, status LEDs, button controls, and full **Text-to-Speech (TTS)** and sound clip streaming.

---

## 🔌 Hardware Wiring & Pinout Guide (ESP32-C3)

| Component | Pin / GPIO | Notes |
| :--- | :--- | :--- |
| **Built-in LED** | GPIO 8 | Active LOW status & heartbeat indicator |
| **Servo 1 (SG90)** | GPIO 5 | LEDC PWM Channel 0 (50Hz) |
| **OLED SDA** | GPIO 7 | I2C SSD1306 (128x64 display) |
| **OLED SCL** | GPIO 6 | I2C SSD1306 |
| **Green LED** | GPIO 20 | Breadboard status LED (Active HIGH) |
| **Red LED** | GPIO 21 | Breadboard status LED (Active HIGH) |
| **Button 1 (BTN 1)** | GPIO 0 | Short press cycles eye emotions |
| **Button 2 (BTN 2)** | GPIO 1 | Short press cycles fun OLED animations |
| **BOOT Button** | GPIO 9 | Hold 7 sec + triple click to reset WiFi |
| **Speaker PDM Data** | **GPIO 1** | I2S PDM TX Data pin to speaker module |
| **Speaker PDM Clock**| **GPIO 2** | I2S PDM TX Clock pin to speaker module |
| **Audio Amp Enable** | **GPIO 18**| Amplifier enable pin (HIGH = ON, LOW = OFF) |

> **Note**: GPIO 1 is shared on breadboard headers between BTN_2 and AUDIO_DATA. When using the standalone speaker module, wire AUDIO_DATA to its own header pin on the speaker board.

---

## 🛠️ Build & Flash Guide

Build using `idf.py` with board name, device number, and optional ultrasonic sensor configuration:

```bash
# Standard build (Ultrasonic disabled by default):
idf.py -DBOARD=esp32c3_speakerbot -DDEVICE_NUMBER=8 build

# Build with Ultrasonic (HC-SR04) enabled:
idf.py -DBOARD=esp32c3_speakerbot -DDEVICE_NUMBER=8 -DENABLE_ULTRASONIC=1 build
```

Passing `-DDEVICE_NUMBER=8` configures mDNS hostname `speakerbot8.local` and SoftAP `SpeakerBot-8`.

---

## 🌐 Network & Device Setup


- **mDNS Hostname**: `http://speakerbot<N>.local` (Replace `<N>` with device number passed during build via `-DDEVICE_NUMBER=1`)
- **SoftAP Hotspot**: `SpeakerBot-<N>` (Open hotspot fallback, IP: `http://192.168.4.1` with Captive Portal enabled)
- **WiFi Reset Procedure**: 
  1. Hold **BOOT button (GPIO 9)** for **7 seconds**.
  2. The OLED will show a countdown from 3.
  3. Release and **triple-click** the BOOT button to confirm reset.
  4. All saved WiFi credentials will be erased and the device will reboot into Hotspot mode.

---

## 🔊 Voice, TTS & Speaker Audio API

- **Stream Raw PCM Audio to Speaker**: `POST /audio`
  - *Body*: 16kHz 16-bit signed mono PCM binary chunks. Played in real-time through the I2S PDM speaker.
- **Broadcast TTS Text to SSE Web Browsers**: `GET /tts?say=Hello+Human` or `GET /sendtts:Hello+Human`
  - *Broadcasts text to connected browser clients for Web Speech API synthesis.*
- **Play Sound Clips**:
  - `GET /paulbot` — Play "Hi, My name is Paulbot" SAM TTS boot sound
  - `GET /bark` — Play dog bark audio clip
  - `GET /huh` — Play "Huh?" sound clip
  - `GET /yes` — Play "Yes" sound clip
  - `GET /jump` — Play cartoon jump sound
  - `GET /ding` — Play bell chime sound
  - `GET /random` — Play random sound clip

---

## 👁️ OLED Display & Animations API

- **Change Eye Emotion**: `GET /eye_mood?val=X`
  - `0`: Normal / Happy
  - `1`: Mad / Angry
  - `2`: Sad
  - `3`: Sleepy
  - `4`: Surprised
- **Display Text Overlay**: `GET /oled_text?msg=Hello+World`
  - *Shows retro text on OLED screen for 4 seconds then reverts to animated eyes.*
- **Display Animations**: `GET /anim?mode=X`
  - `0` (`/anim_eyes`): Animated Eyes
  - `15` (`/anim_mario`): Mario Dance
  - `16` (`/anim_fireworks`): Particle Fireworks
  - `17` (`/anim_matrix`): Matrix Rain
  - `18` (`/anim_invader`): Space Invader
  - `19` (`/anim_heartbeat`): Heartbeat Line
  - `22` (`/show_ip`): Show IP Address
  - `24` (`/big_yawn`): Exaggerated Yawn

---

## 🎮 Retro OLED Games

Trigger interactive games on the OLED screen (controllable via BTN_1 / BTN_2 or Web UI):
- `GET /game_on_h` — Horizontal Pong
- `GET /game_on_v` — Vertical Pong
- `GET /game_flappy` — Flappy Bird
- `GET /game_dino` — Chrome Dino Runner
- `GET /game_snake` — Classic Snake
- `GET /game_pacman` — Pacman
- `GET /game_frogger` — Frogger
- `GET /game_racing` — Car Racing
- `GET /game_math` — Math Quiz
- `GET /game_truth` — Truth Table Generator
- `GET /game_off` — Exit game to Normal Eyes mode

---

## 🐾 Servo & Action API

- **Move Servo to Angle**: `GET /s1_90` (Move Servo 1 to 90°), `GET /s1_0`, `GET /s1_180`
- **Quick Action**: `GET /hi` (Wave arm / leg)
- **LED Control**:
  - `GET /l1on` / `GET /l1off` / `GET /toggle` (Built-in LED)
  - `GET /grnon` / `GET /grnoff` / `GET /grntog` (Green Status LED)
  - `GET /redon` / `GET /redoff` / `GET /redtog` (Red Status LED)

---

## 🕒 Fleet Scheduling API

Schedule an action to trigger across single or multiple SpeakerBot units at an exact wall-clock time or relative delay:
- **Relative Delay**: `GET /schedule?action=hi&delay=5` (Runs `hi` in 5 seconds)
- **TTS Text Schedule**: `GET /schedule?action=tts:Hello&delay=10` (Says "Hello" in 10 seconds)
- **NTP Epoch Sync**: `GET /schedule?action=bark&at=1714000000` (Triggers `bark` at exact Unix epoch)

---

## 🔄 OTA Firmware Updates

- **Upload New Firmware**: `POST /ota`
  - *Header*: `Content-Type: application/octet-stream`
  - *Body*: Raw `.bin` firmware file compiled for ESP32-C3.
  - Returns `{"ota":"ok","restart":true}` and automatically reboots after 1.5s.

---

## 📶 WiFi Provisioning API

- **List Saved Networks**: `GET /wifi`
- **Save Credentials**: `POST /wifi` — Body `{"ssid":"MyWiFi","pass":"MyPassword"}`
- **Scan Visible Networks**: `GET /wifi_scan`
- **Delete Saved Network**: `DELETE /wifi?delete=N`
