# PaulBot v1 API Cheat Sheet

This document lists the available HTTP GET endpoints to control your PaulBot via its local web server. You can trigger these directly from your browser, via `curl`, or using Python scripts.

**Base URL**: `http://paulbot3.local` (Replace `paulbot3` with your specific bot's hostname or IP address).

## 🗣️ Voice & Audio API
- **Say a phrase**: `GET /tts?say=My%20name%20is%20Paulbot`
  - *Forces the bot to fetch the audio and speak it out loud.*
- **Play dog bark clip**: `GET /bark`
- **Play "Huh?"**: `GET /huh`
- **Play "Yes"**: `GET /yes`
- **Play "Jump"**: `GET /jump`
- **Play "Ding"**: `GET /ding`
- **Play Random Sound**: `GET /random`

## 👁️ OLED Display API
- **Change Eye Mood**: `GET /eye_mood?val=X`
  - `0`: Happy (Full Eyeball - Default)
  - `1`: Sad
  - `2`: Neutral
  - `3`: Angry
- **Display Text**: `GET /oled_text?msg=Hello+World`
  - *Displays white 5x7 retro text on the OLED screen for 4 seconds, then automatically reverts to the animated eyes.*

## 🎆 Display Animations API
- **Direct mode endpoint**: `GET /anim?mode=X`
  - `0`: 👁 Eyes (default — animated eyes with pupils)
  - `1`: 🎆 Fireworks (colorful particle bursts)
  - `2`: 🟢 Matrix Rain (falling green characters)
  - `3`: ❤️ Heartbeat (artsy continuous line pulse)
- **Named action aliases** (work with `/schedule` and `/paulbot`):
  - `GET /anim_eyes` — switch to eyes mode
  - `GET /anim_fireworks` — switch to fireworks
  - `GET /anim_matrix` — switch to matrix rain
  - `GET /anim_heartbeat` — switch to heartbeat

> **Note**: Fireworks also auto-play for 4 seconds whenever someone loads the WebUI (a welcome animation). All other modes are permanent until changed.

## 🐾 Movement & Servos
- **Move exact servo angle**: 
  - `GET /s1_90` (Moves Servo 1 to 90 degrees)
  - `GET /s2_180` (Moves Servo 2 to 180 degrees)
  - `GET /s3_0`, `GET /s4_45`, etc.
- **Pre-defined Actions**:
  - `GET /hi` (Waves a single leg)
  - `GET /wiggle` (Wiggles back and forth)
  - `GET /stand` (Resets to neutral standing position)
  - `GET /lay` or `GET /lie` (Lays down)
  - `GET /bow` (Bows down)
  - `GET /lean` (Leans back)
  - `GET /rock` (Rocks side to side)
  - `GET /sway` (Sways body)
  - `GET /shake` (Shakes hand)
  - `GET /poke` (Poke reaction)
  - `GET /kick` (Kicks back legs)
  - `GET /jump_fwd` or `GET /jumpfwd` (Jumps forward)
  - `GET /jump_bwd` or `GET /jumpbck` (Jumps backward)
  - `GET /walk_fwd` (Takes a step forward)
  - `GET /walk_bwd` (Takes a step backward)

## 🕒 Scheduling (For Synchronized Dances!)
You can queue an action to happen at an exact Unix timestamp across all bots simultaneously, or after a relative delay.

- **Run after delay**: `GET /schedule?action=wiggle&delay=5`
  - *Runs the `wiggle` action exactly 5 seconds from now.*
- **Run at exact time**: `GET /schedule?action=hi&at=1714000000`
  - *Runs the `hi` action when the bot's NTP-synced clock hits the specified Unix epoch timestamp.*
- **Schedule text-to-speech**: `GET /schedule?action=tts:Hello&delay=10`
  - *Says "Hello" in 10 seconds.*

## 💡 LED Control
- **Turn ON**: `GET /l1on` or `GET /led?state=on`
- **Turn OFF**: `GET /l1off` or `GET /led?state=off`
- **Toggle**: `GET /toggle` or `GET /led?state=toggle`

---

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://paulbot3.local:81/schedule?action=bark&delay=3`*
