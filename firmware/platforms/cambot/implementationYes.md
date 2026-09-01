# 🚀 CamBot Diagnostic & Bottleneck Analysis System

This document outlines the peer cross-testing and diagnostic framework created for **cambot1.local** (OV2640) and **cambot3.local** (OV3660) to measure, isolate, and solve camera streaming unresponsiveness.

---

## 🎯 The Core Problem & The 5 Suspects

When the camera stream is turned **OFF**, buttons, motors, and LEDs are instant and responsive. When the camera stream is turned **ON**, control latency spikes. 

To achieve **low ping, high FPS, and crisp quality**, we isolate each layer of the pipeline:

```
┌─────────────────┐       ┌─────────────────┐       ┌─────────────────┐       ┌─────────────────┐
│   1. SENSOR &   │ ───►  │   2. ON-CHIP    │ ───►  │ 3. ESP32 LWIP & │ ───►  │ 4. BROWSER / UI │
│   DMA CAPTURE   │       │   JPEG ENCODE   │       │   SOCKET WRITE  │       │  FETCH CONTROL  │
└─────────────────┘       └─────────────────┘       └─────────────────┘       └─────────────────┘
 [Suspect 1: Sensor]     [Suspect 2: Payload]      [Suspect 4: Backpressure] [Suspect 5: HTTP Lag]
```

| Suspect | Bottleneck Mechanism | Diagnostic Probe |
|---|---|---|
| **1. Sensor / DMA Capture** | Sensor clock or DMA scaling takes > 45ms per frame, capping raw FPS regardless of network | `/diag?frames=30&bench=1` isolates sensor without sending bytes over WiFi |
| **2. JPEG Frame Size** | High resolution + low compression creates 35–60 KB frames, exceeding 2.4GHz WiFi bandwidth | Frame size min/avg/max tracking in `/diag` |
| **3. WiFi Link & RSSI** | Weak signal (< -65 dBm) causes 802.11 retransmits, queue buildup, and packet drops | Real-time `wifi_rssi_dbm` and channel query in `/diag` |
| **4. TCP Send Backpressure** | `httpd_resp_send_chunk` blocks because TCP send buffers are full, stalling the stream task | Real-time `avg_send_ms` vs `avg_capture_ms` in `/diag` |
| **5. HTTP Server Starvation** | Rapid stream packets delay incoming `/drive` or `/status` HTTP requests | Concurrent API latency probe (idle vs streaming) in `cambot_crosstest.py` |

---

## 🛠️ What Was Built

### 1. Firmware Updates ([webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/main/webserver.c))
- **`GET /diag?frames=20&bench=1`**:
  - Automatically identifies camera model (`OV2640`, `OV3660`, `OV5640`, etc.) and sensor PID.
  - Runs an on-device isolated capture benchmark (timing each frame in microseconds, calculating min/avg/max/p95 capture time and frame sizes).
  - Reports free Heap, minimum free Heap, free PSRAM, WiFi SSID, RSSI (dBm), and WiFi channel.
  - Exposes live streaming metrics (`frames_sent`, `total_bytes`, `fps_current`, `bitrate_kbps`, `avg_capture_ms`, `avg_send_ms`, `max_send_ms`, `send_errors`).
- **`GET /diag_stream` & `GET /stream` Enhanced Telemetry**:
  - Injects per-frame metadata headers (`X-Frame-Num`, `X-Cap-Time-Us`) into each MJPEG multipart boundary.
  - Continuously computes exponential moving averages (EMA) for capture vs socket transmission time.

### 2. Python Peer Cross-Testing Suite ([cambot_crosstest.py](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/cambot_crosstest.py))
- Zero external dependencies (uses standard library `urllib`, `threading`, `json`, `statistics`).
- **8 Automated Diagnostic Phases**:
  1. **Device Discovery & Sensor Identity**: Confirms cambot1 (OV2640) and cambot3 (OV3660) identity, RSSI, and memory.
  2. **Baseline Idle Latency**: Measures baseline ping to `/status` and `/drive` (20 samples).
  3. **Isolated Sensor DMA Benchmark**: Tests raw hardware capture speed without network.
  4. **Active Stream Load & Control Lag**: Ingests live MJPEG stream while concurrently firing rapid `/drive` requests to measure control latency degradation.
  5. **Cross-Bot Peer Interference**: Probes the second bot while the first streams to detect WiFi channel congestion.
  6. **Repeat for Peer Bot**: Tests the other camera sensor under identical conditions.
  7. **Dual Stream Fleet Stress**: Streams from both bots simultaneously to measure aggregate WiFi throughput and multi-robot control latency.
  8. **Comprehensive Bottleneck Analysis**: Formats a comparative report and auto-diagnoses the root cause.

---

## ⚡ How to Build, Flash & Run

### Step 1: Build & Flash Firmware

#### For CamBot 1 (OV2640):
```bash
cd firmware/platforms/cambot
idf.py -DBOARD=esp32s3_cambot -DDEVICE_NUMBER=1 set-target esp32s3 build
idf.py -p COM<PORT> flash
```
*(Or upload the built `build/cambot.bin` via the web UI OTA at `http://cambot1.local`)*

#### For CamBot 3 (OV3660):
```bash
cd firmware/platforms/cambot
idf.py -DBOARD=esp32s3_cambot -DCAMERA=ov3660 -DDEVICE_NUMBER=3 set-target esp32s3 build
idf.py -p COM<PORT> flash
```
*(Or upload via OTA at `http://cambot3.local`)*

---

### Step 2: Run the Cross-Test Suite

From your laptop/terminal connected to the same WiFi network:

```bash
cd firmware/platforms/cambot
python cambot_crosstest.py
```

#### Custom Options:
```bash
# Explicit URLs / IPs:
python cambot_crosstest.py --bot1 http://cambot1.local --bot2 http://cambot3.local

# Test a single bot:
python cambot_crosstest.py --single http://cambot1.local

# Longer streaming duration or more latency samples:
python cambot_crosstest.py --stream-sec 12 --samples 30
```

---

### Step 3: Quick Manual HTTP Testing

You can also query the diagnostic endpoint directly in your browser or curl:

```bash
# 20-frame on-device camera benchmark
curl http://cambot1.local/diag?frames=20

# Query system status and live stream stats only (without running camera test)
curl http://cambot1.local/diag?bench=0

# CamBot 3 test
curl http://cambot3.local/diag?frames=20
```

---

## 📊 Sample Output & Interpretation

```
╔══════════════════════════════════════════════════════════════════════════╗
║              CamBot Cross-Diagnostic & Bottleneck Finder                ║
║                    Robot Fleet Performance Suite                         ║
╚══════════════════════════════════════════════════════════════════════════╝

━━━ Phase 1: Device Discovery & Hardware Identity ━━━━━━━━━━━━━━━━━━━━━━━━━
  Connecting to http://cambot1.local ... CONNECTED
    • Device:       cambot1 (Firmware v0.1)
    • Sensor:       OV2640 (PID 0x0026, Quality 16)
    • WiFi:         FleetNet (Channel 6, RSSI: -44 dBm)
    • Memory:       Heap: 142 KB free | PSRAM: 3 MB free
  Connecting to http://cambot3.local ... CONNECTED
    • Device:       cambot3 (Firmware v0.1)
    • Sensor:       OV3660 (PID 0x3660, Quality 16)
    • WiFi:         FleetNet (Channel 6, RSSI: -52 dBm)
    • Memory:       Heap: 138 KB free | PSRAM: 3 MB free

━━━ Phase 2: Baseline Idle Latency (Camera Stream OFF) ━━━━━━━━━━━━━━━━━━━
  Measuring baseline ping to http://cambot1.local (20 samples)...
    • /status latency:  avg=11.2ms  min=7.8ms  max=18.4ms  p95=14.1ms
    • /drive  latency:  avg=8.4ms   min=6.2ms  max=14.2ms  p95=10.8ms
  Measuring baseline ping to http://cambot3.local (20 samples)...
    • /status latency:  avg=12.6ms  min=8.1ms  max=22.1ms  p95=16.0ms
    • /drive  latency:  avg=9.1ms   min=6.8ms  max=15.4ms  p95=11.5ms

━━━ Phase 3: Isolated Sensor DMA & Capture Speed (No Network) ━━━━━━━━━━━
  Running 30-frame capture test on http://cambot1.local (OV2640)... DONE
    • Raw Sensor FPS:      24.2 FPS
    • DMA Capture Time:    avg=41.3ms  min=37.2ms  max=58.1ms  p95=48.0ms
    • JPEG Frame Size:     avg=26.4 KB  min=23 KB  max=31 KB
  Running 30-frame capture test on http://cambot3.local (OV3660)... DONE
    • Raw Sensor FPS:      21.8 FPS
    • DMA Capture Time:    avg=45.8ms  min=41.0ms  max=64.2ms  p95=54.2ms
    • JPEG Frame Size:     avg=31.2 KB  min=28 KB  max=38 KB

━━━ Phase 4: Active Stream Load & Under-Load Responsiveness (Bot 1: OV2640) ━━━
  Turning ON camera stream on http://cambot1.local...
  Probing http://cambot1.local /drive & /status UNDER HEAVY STREAMING LOAD...
    • Client Sustained FPS:   18.4 FPS (147 frames, 3.88 Mbps)
    • ESP32 Capture vs Send:  Capture avg=42.1ms | Socket Send avg=11.4ms (max=34.2ms)
    • /drive Latency Under Load: avg=24.2ms  max=48.1ms  p95=32.0ms
    • Button Lag Factor:      2.8x (normal)
```

---

## 🔧 Actionable Optimization Guide (For Low Ping + High FPS)

Based on diagnostic test results, apply these targeted optimizations:

### 1. If Frame Sizes are > 30 KB (`Suspect 2`):
In `firmware/platforms/cambot/boards/esp32s3_cambot/board_config.h`:
```c
// Increase quality number (18 or 20 reduces JPEG byte size by 30% with zero visible blur)
#define CAMERA_JPEG_QUALITY     18
```

### 2. If Socket Send is Blocking / Spiking (`Suspect 4`):
Ensure non-blocking behavior in `mjpeg_stream_task`:
```c
// If a send takes longer than 35ms, drop the next queued frame to prevent socket queue buildup:
if (send_us > 35000) {
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);
}
```

### 3. If WiFi Channel Congestion Occurs with Both Bots (`Phase 5`):
In `wifi_mgr.c` or router settings:
- Use a dedicated 2.4 GHz channel (1, 6, or 11) with 20 MHz channel width (avoid 40 MHz overlap).
- Set `CONFIG_ESP_WIFI_TX_BA_WIN=32` and `CONFIG_ESP_WIFI_RX_BA_WIN=32` in sdkconfig.
