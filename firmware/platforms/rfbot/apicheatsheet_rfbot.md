# RF Bot API Cheat Sheet

This document lists the available HTTP GET endpoints to control your RF Bot via its local web server. You can trigger these directly from your browser, via `curl`, or using Python scripts.

- **Command line**: idf.py -D BOARD=esp32c3_rfbot -D DEVICE_NUMBER=5 set-target esp32c3 build
- **Web UI**: http://rfbot5.local

**Base URL**: `http://rfbot5.local:80` (Replace `5` with your specific bot's number).

## 💡 LED Control
- **Turn ON**: `GET /l1on` or `GET /led?state=on`
- **Turn OFF**: `GET /l1off` or `GET /led?state=off`
- **Toggle**: `GET /toggle` or `GET /led?state=toggle`

## 🔧 Servo Control
- **Move to Angle**: `GET /servo?num=1&angle=90` (Hold position)
- **Quick Action (Stepped)**: `GET /s1on` (Moves to ON position at medium speed, then returns to neutral and detaches)
- **Manual Angle (URI style)**: `GET /s1_120` (Sets servo 1 to 120° and holds)
- **Quick Actions list**: `s1on`, `s1off`, `s2on`, `s2off`

## 📡 433 MHz RF Radio

### Listening (Receive)
- **Start listening**: `GET /rf/listen/start`
  - *Enables listen mode — the RX task begins buffering every received signal.*
  - *Returns*: `{"ok":true,"listen":"started"}`
- **Stop listening**: `GET /rf/listen/stop`
  - *Disables listen mode. Normal action dispatch resumes.*
  - *Returns*: `{"ok":true,"listen":"stopped"}`
- **Poll for received packets**: `GET /rf/poll`
  - *Returns and clears buffered decoded packets, including when listen mode and the local relay are disabled.*
  - *The ring holds 32 packets and overwrites the oldest when full. Use one polling consumer; simultaneous WebUI and FleetHub polling share this queue.*
  - *Repeated frames of the same code, bit length, and protocol within 250 ms are suppressed.*
  - *`edges` now estimates captured RMT pulse halves, not raw GPIO interrupts; `edges_estimated` is `true`.*
  - *Returns*: `{"listening":true,"packets":[{"code":"1A2B3C","bits":24,"proto":1,"pulse":350}, ...]}`
  - *Returns*: `{"listening":true,"packets":[{"code":"1A2B3C","bits":24,"proto":1,"pulse":350, "relayed": true}, ...]}`
  - *Packets field is empty array `[]` when nothing has been received since last poll.*
  - **WebUI**: The **"📡 433 MHz RF Radio"** card has a **Start Listening** button that toggles listen mode and automatically polls `/rf/poll` every 800 ms, displaying all received signals in a live signal log.

### Photodetector RF Relay
- **How it works**: **Disabled by default.** Enable only when using the ESP32 as a fallback bridge; FleetHub can consume the decoded events instead. When active, receiving RF code `123456` (which displays as `0x1E240` in hex in the signal log) automatically sends an HTTP GET request to the target SpeakerBot URL/endpoint configured (e.g. `speakerbot1.local/bark`, `speakerbot5.local/bark`, or `speakerbot12.local/audio`).
- **Automations Guide**: See [`instructions.md`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/instructions.md) for full guide on adding automations for different 433 MHz commands.
- **Configuration header**: Edit [`rf_relay_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_relay_config.h) to add as many `RF_RELAY(decimal_code, "target/path")` RX rules as you want.
- **Get/Set Relay Config**: `GET /rf/relay` or `GET /rf/relay?enabled=1&host=speakerbot1.local%2Fchoola`
  - Returns JSON: `{"enabled": true, "host": "speakerbot1.local/choola", "last_event": "✓ Sent GET /choola -> 10.0.0.15"}`
- **Test Relay Endpoint**: `GET /rf/relay/test` (Triggers test relay task manually)
- **Live Signal Stream**: `GET /rf/poll` includes `"relayed": true` flag for any matched relay RF packets.

### Status
- **Get RF module state**: `GET /rf/status`
  - *Returns*: `{"listening":false,"learning":false,"tx_gpio":3,"rx_gpio":4}`

### Transmitting (Send)
- **Send a code**: `GET /rf/send?code=<code>&bits=<n>&proto=<p>&pulse=<us>`
  - *Transmits a 433 MHz OOK signal using hardware RMT and the RC-switch protocol. Ten repeats run in hardware while the caller sleeps; RX is paused during TX to suppress self-echo.*
  - *Invalid bit lengths, protocols, or pulse timings return HTTP 400. The longest individual pulse must fit the RMT 32767 us limit.*
  - `code` — **decimal** (e.g. `5584140`) **or** `0x`-prefixed hex (e.g. `0x553F4C`) (**required**)
    - Pure hex letters with no prefix (e.g. `1A2B3C`) also work — auto-detected.
  - `bits` — number of bits to send (default: `24`)
  - `proto` — RC-Switch protocol number 1–12 (default: `1`)
  - `pulse` — base pulse length in µs (default: `185`)
  - *Returns*: `{"ok":true,"code":"5584140","sent_hex":"553F4C","bits":24,"proto":1,"pulse":185}`
    - **`sent_hex`** — actual hex value transmitted (use this to verify decimal input was parsed correctly)
  - **Example**: `GET /rf/send?code=5584140&bits=24&proto=1&pulse=185`

### RF Outlets (Web UI quick-buttons)
The **🔌 RF Outlets** card in the Web UI provides one-tap ON/OFF buttons for named outlets.
All outlet buttons always use: **24 bits, protocol 1, 185 µs pulse**.

| Outlet  | ON code  | OFF code |
|---------|----------|----------|
| Alpha   | 5576451  | 5576460  |
| Bravo   | 5584131  | 5584140  |
| Foxtrot | 1381827  | 1381836  |

**To add/remove outlets**: edit [`rf_outlets_config.h`](file:///c:\Users\dontm\Documents\mojCodexstuff\ActiveGithub\robot-fleet\firmware\platforms\rfbot\main\rf_outlets_config.h) and reflash — no other file changes needed.
Format: `RF_OUTLET("Label", decimal_on_code, decimal_off_code)`



### Python examples
```python
import requests

BASE = "http://rfbot5.local"

# Listen for 10 seconds and print all received signals
requests.get(f"{BASE}/rf/listen/start")
import time; time.sleep(10)
r = requests.get(f"{BASE}/rf/poll")
for pkt in r.json()["packets"]:
    print(f"0x{pkt['code']}  {pkt['bits']}b  proto={pkt['proto']}  pulse={pkt['pulse']}µs")
requests.get(f"{BASE}/rf/listen/stop")

# Send a code
requests.get(f"{BASE}/rf/send?code=1A2B3C&bits=24&proto=1&pulse=350")
```

## 🕒 Scheduling (For Synchronized Events!)
You can queue an action to happen at an exact Unix timestamp across all bots simultaneously, or after a relative delay.

- **Run after delay**: `GET /schedule?action=toggle&delay=5`
  - *Runs the `toggle` LED action exactly 5 seconds from now.*
- **Run at exact time**: `GET /schedule?action=l1on&at=1714000000`
  - *Runs the `l1on` action when the bot's NTP-synced clock hits the specified Unix epoch timestamp.*
- **Schedule text-to-speech**: `GET /schedule?action=tts:Hello&delay=10`
  - *Broadcasts "Hello" to SSE clients in 10 seconds.*

## 🗣️ Server-Sent Events (SSE)
- **Push text to connected browsers**: `GET /sendtts:Hello%20World` or `GET /tts?say=Hello%20World`
  - *Broadcasts the text to any browser listening on `/events`.*

## ⏱️ System & Status
- **Get Status / Telemetry**: `GET /status` or `GET /api/info` (or `/info`)
  - *Returns unified JSON with platform, firmware version, hostname, device_number, uptime, free_heap, epoch, and NTP time_synced.*
- **Get Time**: `GET /time`
  - *Returns JSON with NTP synced time and epoch.*

---

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://rfbot5.local/schedule?action=toggle&delay=3`*

---

## 🔌 Hardware Wiring Guide (ESP32-C3 RFBot)

| Component | Component Pin | ESP32-C3 SuperMini Pin | Notes |
| :--- | :--- | :--- | :--- |
| **Built-in LED** | Anode/Cathode | `GPIO 8` | On-board LED (Active LOW) |
| **RF Transmitter** | DATA | `GPIO 4` (433MHz) / `GPIO 3` (315MHz) | RF TX Signal (RMT PWM) |
| | VCC | `5V` / `3.3V` | Power supply |
| | GND | `GND` | Common Ground |
| **RF Receiver (SRX882 V2.0)** | DATA | `GPIO 10` | RF RX Signal (hardware RMT capture) |
| | **CS (Enable)** | **`3.3V` / `5V`** | **CRITICAL: CS must be tied to VCC/HIGH. If left floating or GND, SRX882 sleeps and receives no signals.** |
| | VCC | `3.3V` / `5V` | Power supply |
| | GND | `GND` | Common Ground |
| | ANT | 17cm wire (433MHz) / 23.8cm (315MHz) | Antenna pad |
| **Servo 1** | Signal (Yellow/Orange) | `GPIO 5` | PWM Channel 0 |
| | VCC (Red) | `5V` | External Power / 5V |
| | GND (Brown/Black) | `GND` | Common Ground |
| **Servo 2** | Signal (Yellow/Orange) | `GPIO 6` | PWM Channel 1 |
| | VCC (Red) | `5V` | External Power / 5V |
| | GND (Brown/Black) | `GND` | Common Ground |
| **User Button 1** | Pin 1 | `GPIO 0` | User Input Button 1 |
| **User Button 2** | Pin 1 | `GPIO 1` | User Input Button 2 |
| **Boot Button** | Pin 1 | `GPIO 9` | On-board BOOT button |

### ⚠️ Important Hardware Notes for SRX882 / SRX882 V2.0 Receivers:
1. **CS Pin (Chip Select / Sleep Mode):** Standard cheap RF modules (e.g. XY-MK-5V) have 4 pins and no CS pin. SRX882 has 5 pins (`ANT`, `GND`, `DATA`, `CS`, `VCC`). **If `CS` is left floating or connected to GND, the SRX882 remains in sleep mode (~0.1 µA) and will output NO results.** Always wire `CS` directly to `3.3V` or `5V`.
2. **Pin Layout Order:** Do not plug SRX882 directly into standard 4-pin receiver sockets. Verify pin ordering on the back silkscreen.
3. **Frequency Variants:** Superheterodyne SRX882 modules have narrow bandpass filters. Ensure your transmitter frequency (433.92 MHz vs 315 MHz) matches your hardware variant (`SRX882-433` or `SRX882-315`).
4. **Antenna:** Solder a straight copper wire to `ANT` (~17.0 cm for 433 MHz, ~23.8 cm for 315 MHz) for full sensitivity.




## RF capture and FleetHub

RMT captures pulse durations on the ESP32. A normal task performs bounded,
sync-based decoding and exposes compact code events through `/rf/poll`.
FleetHub can evaluate automation rules from these events without enabling the
ESP32 relay. No Go bridge rule engine is added by this firmware change.

Raw pulse streaming to FleetHub would still require hardware capture on the
ESP32 and add network traffic and network-dependent latency. Local decoding
keeps capture independent of Wi-Fi. The first partial frame is discarded;
repeated remote frames provide the complete sync-bounded packet.

The firmware retains the 160 MHz CPU setting. The LED uses an integer curve at
25 Hz and only changes PWM duty when needed. Startup Wi-Fi diagnostic scanning
and the forced fallback TX-power override have been removed.
