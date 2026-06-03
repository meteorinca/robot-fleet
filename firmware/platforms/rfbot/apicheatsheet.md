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
  - *Returns and clears all buffered packets accumulated since the last poll.*
  - *Returns*: `{"listening":true,"packets":[{"code":"1A2B3C","bits":24,"proto":1,"pulse":350}, ...]}`
  - *Packets field is empty array `[]` when nothing has been received since last poll.*
  - **WebUI**: The **"📡 433 MHz RF Radio"** card has a **Start Listening** button that toggles listen mode and automatically polls `/rf/poll` every 800 ms, displaying all received signals in a live signal log.

### Status
- **Get RF module state**: `GET /rf/status`
  - *Returns*: `{"listening":false,"learning":false,"tx_gpio":3,"rx_gpio":4}`

### Transmitting (Send)
- **Send a code**: `GET /rf/send?code=<hex>&bits=<n>&proto=<p>&pulse=<us>`
  - *Transmits a 433 MHz OOK signal using the RC-switch protocol.*
  - `code` — hex value, e.g. `1A2B3C` (**required**)
  - `bits` — number of bits to send (default: `24`)
  - `proto` — RC-Switch protocol number 1–12 (default: `1`)
  - `pulse` — base pulse length in µs (default: `350`)
  - *Returns*: `{"ok":true,"code":"1A2B3C","bits":24,"proto":1,"pulse":350}`
  - **Example**: `GET /rf/send?code=1A2B3C&bits=24&proto=1&pulse=350`

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
- **Get Status**: `GET /status`
  - *Returns JSON with version, running status, and time.*
- **Get Time**: `GET /time`
  - *Returns JSON with NTP synced time and epoch.*

---

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://rfbot5.local/schedule?action=toggle&delay=3`*
