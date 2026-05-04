# mojDogv1 — How It Works

> An ESP32-C3 robot dog with procedural dual-eye animation, browser-based TTS, and fully non-blocking controls — all on a single-core 160MHz RISC-V chip.

---

## Architecture Overview

The system runs 6 concurrent FreeRTOS tasks on a single core, carefully prioritized so the web server and audio always preempt the eye animation renderer.

```
Browser                          ESP32-C3
┌──────────────┐                ┌─────────────────────────────────┐
│  Web UI :81  │───GET /servo──▶│  httpd task (prio 5)            │
│              │───GET /led────▶│    → instant response always    │
│              │                │                                 │
│  Google TTS  │                │  audio_feed task (prio 4)       │
│  + AudioCtx  │───POST /audio─▶│    → drains payload queue       │
│  16kHz PCM   │   (async!)     │    → feeds ring buffer          │
└──────────────┘                │                                 │
                                │  dog_audio task (prio 5)        │
                                │    → ring buffer → I2S PDM      │
                                │                                 │
                                │  dog_eyes task (prio 2)         │
                                │    → 160x80 OLED @ ~16 FPS      │
                                │                                 │
                                │  button_task (prio 5)           │
                                │    → mood/blink via ISR queue   │
                                └─────────────────────────────────┘
```

---

## 1. Dual-Eye Animation System

**File:** `main/dog_peripherals.c` → `dog_eyes_task()`

### How Two Eyes Fit on 160×80

Each eye is an ellipse centered at **x=40** (left) and **x=120** (right), both at **y=40**:

```
┌────────────────────────────────────────┐
│         ┌──────┐    ┌──────┐          │ 80px
│        (  LEFT  )  (  RIGHT )         │
│         └──────┘    └──────┘          │
└────────────────────────────────────────┘
                  160px
     cx=40              cx=120
     rx=34              rx=34
```

### Rendering Pipeline (per pixel)

1. **Spatial early-reject** — Pixels in the gap (x 75–85) and far edges skip entirely (~20% savings)
2. **Ellipse test** — `dx²·ry² + dy²·rx²  ≤  rx²·ry²` (pure `int` math, max ~2.4M)
3. **Eyelid cutoff** — Linear function of `inner_dx` (mirrored per eye) clips top of ellipse
4. **Sclera** — White center with warm-tinted edges
5. **Iris** — 4-zone radial gradient: dark limbal ring → teal → aqua-green → bright aqua
6. **Speckle** — Fast xorshift PRNG: amber flecks (~6%) + dark flecks (~3%)
7. **Pupil** — Black circle with ±1px size oscillation for "breathing" effect
8. **Catchlights** — Two specular highlights per eye, mirrored left/right

### Moods

| Mood | `eye_mood` | Eyelid |
|------|-----------|--------|
| Angry | 0 | Slants **down** toward nose bridge |
| Neutral | 1 | Flat horizontal |
| Sad | 2 | Slants **up** toward nose bridge |

Cycled by pressing the BOOT button.

### Eye Wander & Blinking

- **Gaze**: Shared pupil target shifts randomly ~every second, smoothly interpolated 1px/frame
- **Blink**: Random intervals (2.5–9s) + force-blink via button; squashes ry to 3px for 3 frames

---

## 2. Text-to-Speech Pipeline

### Browser Side (JavaScript)

```
User types text
    → Google Translate TTS URL (MP3)
    → fetch() via CORS proxy
    → AudioContext.decodeAudioData()
    → OfflineAudioContext (resample to 16kHz mono)
    → Int16Array PCM
    → POST /audio to ESP32
```

The browser does ALL heavy lifting: MP3 decoding, resampling, PCM conversion. The ESP32 just plays raw samples.

### ESP32 Side (Streaming Chunks, Non-Blocking)

```
POST /audio arrives (any size)
    → httpd reads body in 4KB chunks
    → each chunk: malloc(4KB), read, enqueue to payload_queue (depth 16)
    → if queue full: blocks up to 500ms (feeder is draining)
    → responds "OK" after all chunks enqueued

audio_feed task (background, prio 4):
    → dequeues chunks one by one
    → feeds ring buffer in 1KB sub-chunks
    → frees each chunk after use

dog_audio task (prio 5):
    → reads from 16KB ring buffer
    → writes to I2S PDM → speaker
```

No size cap, no large allocation. A 10-second sentence (320KB PCM) streams through as 80 × 4KB chunks.

---

## 3. Why This Architecture Matters (Single-Core Scheduling)

The ESP32-C3 has **one** RISC-V core. Every task shares the same core via FreeRTOS preemptive scheduling.

### Task Priority Map

| Priority | Task | Why |
|----------|------|-----|
| 5 | httpd, dog_audio, button | Must respond instantly |
| 4 | audio_feed | Important but can yield to httpd |
| 2 | dog_eyes | Lowest — renders with leftover CPU |

### Key Design Decisions

1. **Eyes at priority 2**: The renderer computes 12,800 pixels/frame. At priority 5, it starved the web server. At priority 2, httpd/audio always preempt it.

2. **`vTaskDelay(1)` not `taskYIELD()`**: `taskYIELD()` only yields to equal-or-higher priority tasks. `vTaskDelay(1)` truly sleeps, letting the scheduler run anything.

3. **No `long long`**: 64-bit multiply on a 32-bit RISC-V compiles to ~4 instructions of software emulation. The ellipse math max value is ~2.4M (well within int32's 2.1B limit), so plain `int` works fine.

4. **Async audio POST**: The old synchronous handler blocked httpd for the entire audio duration. Now it returns instantly, freeing the server for servo/LED requests.

---

## 4. Capabilities

| Feature | Details |
|---------|---------|
| **Display** | 160×80 ST7789 SPI OLED, 16-bit RGB565, ~16 FPS |
| **Eyes** | Dual procedural ellipses, gradient iris, speckle texture, catchlights, pupil dilation |
| **Moods** | 3 expressions (angry/neutral/sad) via button |
| **TTS** | Browser-side Google TTS → 16kHz PCM → async POST → I2S PDM |
| **Audio** | 16KB ring buffer, async feeder, non-blocking HTTP |
| **Servos** | Up to 4 with quick-action patterns (hi, lay, stand, walk) |
| **LED** | WS2812 NeoPixel |
| **Buttons** | 3 GPIO with ISR + debounce queue |
| **Scheduling** | Named actions by delay or epoch time |
| **OTA** | Over-the-air firmware updates |
| **mDNS** | `dogbot1.local:81` |

---

## File Map

| File | Purpose |
|------|---------|
| `main/dog_peripherals.c` | Eyes, audio, mic, buttons |
| `main/webserver.c` | HTTP handlers, TTS endpoint, web UI |
| `main/config.h` | Pin definitions, board config |
| `main/servo.c` | LEDC PWM servo driver |
| `main/ws2812.c` | NeoPixel LED driver |
| `main/wifi_mgr.c` | Wi-Fi + mDNS |
| `main/timekeep.c` | NTP sync + scheduler |
