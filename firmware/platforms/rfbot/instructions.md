# 📡 RFBot 433 MHz Automation & API Relay Instructions

RFBot allows you to receive any 433 MHz (or 315 MHz) radio signal—from photodetectors, door sensors, wireless buttons, or handheld remotes—and automatically trigger **HTTP GET API calls** to other ESP32 devices in your robot fleet (e.g. SpeakerBot, DogBot, CarBot).

> [!IMPORTANT]
> **Relay Mode is ON by Default**  
> As of the latest firmware update, RF Relay mode (`s_relay_enabled = true`) is enabled automatically on boot. No manual setup is needed to start relaying incoming signals.

---

## 🛠️ Step 1: Discover & Capture 433 MHz Signals

Before creating automations, find the unique 433 MHz code for your remote, sensor, or photodetector:

1. Open your browser and go to `http://rfbot5.local` (replace `5` with your RFBot's ID) or `http://<RFBOT_IP>`.
2. Scroll to the **"📡 433 MHz RF Radio"** card.
3. Click **Start Listening** (turns to red `🔴 LIVE`).
4. Trigger your 433 MHz remote button or sensor.
5. Watch the **Signal Log**. Note down the **Code** (decimal or hex).

---

## ⚡ Method 1: Add Unlimited RX Relay Automations in `rf_relay_config.h` (Recommended!)

Just like adding TX buttons with `RF_OUTLET(...)` in [`rf_outlets_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_outlets_config.h), you can add as many RX relay rules as you want in [`rf_relay_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_relay_config.h)!

### Format: `RF_RELAY(decimal_code, "target_host/path")`

Edit [`rf_relay_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_relay_config.h):

```c
// ── RX Relay Rules (Add as many RF_RELAY lines as you want!) ─────────────────

RF_RELAY(123456,  "speakerbot1.local/choola")
RF_RELAY(5577987, "carbot1.local/walk_fwd")
RF_RELAY(5575987, "dogbot2.local/s1on")
RF_RELAY(1381827, "speakerbot5.local/bark")
```

When RFBot receives any of these codes, it automatically sends an HTTP GET request to the specified target host and path. Every matched packet also displays with the `⚡ RELAYED` badge in the Web UI signal log!

---

## 🎛️ Method 2: Change Targets Dynamically via Web UI / HTTP API

You can change the primary target host/endpoint or enable/disable relaying **on the fly** without reflashing the board!

### Via Web UI
1. Navigate to `http://rfbot5.local`.
2. Locate the **Photodetector RF Relay** card.
3. Enter the **Target SpeakerBot / ESP32 Host & Path** (e.g., `speakerbot1.local/choola`).
4. Click **Save Relay Settings**.
5. Click **⚡ Test Bark** to test the API endpoint instantly.

### Via HTTP API GET Request
```bash
# Enable relay and point to a different ESP32
curl "http://rfbot5.local/rf/relay?enabled=1&host=speakerbot1.local%2Fchoola"

# Disable relay
curl "http://rfbot5.local/rf/relay?enabled=0"

# Test trigger
curl "http://rfbot5.local/rf/relay/test"
```

---

## 🐍 Method 3: External Automation via Python Daemon (No Reflashing Required!)

If you prefer to manage complex multi-robot automations using Python or Home Assistant:

```python
import requests, time

RFBOT_IP = "http://rfbot5.local"

AUTOMATION_MAP = {
    "1E240":  "http://speakerbot1.local/choola",
    "553F4C": "http://carbot1.local/walk_fwd",
    "553F4D": "http://dogbot2.local/s1on",
}

def main():
    print("🤖 Starting 433 MHz RF Automation Daemon...")
    requests.get(f"{RFBOT_IP}/rf/listen/start")

    while True:
        try:
            r = requests.get(f"{RFBOT_IP}/rf/poll", timeout=2).json()
            for pkt in r.get("packets", []):
                code_hex = pkt.get("code")
                if code_hex in AUTOMATION_MAP:
                    target_url = AUTOMATION_MAP[code_hex]
                    print(f"🚀 Triggering Automation -> GET {target_url}")
                    requests.get(target_url, timeout=3)
        except Exception as e:
            print(f"Polling error: {e}")
        time.sleep(0.5)

if __name__ == "__main__":
    main()
```

---

## 📌 Summary Cheat Sheet

| Action | How to do it |
| :--- | :--- |
| **Add RX Relay Rules** | Add `RF_RELAY(code, "target/path")` lines in [`rf_relay_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_relay_config.h) |
| **Add TX Outlet Buttons** | Add `RF_OUTLET("Label", on_code, off_code)` lines in [`rf_outlets_config.h`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/rf_outlets_config.h) |
| **Capture RF Codes** | Web UI `http://rfbotX.local` -> Click **Start Listening** |
| **Change Target URL on the fly** | `GET /rf/relay?enabled=1&host=targetbot.local/endpoint` |
| **Test Endpoint** | `GET /rf/relay/test` or click **⚡ Test Bark** in Web UI |
