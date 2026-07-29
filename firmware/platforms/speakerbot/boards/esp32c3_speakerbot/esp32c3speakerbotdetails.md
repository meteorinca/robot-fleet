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

## SpeakerBot Hardware (ESP32-C3 SuperMini Pinout)

| Peripheral              | SuperMini GPIO | Connect To / Notes                 |
|-------------------------|----------------|------------------------------------|
| **MAX98357A DIN**       | **GPIO 1**     | **I2S DIN (Serial Data)**          |
| **MAX98357A BCLK**      | **GPIO 2**     | **I2S BCLK (Bit Clock)**           |
| **MAX98357A LRC (WS)**  | **GPIO 3**     | **I2S LRC / WS (Left-Right Clock)**|
| **MAX98357A SD_MODE**   | **GPIO 0**     | **SD pin (or tie to 3.3V/5V on breadboard)** |
| Built-in LED            | GPIO 8         | On-board status LED (Active LOW)   |
| Boot / Reset Button     | GPIO 9         | On-board BOOT button (Hold 7s for WiFi reset) |
| Servo (SG90)            | GPIO 5         | LEDC PWM Channel 0                 |
| OLED SDA                | GPIO 7         | SSD1306 OLED SDA                   |
| OLED SCL                | GPIO 6         | SSD1306 OLED SCL                   |
| Ultrasonic TRIG (Opt)   | GPIO 10        | HC-SR04 TRIG (via `-DENABLE_ULTRASONIC=1`) |
| Ultrasonic ECHO (Opt)   | GPIO 4         | HC-SR04 ECHO                       |
| Green LED (Opt)         | GPIO 20        | Breadboard status LED              |
| Red LED (Opt)           | GPIO 21        | Breadboard status LED              |

> **Resolution of Pin Conflicts**:
> 1. **Buttons 1 & 2 have been REMOVED completely.** GPIO 1 is now 100% dedicated to **MAX98357A DIN**.
> 2. **LRC / WS Pin**: Connect **MAX98357A LRC (or WS)** pin to **GPIO 3**.
> 3. **SD Pin**: Connect **MAX98357A SD** pin to **GPIO 0** (or simply tie it to 3.3V/5V power rail on your breadboard to keep the amp enabled).
> 4. **No GPIO 18 needed**: ESP32-C3 SuperMini does not break out GPIO 18; GPIO 0 is used for software amp shutdown control.



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
