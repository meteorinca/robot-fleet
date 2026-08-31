# CamBot Peer Diagnostic Test — Camera Bottleneck Finder

**Problem:** Everything on cambot1.local and cambot3.local works great (buttons, motors, LED) *except* when the camera stream is turned on — something causes responsiveness issues. We need the cambots to test **each other** and pinpoint exactly where the bottleneck lives.

**Approach:** Add a lightweight `/diag` endpoint to the firmware and a companion Python script that orchestrates cambot1 ↔ cambot3 cross-testing. Each bot probes the other's endpoints under various streaming conditions and reports precise timing data.

---

## What We'll Measure (The 5 Suspects)

The camera stream pipeline has these stages, and any of them could be the bottleneck:

| # | Suspect | What It Means | How We Test It |
|---|---------|--------------|----------------|
| 1 | **Camera DMA capture** | `esp_camera_fb_get()` blocks too long — the OV2640/OV3660 + DMA engine can't deliver frames fast enough | New `/diag` endpoint: capture N frames, report avg/min/max capture time + frame sizes |
| 2 | **JPEG encoding** | On-sensor JPEG compression produces frames too large for WiFi to keep up | `/diag` reports actual JPEG sizes per frame — if frames are 40KB+ at VGA, that's the issue |
| 3 | **WiFi TX throughput** | The ESP32-S3's WiFi radio can't push the MJPEG byte rate fast enough — packets queue up | Cross-test: bot A downloads `/stream` from bot B, measures sustained bytes/sec vs theoretical max |
| 4 | **HTTP server starvation** | The `httpd` worker threads are blocked by the stream, causing `/drive`, `/status`, etc. to time out | Cross-test: bot A hits bot B's `/drive` and `/status` endpoints *while streaming* and measures response latency vs baseline |
| 5 | **TCP/socket backpressure** | `httpd_resp_send_chunk()` blocks because the TCP send buffer is full, stalling the capture loop | `/diag` measures how long each `send_chunk` call takes inside the stream task |

---

## Proposed Changes

### Firmware: New `/diag` Endpoint

#### [MODIFY] [webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/main/webserver.c)

Avoid emoticons and too verbose labels/text etc.

Add a new `GET /diag` handler that runs an on-device self-test and returns JSON:

```
GET /diag?frames=20
```

Returns:
```json
{
  "device": "cambot1",
  "test": "capture_bench",
  "frames_requested": 20,
  "frames_captured": 20,
  "capture_times_ms": {
    "avg": 42.3,
    "min": 38.1,
    "max": 67.8,
    "p95": 55.2
  },
  "frame_sizes_bytes": {
    "avg": 28456,
    "min": 24012,
    "max": 35890
  },
  "total_bytes": 569120,
  "total_time_ms": 846,
  "fps": 23.6,
  "heap_free": 142360,
  "heap_min_free": 98200,
  "psram_free": 3145728,
  "streaming_was_active": false,
  "wifi_rssi": -42
}
```

**What this tests:** Suspects #1 (capture speed) and #2 (JPEG size). Runs entirely on-device — no network involved — so it isolates camera+DMA performance.

**Implementation detail:**
- Temporarily captures N frames using `esp_camera_fb_get()` / `esp_camera_fb_return()`
- Times each capture with `esp_timer_get_time()` (microsecond precision)
- Records frame sizes
- Computes stats
- Reports heap/PSRAM to check if memory pressure is a factor
- Queries WiFi RSSI via `esp_wifi_sta_get_rssi()` to flag weak signal
- Can be run with or without streaming active (run both ways to compare)

---

#### [MODIFY] [webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/main/webserver.c) — `/diag_stream` endpoint

Add a second endpoint that benchmarks the **full network path**:

```
GET /diag_stream?seconds=10
```

Returns:
```json
{
  "device": "cambot1",
  "test": "stream_bench",
  "duration_ms": 10023,
  "frames_sent": 148,
  "total_bytes_sent": 4213440,
  "throughput_kbps": 3362,
  "fps_actual": 14.8,
  "send_times_ms": {
    "avg": 12.1,
    "min": 3.2,
    "max": 89.4,
    "p95": 45.6
  },
  "capture_times_ms": {
    "avg": 41.5,
    "min": 37.0,
    "max": 62.1
  },
  "frames_dropped": 2,
  "send_errors": 0,
  "wifi_rssi": -42
}
```

**What this tests:** Suspects #3 (WiFi throughput), #4 (server starvation), and #5 (TCP backpressure). It streams to the requesting client while instrumenting every frame.

**Implementation detail:**
- Runs like `mjpeg_stream_task` but with per-frame timing instrumentation
- Separately measures: (a) time in `esp_camera_fb_get()`, (b) time in `httpd_resp_send_chunk()`
- If send_time >> capture_time, the bottleneck is network/TCP
- If capture_time >> send_time, the bottleneck is camera/DMA
- Reports `frames_dropped` when a send takes so long that the next capture is stale

---

### Firmware: Register New Endpoints

#### [MODIFY] [webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/main/webserver.c) — `webserver_start()`

Add to the URI table (line ~940):
```c
{ "/diag",        HTTP_GET,  diag_handler,        NULL },
{ "/diag_stream", HTTP_GET,  diag_stream_handler, NULL },
```

---

### Python Orchestrator Script: Cross-Test

#### [NEW] [cambot_crosstest.py](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/cambot_crosstest.py)

A Python script you run from your laptop that makes cambot1 and cambot3 test each other:

```
python cambot_crosstest.py
```

**Test sequence:**

| Phase | What Happens | Duration |
|-------|-------------|----------|
| 1. **Baseline API latency** | Hit `/status`, `/drive?steer=0&drive=0`, `/cam_status` on both bots 20× each, measure response times | ~5 sec |
| 2. **On-device capture bench** | Call `/diag?frames=30` on each bot (camera only, no stream) | ~3 sec each |
| 3. **Stream throughput** | Turn on cam1 stream, have the script download from `/diag_stream?seconds=10` on cam1 | 10 sec |
| 4. **API latency UNDER LOAD** | While cam1 is streaming via `/stream`, rapidly hit cam1's `/status` + `/drive` + `/cam_status` 20× each, measure response times | ~5 sec |
| 5. **Cross-bot probe under load** | cam1 streams; script hits cam3's endpoints to verify cam3 is unaffected (control group) | ~5 sec |
| 6. **Repeat for cam3** | Same tests but with cam3 streaming | ~25 sec |
| 7. **Both streaming** | Both cams stream simultaneously; measure API latency on both | ~10 sec |

**Output:** A formatted report like:

```
╔══════════════════════════════════════════════════════════════════╗
║              CamBot Cross-Diagnostic Report                     ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  CAMBOT1 (cambot1.local)           CAMBOT3 (cambot3.local)      ║
║  ─────────────────────             ─────────────────────         ║
║  WiFi RSSI: -38 dBm (good)        WiFi RSSI: -55 dBm (fair)    ║
║  Heap free: 142 KB                 Heap free: 138 KB            ║
║  PSRAM free: 3.0 MB               PSRAM free: 3.0 MB           ║
║                                                                  ║
║  CAPTURE (camera only, no network)                               ║
║  ─────────────────────────────────                               ║
║  Avg capture: 42 ms  ✅            Avg capture: 44 ms  ✅       ║
║  Avg frame:   28 KB                Avg frame:   29 KB           ║
║  FPS (raw):   23.6                 FPS (raw):   22.8            ║
║                                                                  ║
║  STREAM (capture + network)                                      ║
║  ──────────────────────────                                      ║
║  Throughput:  3.3 Mbps             Throughput:  2.1 Mbps  ⚠️    ║
║  FPS (net):   14.8                 FPS (net):   9.2  ⚠️         ║
║  Send p95:    46 ms                Send p95:    112 ms  🔴      ║
║                                                                  ║
║  API LATENCY (no stream)                                         ║
║  ────────────────────────                                        ║
║  /status avg:  12 ms  ✅           /status avg:  14 ms  ✅      ║
║  /drive avg:    8 ms  ✅           /drive avg:    9 ms  ✅      ║
║                                                                  ║
║  API LATENCY (while streaming)                                   ║
║  ──────────────────────────────                                  ║
║  /status avg:  45 ms  ✅           /status avg:  890 ms  🔴     ║
║  /drive avg:   38 ms  ✅           /drive avg:   1240 ms 🔴     ║
║                                                                  ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  🔍 BOTTLENECK ANALYSIS                                         ║
║  ────────────────────────                                        ║
║                                                                  ║
║  cambot1: ✅ No significant bottlenecks detected                ║
║                                                                  ║
║  cambot3: 🔴 BOTTLENECK FOUND — WiFi TX Backpressure            ║
║    • WiFi RSSI is -55 dBm (weaker signal = lower throughput)    ║
║    • Send p95 is 112ms (vs 46ms on cam1 — 2.4× slower)         ║
║    • API latency explodes during streaming (890ms vs 14ms)      ║
║    • Raw capture speed is fine (44ms) — camera is NOT the issue ║
║    → The httpd send buffer fills up because WiFi can't drain    ║
║      it fast enough. The chunked sends block, which stalls the  ║
║      httpd worker pool, starving /drive and /status.            ║
║                                                                  ║
║  💡 RECOMMENDATIONS:                                             ║
║    1. Move cambot3 closer to the WiFi AP (or add a repeater)   ║
║    2. Reduce CAMERA_FRAME_SIZE from VGA to QVGA (halves data)  ║
║    3. Increase CAMERA_JPEG_QUALITY from 16 to 20 (smaller JPEGs)║
║    4. Reduce CAMERA_FB_COUNT from 2 to 1 (frees PSRAM)         ║
║    5. Consider adding frame-skip logic when send_time > 50ms   ║
║                                                                  ║
╚══════════════════════════════════════════════════════════════════╝
```

**The script auto-diagnoses by comparing:**
- Capture time vs send time → camera bottleneck or network bottleneck?
- API latency idle vs streaming → httpd starvation?
- cam1 metrics vs cam3 metrics → device-specific issue or systemic?
- WiFi RSSI → signal strength causing throughput issues?
- Frame sizes → are JPEGs too large for the available bandwidth?

---

## File Summary

| File | Change | Purpose |
|------|--------|---------|
| [webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/main/webserver.c) | MODIFY | Add `diag_handler` and `diag_stream_handler` + register URIs |
| [cambot_crosstest.py](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/cambot/cambot_crosstest.py) | NEW | Python orchestrator that runs the full cross-diagnostic suite |

---

## User Review Required

> [!IMPORTANT]
> **Firmware flash required.** The `/diag` and `/diag_stream` endpoints must be compiled into the firmware and OTA'd to both cambot1 and cambot3 before the cross-test script can run. The existing endpoints (`/status`, `/drive`, etc.) will also be tested as-is.

> [!NOTE]
> **RAM budget.** The `/diag` handler needs to buffer timing stats for up to 50 frames. At ~16 bytes per frame record, that's ~800 bytes on the stack — well within the 4KB we have. The JSON response itself is built incrementally via `httpd_resp_send_chunk` to avoid needing a large buffer.

---

## Open Questions

> [!IMPORTANT]
> **Camera model:** cambot1 has OV2640 and cambot3 has OV3660? This matters because the two sensors have different JPEG encoding speeds and the cross-test comparison would need to account for that.
ANSWER: YES, cambot1 has OV2640 and cambot3 has OV3660.

> [!NOTE]
> **Both bots on same WiFi AP?** If cambot1 and cambot3 connect to different access points or bands (2.4 GHz vs 5 GHz), that could explain asymmetric throughput. The script will capture RSSI but knowing the physical setup helps interpret results.
ANSWER: Both bots are on the same wifi AP and should have similar signal strength. 

> [!NOTE]
> **"Unresponsive" definition:** When you say buttons stop working with camera on — does the web UI freeze entirely (can't load the page), or do specific buttons (like /drive) just lag? And is this from a phone or desktop browser? This helps narrow whether it's the ESP choking on sends or the browser struggling to render the MJPEG stream + handle JS events simultaneously.
ANSWER: We're mainly talking about camera latency, fps, quality. WE want to figure out what the bottleneck is and what maximum we can achieve with good software and current hardware. We don't want to compromise on latency so that is the main. What is the max quality we can get at 30fps? What is the min latency we can get at 30fps? etc.

---

## Verification Plan

### Automated Tests
1. Build firmware: `idf.py set-target esp32s3 build` — verify `/diag` and `/diag_stream` compile without errors
2. Flash to both bots: `idf.py -DDEVICE_NUMBER=1 flash` and `idf.py -DDEVICE_NUMBER=3 flash`
3. Smoke test: `curl http://cambot1.local/diag?frames=5` — verify valid JSON response
4. Run full suite: `python cambot_crosstest.py` — verify complete report generation

### Manual Verification
- Compare diagnostic results against observed behavior ("cam1 works fine, cam3 stutters" should correlate with the numbers)
- Verify that the new `/diag` endpoint doesn't interfere with normal operation (it captures frames but doesn't start a persistent stream)
