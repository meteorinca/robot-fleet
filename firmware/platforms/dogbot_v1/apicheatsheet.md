# MojDog v1 API Cheat Sheet

This document lists the available HTTP GET endpoints to control your MojDog via its local web server. You can trigger these directly from your browser, via `curl`, or using Python scripts.

**Base URL**: `http://dogbot3.local:81` (Replace `dogbot3` with your specific bot's hostname or IP address).

## 🗣️ Voice & Audio API
- **Say a phrase**: `GET /tts?say=My%20name%20is%20Paulbot`
  - *Forces the bot to fetch the audio and speak it out loud.*
- **Play dog bark clip**: `GET /bark`

## 👁️ OLED Display API
- **Change Eye Mood**: `GET /eye_mood?val=X`
  - `0`: Happy (Full Eyeball - Default)
  - `1`: Angry
  - `2`: Neutral
  - `3`: Sad
- **Display Text**: `GET /oled_text?msg=Hello+World`
  - *Displays the green 5x7 retro text on the OLED screen for 3 seconds, then automatically reverts to the animated eyes.*

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

*Tip: You can test any of these by just typing them into your browser's address bar! Example: `http://dogbot3.local:81/schedule?action=bark&delay=3`*
