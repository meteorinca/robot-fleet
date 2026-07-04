# MyBot API Cheat Sheet

This document lists the available HTTP GET endpoints to control your MyBot via its local web server. You can trigger these directly from your browser, via `curl`, or using Python scripts.

**Base URL**: `http://mybot5.local:80` (Replace `5` with your specific bot's number).

## 💡 LED Control
- **Status LED Turn ON**: `GET /l1on` or `GET /led?state=on`
- **Status LED Turn OFF**: `GET /l1off` or `GET /led?state=off`
- **Status LED Toggle**: `GET /toggle` or `GET /led?state=toggle`

## 🔴🟢 External LED Control
- **Green LED**: `GET /grnon`, `GET /grnoff`, `GET /grntog`
- **Red LED**: `GET /redon`, `GET /redoff`, `GET /redtog`

## 🔧 Servo Control
- **Move to Angle**: `GET /servo?num=1&angle=90` (Hold position)
- **Quick Action (Stepped)**: `GET /s1on` (Moves to ON position at medium speed, then returns to neutral and detaches)
- **Manual Angle (URI style)**: `GET /s1_120` (Sets servo 1 to 120° and holds)
- **Quick Actions list**: `s1on`, `s1off`, `s2on`, `s2off`

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

## 🎵 Buzzer Control
Requires `BUZZER_PIN` to be defined in `board_config.h`.

- **Play Tone**: `GET /tone?f=1000&d=100`
  - *`f`: Frequency in Hz (e.g., 1000) · `d`: Duration in ms (e.g., 100)*
- **Play Demo Melody**: `GET /demo?type=coin`
  - *`type`: 'coin', 'gameover', 'siren', 'laser', 'mario', '1up'*

## ⏱️ System & Status
- **Get Status**: `GET /status`
  - *Returns JSON with version, running status, and time.*
- **Get Time**: `GET /time`
  - *Returns JSON with NTP synced time and epoch.*

---

## 🕹️ Fun OLED Animations (Physical Buttons)

| Button | Behavior |
|--------|----------|
| **BTN_1** (short press, in Normal mode) | Launch **Dancing Mario** — pixelated sprite dances with coin SFX and spinning stars |
| **BTN_2** (short press, in Normal mode) | Cycle through fun animations: Fireworks → Matrix Rain → Space Invaders → Heartbeat → Normal |
| **BTN_2** (hold 3 s) | Open the full **OLED Menu** |

## 🎬 OLED Animations via API

- **Dancing Mario**: `GET /anim_mario`
- **Fireworks**: `GET /anim_fireworks`
- **Matrix Rain**: `GET /anim_matrix`
- **Space Invaders parade**: `GET /anim_invader`
- **Heartbeat/Love**: `GET /anim_heartbeat`
- **Return to Eyes**: `GET /game_off`

---

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://mybot5.local/anim_mario`*
