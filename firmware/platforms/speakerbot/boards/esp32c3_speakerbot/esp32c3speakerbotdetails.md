# ESP32-C3 SpeakerBot

## SpeakerBot v1.0

SpeakerBot is MyBot upgraded with an I2S PDM speaker. It runs on a breadboard with amazing animated OLED eyes, ultrasonic distance sensing, a single SG90 servo, and now full streaming audio playback. It comes built-in with preprogrammed demos such as:

- 🎵 Streaming TTS audio via browser Web Speech API
- 🐾 Bark audio clip
- 🤖 Paulbot boot jingle
- 🔔 Mario / coin / siren buzzer demos
- 👁️ Animated OLED eye emotions (5 moods)
- 🎮 Pong, Snake, Frogger, Flappy Bird, Dino, Pacman games
- 📡 Ultrasonic distance view
- 🌐 Full WiFi provisioning via captive portal
- 🔄 OTA firmware updates via web UI or Python

## SpeakerBot Hardware

| Peripheral         | GPIO         | Notes                              |
|--------------------|--------------|------------------------------------|
| Built-in LED       | GPIO 8       | Active LOW                         |
| Buzzer (passive)   | GPIO 3       | LEDC PWM, Mario / coin demos       |
| Servo (SG90)       | GPIO 5       | LEDC Channel 0                     |
| OLED SDA           | GPIO 7       | I2C, SSD1306 128×64                |
| OLED SCL           | GPIO 6       | I2C                                |
| Ultrasonic TRIG    | GPIO 10      | HC-SR04                            |
| Ultrasonic ECHO    | GPIO 4       | HC-SR04                            |
| Green LED          | GPIO 20      | Breadboard status LED              |
| Red LED            | GPIO 21      | Breadboard status LED              |
| Button 1           | GPIO 0       | Eye emotion cycling                |
| Button 2           | GPIO 1       | Animation cycling                  |
| Boot Button        | GPIO 9       | 7-second hold to reset WiFi        |
| **Speaker PDM TX** | **GPIO 1**   | **I2S PDM Data — connect to speaker module** |
| **Speaker PDM CLK**| **GPIO 2**   | **I2S PDM Clock**                  |
| **Amp Enable**     | **GPIO 18**  | **Amplifier enable (HIGH = on)**   |

> **Note**: GPIO 1 is shared between Button 2 and AUDIO_DATA_GPIO on the breadboard.
> When using the standalone speaker module, wire AUDIO_DATA to its own pin on the module
> and keep BTN_2 connected to a separate GPIO if needed. In the default config, BTN_2
> is wired to the breadboard header but the speaker module receives the I2S PDM signal.

## 🛠️ Build & Flash Guide

Build using `idf.py` with board name, device number, and optional ultrasonic sensor configuration:

```bash
# Standard build (Ultrasonic disabled by default):
idf.py -DBOARD=esp32c3_speakerbot -DDEVICE_NUMBER=8 build

# Build with Ultrasonic (HC-SR04) enabled:
idf.py -DBOARD=esp32c3_speakerbot -DDEVICE_NUMBER=8 -DENABLE_ULTRASONIC=1 build
```

Passing `-DDEVICE_NUMBER=8` generates hostname `speakerbot8.local` and hotspot `SpeakerBot-8`.

## SpeakerBot Connection Guide


### Network
- **Hostname**: `speakerbot<N>.local` (mDNS, replace `<N>` with device number)
- **AP Mode**: `SpeakerBot-<N>` (open WiFi hotspot, IP: `192.168.4.1`)

### WiFi Reset Procedure
Hold the **BOOT button** (GPIO 9) for **7 seconds** → OLED shows countdown → release and **triple-click** to confirm → all WiFi credentials erased, device reboots to hotspot.

### OTA Update
POST the `.bin` firmware binary to `http://speakerbot<N>.local/ota`.

### Audio Streaming
Use the Web UI's Speaker section to type text and stream it via the browser's Web Speech API directly to the `/audio` endpoint. The ESP32 receives raw PCM chunks and plays them through the I2S PDM speaker.
