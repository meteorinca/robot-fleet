# Resetting RFBot's WiFi Settings

Use this procedure to erase all saved WiFi networks and return RFBot to hotspot mode.

---

## When to do this

- You want to remove all the saved WiFi networks from RFBot.
- RFBot can't connect to any known network and you want to start fresh.

---

## What it does

Pressing and holding the **BOOT button** for 7 seconds triggers a reset that:

1. **Erases all saved WiFi passwords** stored on the robot in NVS.
2. **Restarts** the robot automatically.
3. After restart, RFBot broadcasts its own WiFi hotspot (`RFBot-<number>`) so you can set it up fresh via the web portal.

> ⚠️ **This cannot be undone.** All previously saved networks will be erased.
> You will need to re-enter WiFi credentials via the setup portal after the reset.

---

## Step-by-step instructions

### Step 1 — Find the BOOT button

The **BOOT** button is the small built-in button on the ESP32 module itself.
It is labelled **BOOT** or **IO9** on the board silkscreen.

---

### Step 2 — Hold the BOOT button for 7 seconds

1. Power on RFBot.
2. Press **and hold** the BOOT button for 7 seconds.
3. The board status LED will blink during the countdown (or if an OLED display is connected, the screen displays `Hold to Reset: X`).

---

### Step 3 — Confirm the reset

After 7 seconds, release the button. You have 5 seconds to confirm:

- **Triple-click the BOOT button** to confirm.
- The status LED flashes rapidly to indicate confirmation.

---

### Step 4 — After the reset

Once confirmed:

1. RFBot restarts (takes ~3 seconds).
2. A new WiFi hotspot named **`RFBot-<N>`** appears (where `<N>` is the device number).
3. Connect to that hotspot from any phone or laptop — no password needed.
4. A setup page will open automatically (captive portal), or navigate to **[http://192.168.4.1](http://192.168.4.1)**.
5. Enter the new WiFi network name and password and tap **Connect & Reboot**.
6. RFBot restarts and connects to the new network 🎉
