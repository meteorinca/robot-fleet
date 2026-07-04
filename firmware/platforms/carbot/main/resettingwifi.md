# Resetting CarBot's WiFi Settings

Use this procedure to erase all saved WiFi networks and return CarBot to hotspot
mode.

---

## When to do this

- You want to remove all the saved wifi networks from CarBot
- CarBot can't connect to any known network and you want to start fresh

---

## What it does

Pressing and holding the **BOOT button** for 7 seconds triggers a reset that:

1. **Erases all saved WiFi passwords** stored on the robot
2. **Restarts** the robot automatically
3. After restart, CarBot broadcasts its own WiFi hotspot (`CarBot-<number>`)
   so the new user can set it up fresh via the web portal

> ⚠️ **This cannot be undone.** All previously saved networks will be gone.
> You will need to re-enter WiFi credentials via the setup portal after the reset.

---

## Step-by-step instructions

### Step 1 — Find the BOOT button

The **BOOT** button is the small built-in button on the ESP32 module itself.
It is labelled **BOOT** or **IO9** on the board silkscreen.

> 💡 This is *not* the same as the user buttons (BTN1 / BTN2) on the robot body.

---

### Step 2 — Hold the BOOT button for 7 seconds

1. Power on CarBot and wait for the eyes to appear on the OLED screen
2. Press **and hold** the BOOT button
3. The OLED will show a countdown:  
   `Hold...6s` → `Hold...5s` → … → `Hold...1s`
4. After **7 seconds**, a confirmation screen appears:

```
FORGET WiFi?         10s
────────────────────────
Erases all saved
networks. Robot
will hotspot.
────────────────────────
[YES] Press btn
[ NO] Hold 7s...
```

---

### Step 3 — Confirm the reset

On the confirmation screen:

| Action | Result |
|---|---|
| **Press BOOT once** | ✅ Confirms reset — erases WiFi, restarts into hotspot mode |
| **Do nothing for 10 s** | ❌ Cancels automatically — no changes made |

> The screen shows a live countdown (`10s`, `9s`, …). If you change your mind,
> just wait and it cancels on its own.

---

### Step 4 — After the reset

Once confirmed:

1. The OLED briefly shows **"Resetting..."**
2. CarBot restarts (takes ~3 seconds)
3. A new WiFi hotspot named **`CarBot-<N>`** appears (where `<N>` is the device
   number printed on the robot)
4. Connect to that hotspot from any phone or laptop — no password needed
5. A setup page will open automatically (captive portal), or navigate to
   **[http://192.168.4.1](http://192.168.4.1)**
6. Enter the new WiFi network name and password and tap **Save**
7. CarBot restarts and connects to the new network 🎉

---

## FAQ

**Q: Will this factory-reset everything else (settings, code)?**  
A: No. Only WiFi credentials are erased. All firmware, behaviour settings, and
other data remain untouched.

**Q: I held the button but nothing happened.**  
A: Make sure you are holding the **BOOT** button (not BTN1 or BTN2). The OLED
countdown hint should appear after ~1 second of holding.

**Q: The confirmation screen appeared but I changed my mind.**  
A: Just let the 10-second timer expire. The screen will return to the normal
animated eyes and nothing is changed.

**Q: Can I re-add my own WiFi back?**  
A: Yes — connect to the hotspot and use the setup portal at 192.168.4.1 to add
your network credentials again.
