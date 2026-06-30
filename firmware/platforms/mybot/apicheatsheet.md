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

## 🌈 NeoPixel Strip Control (WS2812 — 10 pixels)
Requires `WS2812_DATA_GPIO` and `WS2812_NUM_LEDS` to be defined in `board_config.h`.

- **Set one pixel**: `GET /neopixel?pixel=N&r=R&g=G&b=B`
  - *`pixel`: 0–9 · `r`,`g`,`b`: 0–255 · Sets pixel N to the given RGB color.*
- **Set all pixels**: `GET /neopixel_all?r=R&g=G&b=B`
  - *Sets every pixel on the strip to a single color.*
- **Clear all pixels**: `GET /neopixel_clear`
  - *Turns every pixel off (all black).*

## ⏱️ System & Status
- **Get Status**: `GET /status`
  - *Returns JSON with version, running status, and time.*
- **Get Time**: `GET /time`
  - *Returns JSON with NTP synced time and epoch.*

---

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://mybot5.local/schedule?action=toggle&delay=3`*
