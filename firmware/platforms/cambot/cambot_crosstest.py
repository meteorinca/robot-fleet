#!/usr/bin/env python3
"""
cambot_crosstest.py
===============================================================================
CamBot Peer Diagnostic & Bottleneck Analysis Tool
Robot Fleet — https://github.com/meteorinca/robot-fleet

Performs automated cross-testing between cambot1.local (OV2640) and
cambot3.local (OV3660) to measure:
  1. Camera DMA capture speed & JPEG sizes (on-device isolation)
  2. WiFi throughput & sustained FPS
  3. TCP socket send backpressure
  4. HTTP API responsiveness & button latency (idle vs active stream load)
  5. Peer WiFi airtime / channel interference
  6. Dual simultaneous stream fleet stress

Usage:
  python cambot_crosstest.py
  python cambot_crosstest.py --bot1 http://cambot1.local --bot2 http://cambot3.local
  python cambot_crosstest.py --single http://cambot1.local
===============================================================================
"""

import sys
import os
import time
import json
import socket
import argparse
import threading
import statistics
import urllib.request
import urllib.error
from urllib.parse import urljoin

# Enable ANSI colors and UTF-8 encoding on Windows conhost / PowerShell
if sys.platform == "win32":
    os.system("")
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    if hasattr(sys.stderr, "reconfigure"):
        try:
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

# ── ANSI Color Formatting ───────────────────────────────────────────────────
CLR_RESET  = "\033[0m"
CLR_BOLD   = "\033[1m"
CLR_DIM    = "\033[2m"
CLR_RED    = "\033[91m"
CLR_GREEN  = "\033[92m"
CLR_YELLOW = "\033[93m"
CLR_BLUE   = "\033[94m"
CLR_MAG    = "\033[95m"
CLR_CYAN   = "\033[96m"

def c_pass(text): return f"{CLR_GREEN}{CLR_BOLD}{text}{CLR_RESET}"
def c_warn(text): return f"{CLR_YELLOW}{CLR_BOLD}{text}{CLR_RESET}"
def c_fail(text): return f"{CLR_RED}{CLR_BOLD}{text}{CLR_RESET}"
def c_info(text): return f"{CLR_CYAN}{text}{CLR_RESET}"
def c_head(text): return f"{CLR_BOLD}{CLR_MAG}{text}{CLR_RESET}"
def c_dim(text):  return f"{CLR_DIM}{text}{CLR_RESET}"

# ── HTTP Helpers ─────────────────────────────────────────────────────────────
def clean_url(url):
    if not url.startswith("http://") and not url.startswith("https://"):
        return f"http://{url}"
    return url.rstrip("/")

def http_get_json(url, timeout=4.0):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "CamBot-Diag/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read().decode("utf-8")
            return json.loads(data), None
    except Exception as e:
        return None, str(e)

def http_get_text(url, timeout=3.0):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "CamBot-Diag/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="ignore"), resp.status, None
    except Exception as e:
        return None, 0, str(e)

def measure_endpoint_latency(base_url, endpoint, samples=15, timeout=2.0):
    """Measures latency (in milliseconds) for repeated requests to an endpoint."""
    url = f"{base_url}{endpoint}"
    latencies = []
    errors = 0

    for _ in range(samples):
        t0 = time.perf_counter()
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "CamBot-Diag/1.0"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp.read()
            dt_ms = (time.perf_counter() - t0) * 1000.0
            latencies.append(dt_ms)
        except Exception:
            errors += 1
        time.sleep(0.04)  # 40ms interval (~25Hz probe rate)

    if not latencies:
        return {"avg": 0, "min": 0, "max": 0, "p95": 0, "samples": 0, "errors": errors}

    latencies.sort()
    p95_idx = int(len(latencies) * 0.95)
    if p95_idx >= len(latencies): p95_idx = len(latencies) - 1

    return {
        "avg": statistics.mean(latencies),
        "min": min(latencies),
        "max": max(latencies),
        "p95": latencies[p95_idx],
        "samples": len(latencies),
        "errors": errors
    }

# ── MJPEG Stream Ingestion & Telemetry ───────────────────────────────────────
class StreamConsumer(threading.Thread):
    def __init__(self, stream_url, duration_sec=8.0):
        super().__init__(daemon=True)
        self.stream_url = stream_url
        self.duration_sec = duration_sec
        self.stop_event = threading.Event()
        self.frames_received = 0
        self.total_bytes = 0
        self.cap_times_us = []
        self.frame_sizes = []
        self.error = None
        self.actual_duration_sec = 0.0

    def run(self):
        t_start = time.perf_counter()
        try:
            req = urllib.request.Request(self.stream_url, headers={"User-Agent": "CamBot-Diag/1.0"})
            with urllib.request.urlopen(req, timeout=5.0) as stream:
                boundary = b"--frame"
                buf = bytearray()

                while not self.stop_event.is_set():
                    if (time.perf_counter() - t_start) >= self.duration_sec:
                        break

                    chunk = stream.read(4096)
                    if not chunk:
                        break
                    buf.extend(chunk)
                    self.total_bytes += len(chunk)

                    # Look for multipart delimiter
                    while True:
                        idx = buf.find(boundary)
                        if idx == -1:
                            break

                        frame_chunk = bytes(buf[:idx])
                        buf = buf[idx + len(boundary):]

                        if b"Content-Length:" in frame_chunk:
                            self.frames_received += 1
                            self.frame_sizes.append(len(frame_chunk))

                            # Parse embedded diagnostic header
                            cap_idx = frame_chunk.find(b"X-Cap-Time-Us:")
                            if cap_idx != -1:
                                try:
                                    end_line = frame_chunk.find(b"\r\n", cap_idx)
                                    val = int(frame_chunk[cap_idx + 14:end_line].strip())
                                    self.cap_times_us.append(val)
                                except Exception:
                                    pass

        except Exception as e:
            self.error = str(e)

        self.actual_duration_sec = time.perf_counter() - t_start

    def stop(self):
        self.stop_event.set()

    @property
    def fps(self):
        if self.actual_duration_sec > 0:
            return self.frames_received / self.actual_duration_sec
        return 0.0

    @property
    def bitrate_mbps(self):
        if self.actual_duration_sec > 0:
            return (self.total_bytes * 8.0) / (self.actual_duration_sec * 1_000_000.0)
        return 0.0


# ── Test Suite Orchestration ────────────────────────────────────────────────
class CamBotDiagnosticSuite:
    def __init__(self, bot1_url, bot2_url=None, stream_sec=8.0, probe_samples=20):
        self.bot1_url = clean_url(bot1_url)
        self.bot2_url = clean_url(bot2_url) if bot2_url else None
        self.stream_sec = stream_sec
        self.probe_samples = probe_samples
        self.results = {}

    def print_banner(self):
        print(f"\n{CLR_BOLD}{CLR_CYAN}╔══════════════════════════════════════════════════════════════════════════╗{CLR_RESET}")
        print(f"{CLR_BOLD}{CLR_CYAN}║              CamBot Cross-Diagnostic & Bottleneck Finder                ║{CLR_RESET}")
        print(f"{CLR_BOLD}{CLR_CYAN}║                    Robot Fleet Performance Suite                         ║{CLR_RESET}")
        print(f"{CLR_BOLD}{CLR_CYAN}╚══════════════════════════════════════════════════════════════════════════╝{CLR_RESET}\n")

    def run_discovery(self):
        print(f"{c_head('━━━ Phase 1: Device Discovery & Hardware Identity ━━━━━━━━━━━━━━━━━━━━━━━━━')}")
        bots = [("Bot 1", self.bot1_url)]
        if self.bot2_url:
            bots.append(("Bot 2", self.bot2_url))

        for name, url in bots:
            print(f"  Connecting to {CLR_BOLD}{url}{CLR_RESET} ... ", end="", flush=True)
            diag, err = http_get_json(f"{url}/diag?frames=0&bench=0", timeout=4.0)
            if err:
                print(c_fail(f"FAILED ({err})"))
                print(f"  {CLR_YELLOW}Attempting fallback /status check...{CLR_RESET} ", end="", flush=True)
                status, err2 = http_get_json(f"{url}/status", timeout=3.0)
                if err2:
                    print(c_fail(f"OFFLINE ({err2})"))
                    self.results[url] = {"online": False, "error": err2}
                    continue
                else:
                    print(c_pass("ONLINE (Legacy /status)"))
                    self.results[url] = {
                        "online": True,
                        "sensor_name": "Legacy (Flash updated firmware for /diag)",
                        "wifi_rssi": "N/A",
                        "free_psram": "N/A",
                        "free_heap": "N/A",
                        "version": status.get("version", "unknown"),
                        "legacy": True
                    }
            else:
                print(c_pass("CONNECTED"))
                sensor = diag.get("sensor", {})
                sys_info = diag.get("system", {})
                self.results[url] = {
                    "online": True,
                    "sensor_name": sensor.get("name", "Unknown"),
                    "sensor_pid": sensor.get("pid", "Unknown"),
                    "framesize": sensor.get("framesize", 0),
                    "quality": sensor.get("quality", 0),
                    "wifi_ssid": sys_info.get("wifi_ssid", "N/A"),
                    "wifi_rssi": sys_info.get("wifi_rssi_dbm", 0),
                    "wifi_chan": sys_info.get("wifi_channel", 0),
                    "free_heap": sys_info.get("free_heap_bytes", 0),
                    "free_psram": sys_info.get("free_psram_bytes", 0),
                    "version": diag.get("version", "unknown"),
                    "legacy": False
                }
                print(f"    • Device:       {CLR_BOLD}{diag.get('device', 'cambot')}{CLR_RESET} (Firmware v{diag.get('version')})")
                print(f"    • Sensor:       {CLR_MAG}{sensor.get('name', 'OV2640')}{CLR_RESET} (PID {sensor.get('pid', 'N/A')}, Quality {sensor.get('quality', 16)})")
                print(f"    • WiFi:         {sys_info.get('wifi_ssid')} (Channel {sys_info.get('wifi_channel')}, RSSI: {sys_info.get('wifi_rssi_dbm')} dBm)")
                print(f"    • Memory:       Heap: {sys_info.get('free_heap_bytes', 0)//1024} KB free | PSRAM: {sys_info.get('free_psram_bytes', 0)//1024//1024} MB free")

    def run_idle_latency(self):
        print(f"\n{c_head('━━━ Phase 2: Baseline Idle Latency (Camera Stream OFF) ━━━━━━━━━━━━━━━━━━━')}")
        for url in [self.bot1_url, self.bot2_url]:
            if not url or not self.results.get(url, {}).get("online"):
                continue

            # Ensure camera is off
            http_get_text(f"{url}/cam_off", timeout=2.0)
            time.sleep(0.3)

            print(f"  Measuring baseline ping to {CLR_BOLD}{url}{CLR_RESET} ({self.probe_samples} samples)...")
            status_lat = measure_endpoint_latency(url, "/status", samples=self.probe_samples)
            drive_lat  = measure_endpoint_latency(url, "/drive?steer=0&drive=0", samples=self.probe_samples)

            self.results[url]["idle_status_lat"] = status_lat
            self.results[url]["idle_drive_lat"]  = drive_lat

            print(f"    • /status latency:  avg={status_lat['avg']:.1f}ms  min={status_lat['min']:.1f}ms  max={status_lat['max']:.1f}ms  p95={status_lat['p95']:.1f}ms")
            print(f"    • /drive  latency:  avg={drive_lat['avg']:.1f}ms  min={drive_lat['min']:.1f}ms  max={drive_lat['max']:.1f}ms  p95={drive_lat['p95']:.1f}ms")

    def run_sensor_benchmark(self):
        print(f"\n{c_head('━━━ Phase 3: Isolated Sensor DMA & Capture Speed (No Network) ━━━━━━━━━━━')}")
        for url in [self.bot1_url, self.bot2_url]:
            if not url or not self.results.get(url, {}).get("online"):
                continue

            if self.results[url].get("legacy"):
                print(f"  {url}: Skipping /diag capture benchmark (requires updated firmware)")
                continue

            print(f"  Running 30-frame capture test on {CLR_BOLD}{url}{CLR_RESET} ({self.results[url]['sensor_name']})... ", end="", flush=True)
            diag, err = http_get_json(f"{url}/diag?frames=30&bench=1", timeout=8.0)
            if err or not diag:
                print(c_fail(f"FAILED ({err})"))
                continue

            bench = diag.get("capture_bench", {})
            self.results[url]["capture_bench"] = bench
            print(c_pass("DONE"))
            cap_ms = bench.get("capture_ms", {})
            f_bytes = bench.get("frame_bytes", {})
            print(f"    • Raw Sensor FPS:      {CLR_BOLD}{bench.get('raw_fps', 0):.1f} FPS{CLR_RESET}")
            print(f"    • DMA Capture Time:    avg={cap_ms.get('avg', 0):.1f}ms  min={cap_ms.get('min', 0):.1f}ms  max={cap_ms.get('max', 0):.1f}ms  p95={cap_ms.get('p95', 0):.1f}ms")
            print(f"    • JPEG Frame Size:     avg={f_bytes.get('avg', 0)/1024:.1f} KB  min={f_bytes.get('min', 0)//1024} KB  max={f_bytes.get('max', 0)//1024} KB")

    def run_single_stream_stress(self, target_url, peer_url=None):
        target_name = "Bot 1" if target_url == self.bot1_url else "Bot 2"
        sensor = self.results.get(target_url, {}).get("sensor_name", "Camera")
        print(f"\n{c_head(f'━━━ Phase 4: Active Stream Load & Under-Load Responsiveness ({target_name}: {sensor}) ━━━')}")

        # Start stream
        print(f"  Turning ON camera stream on {CLR_BOLD}{target_url}{CLR_RESET}...")
        http_get_text(f"{target_url}/cam_on", timeout=2.0)
        time.sleep(0.5)

        # Launch consumer thread
        consumer = StreamConsumer(f"{target_url}/stream", duration_sec=self.stream_sec)
        consumer.start()

        time.sleep(1.0) # Let stream stabilize

        # Probe target API latency concurrent with streaming
        print(f"  Probing {target_url} /drive & /status UNDER HEAVY STREAMING LOAD...")
        stream_status_lat = measure_endpoint_latency(target_url, "/status", samples=self.probe_samples)
        stream_drive_lat  = measure_endpoint_latency(target_url, "/drive?steer=0&drive=0", samples=self.probe_samples)

        # Also probe peer bot if present (test WiFi channel contention)
        peer_status_lat = None
        if peer_url and self.results.get(peer_url, {}).get("online"):
            print(f"  Probing peer {peer_url} to detect WiFi channel contention/interference...")
            peer_status_lat = measure_endpoint_latency(peer_url, "/status", samples=10)

        consumer.join(timeout=self.stream_sec + 3.0)
        consumer.stop()

        # Query on-device live stats from target
        diag, _ = http_get_json(f"{target_url}/diag?frames=0&bench=0", timeout=3.0)
        live_stats = diag.get("live_stream", {}) if diag else {}

        # Turn stream off & ensure motor stop
        http_get_text(f"{target_url}/cam_off", timeout=2.0)
        http_get_text(f"{target_url}/drive?stop=1", timeout=2.0)
        time.sleep(0.3)

        self.results[target_url]["stream_results"] = {
            "fps_client": consumer.fps,
            "bitrate_mbps": consumer.bitrate_mbps,
            "frames_received": consumer.frames_received,
            "stream_status_lat": stream_status_lat,
            "stream_drive_lat": stream_drive_lat,
            "peer_interference_lat": peer_status_lat,
            "device_stats": live_stats,
            "error": consumer.error
        }

        idle_drive = self.results[target_url].get("idle_drive_lat", {}).get("avg", 10.0)
        load_drive = stream_drive_lat.get("avg", 10.0)
        lag_factor = (load_drive / idle_drive) if idle_drive > 0 else 1.0

        print(f"    • Client Sustained FPS:   {CLR_BOLD}{consumer.fps:.1f} FPS{CLR_RESET} ({consumer.frames_received} frames, {consumer.bitrate_mbps:.2f} Mbps)")
        if live_stats:
            print(f"    • ESP32 Capture vs Send:  Capture avg={live_stats.get('avg_capture_ms', 0):.1f}ms | Socket Send avg={live_stats.get('avg_send_ms', 0):.1f}ms (max={live_stats.get('max_send_ms', 0):.1f}ms)")
        print(f"    • /drive Latency Under Load: avg={load_drive:.1f}ms  max={stream_drive_lat.get('max', 0):.1f}ms  p95={stream_drive_lat.get('p95', 0):.1f}ms")
        print(f"    • Button Lag Factor:      {c_warn(f'{lag_factor:.1f}x slower') if lag_factor > 3.0 else c_pass(f'{lag_factor:.1f}x (normal)')}")

    def run_dual_stream_stress(self):
        if not self.bot2_url or not self.results.get(self.bot2_url, {}).get("online"):
            return

        print(f"\n{c_head('━━━ Phase 5: Dual Concurrent Fleet Streaming Stress (Both Bots Active) ━━━━')}")
        print(f"  Starting simultaneous MJPEG streams on Bot 1 and Bot 2 for {self.stream_sec:.0f}s...")

        http_get_text(f"{self.bot1_url}/cam_on", timeout=2.0)
        http_get_text(f"{self.bot2_url}/cam_on", timeout=2.0)
        time.sleep(0.5)

        c1 = StreamConsumer(f"{self.bot1_url}/stream", duration_sec=self.stream_sec)
        c2 = StreamConsumer(f"{self.bot2_url}/stream", duration_sec=self.stream_sec)

        c1.start()
        c2.start()
        time.sleep(1.0)

        print(f"  Measuring concurrent fleet drive response under dual stream load...")
        lat1 = measure_endpoint_latency(self.bot1_url, "/drive?steer=0&drive=0", samples=10)
        lat2 = measure_endpoint_latency(self.bot2_url, "/drive?steer=0&drive=0", samples=10)

        c1.join(timeout=self.stream_sec + 3.0)
        c2.join(timeout=self.stream_sec + 3.0)
        c1.stop()
        c2.stop()

        http_get_text(f"{self.bot1_url}/cam_off", timeout=2.0)
        http_get_text(f"{self.bot2_url}/cam_off", timeout=2.0)

        total_mbps = c1.bitrate_mbps + c2.bitrate_mbps
        print(f"    • Bot 1 Stream:   {c1.fps:.1f} FPS | {c1.bitrate_mbps:.2f} Mbps | /drive lag avg={lat1.get('avg', 0):.1f}ms")
        print(f"    • Bot 2 Stream:   {c2.fps:.1f} FPS | {c2.bitrate_mbps:.2f} Mbps | /drive lag avg={lat2.get('avg', 0):.1f}ms")
        print(f"    • Combined Fleet Bandwidth: {CLR_BOLD}{total_mbps:.2f} Mbps{CLR_RESET}")

        self.results["dual_stream"] = {
            "bot1_fps": c1.fps,
            "bot1_mbps": c1.bitrate_mbps,
            "bot1_lat": lat1,
            "bot2_fps": c2.fps,
            "bot2_mbps": c2.bitrate_mbps,
            "bot2_lat": lat2,
            "total_mbps": total_mbps
        }

    def evaluate_bottlenecks(self):
        print(f"\n{CLR_BOLD}{CLR_CYAN}╔══════════════════════════════════════════════════════════════════════════╗{CLR_RESET}")
        print(f"{CLR_BOLD}{CLR_CYAN}║                     COMPREHENSIVE BOTTLENECK ANALYSIS                    ║{CLR_RESET}")
        print(f"{CLR_BOLD}{CLR_CYAN}╚══════════════════════════════════════════════════════════════════════════╝{CLR_RESET}\n")

        for url, data in self.results.items():
            if url == "dual_stream" or not data.get("online"):
                continue

            bot_label = f"Device: {url} ({data.get('sensor_name', 'Camera')})"
            print(f"{CLR_BOLD}{CLR_MAG}── {bot_label} ──{CLR_RESET}")

            # Suspect 1: Camera DMA / Hardware
            cap_bench = data.get("capture_bench", {})
            cap_avg = cap_bench.get("capture_ms", {}).get("avg", 0)
            raw_fps = cap_bench.get("raw_fps", 0)

            if cap_avg > 50.0:
                print(f"  {c_warn('[SUSPECT 1 - SENSOR SPEED]')} Raw capture takes {cap_avg:.1f}ms (caps max FPS at {raw_fps:.1f}).")
                print(f"    → Sensor clock or DMA scaling is taking significant time per frame.")
            else:
                print(f"  {c_pass('[SUSPECT 1 - SENSOR SPEED]')} Raw capture is fast ({cap_avg:.1f}ms avg → {raw_fps:.1f} theoretical FPS). Sensor is healthy.")

            # Suspect 2: JPEG compression size & bandwidth
            frame_avg_kb = cap_bench.get("frame_bytes", {}).get("avg", 0) / 1024.0
            if frame_avg_kb > 35.0:
                print(f"  {c_warn('[SUSPECT 2 - FRAME SIZE]')} Average frame is large ({frame_avg_kb:.1f} KB).")
                print(f"    → At 20 FPS, this requires > {frame_avg_kb*20*8/1000:.1f} Mbps of sustained local WiFi bandwidth.")
            else:
                print(f"  {c_pass('[SUSPECT 2 - FRAME SIZE]')} Frame sizes are efficient ({frame_avg_kb:.1f} KB avg).")

            # Suspect 3: WiFi Signal & Link Quality
            rssi = data.get("wifi_rssi", -100)
            if isinstance(rssi, (int, float)):
                if rssi < -70:
                    print(f"  {c_fail('[SUSPECT 3 - WIFI RSSI]')} Signal is weak ({rssi} dBm). High packet retransmits will cause stream stutter and control lag.")
                elif rssi < -55:
                    print(f"  {c_warn('[SUSPECT 3 - WIFI RSSI]')} Signal is moderate ({rssi} dBm). Minor packet delay possible.")
                else:
                    print(f"  {c_pass('[SUSPECT 3 - WIFI RSSI]')} Signal is strong ({rssi} dBm).")

            # Suspect 4: Socket Send Backpressure vs Capture
            stream_res = data.get("stream_results", {})
            dev_stats = stream_res.get("device_stats", {})
            send_ms = dev_stats.get("avg_send_ms", 0)
            cap_stream_ms = dev_stats.get("avg_capture_ms", 0)
            max_send_ms = dev_stats.get("max_send_ms", 0)

            if send_ms > cap_stream_ms or max_send_ms > 80.0:
                print(f"  {c_fail('[SUSPECT 4 - TCP BACKPRESSURE]')} Socket send is blocking ({send_ms:.1f}ms avg, {max_send_ms:.1f}ms max spike).")
                print(f"    → The TCP output buffer is filling faster than WiFi can drain it, stalling the stream loop.")
            else:
                print(f"  {c_pass('[SUSPECT 4 - TCP BACKPRESSURE]')} Socket writes are draining quickly ({send_ms:.1f}ms avg).")

            # Suspect 5: HTTP Server Thread & Button Lag
            idle_drive = data.get("idle_drive_lat", {}).get("avg", 10.0)
            stream_drive = stream_res.get("stream_drive_lat", {}).get("avg", 10.0)
            drive_p95 = stream_res.get("stream_drive_lat", {}).get("p95", 10.0)

            if stream_drive > 150.0 or drive_p95 > 250.0:
                print(f"  {c_fail('[SUSPECT 5 - BUTTON/API LAG]')} Control latency exploded from {idle_drive:.1f}ms (idle) to {stream_drive:.1f}ms (streaming) with {drive_p95:.1f}ms p95 spikes!")
                print(f"    → Root cause: WiFi transmission and socket buffer saturation are delaying HTTP requests for /drive.")
            elif stream_drive > 50.0:
                print(f"  {c_warn('[SUSPECT 5 - BUTTON/API LAG]')} Moderate control lag during streaming ({stream_drive:.1f}ms avg vs {idle_drive:.1f}ms idle).")
            else:
                print(f"  {c_pass('[SUSPECT 5 - BUTTON/API LAG]')} Button and control responsiveness remain instantaneous ({stream_drive:.1f}ms avg under load).")

            print()

        # Actionable Optimization Roadmap
        print(f"{CLR_BOLD}{CLR_GREEN}💡 ACTIONABLE OPTIMIZATIONS FOR LOW-PING, HIGH-FPS RC STREAMING:{CLR_RESET}")
        print("  1.  Tune JPEG Quality vs Speed:")
        print("      • Set CAMERA_JPEG_QUALITY = 18 or 20 in board_config.h (reduces payload by 25-35% with imperceptible visual difference).")
        print("  2.  Eliminate WiFi SoftAP / Router Jitter:")
        print("      • Lock WiFi channel to least congested 2.4GHz channel (1, 6, or 11).")
        print("      • Ensure CONFIG_ESP_WIFI_TX_BA_WIN=32 and CONFIG_LWIP_TCP_RTO_TIME=200 in sdkconfig.")
        print("  3.  Frame-Dropping Under Socket Congestion:")
        print("      • If a socket send takes > 30ms, immediately drop stale pending frames rather than buffering them.")
        print("  4.  Core Affinity Isolation:")
        print("      • Keep LWIP / WiFi stack on Core 0 and pin mjpeg_stream_task to Core 1.")
        print("  5.  Browser Joystick Event Throttling:")
        print("      • Web UI already throttles /drive calls, but ensure browser fetch uses keep-alive and does not pipeline behind pending requests.")

    def cleanup(self):
        """Ensure all streams and motors are stopped when suite finishes or aborts."""
        for url in [self.bot1_url, self.bot2_url]:
            if url and self.results.get(url, {}).get("online"):
                try:
                    http_get_text(f"{url}/cam_off", timeout=1.5)
                    http_get_text(f"{url}/drive?stop=1", timeout=1.5)
                except Exception:
                    pass

    def run_all(self):
        try:
            self.print_banner()
            self.run_discovery()
            self.run_idle_latency()
            self.run_sensor_benchmark()

            if self.results.get(self.bot1_url, {}).get("online"):
                self.run_single_stream_stress(self.bot1_url, peer_url=self.bot2_url)

            if self.bot2_url and self.results.get(self.bot2_url, {}).get("online"):
                self.run_single_stream_stress(self.bot2_url, peer_url=self.bot1_url)

            if self.bot2_url and self.results.get(self.bot1_url, {}).get("online") and self.results.get(self.bot2_url, {}).get("online"):
                self.run_dual_stream_stress()

            self.evaluate_bottlenecks()
        except KeyboardInterrupt:
            print(f"\n{c_warn('Test interrupted by user (Ctrl+C). Cleaning up...')}")
        finally:
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(description="CamBot Cross-Testing & Bottleneck Diagnostic Suite")
    parser.add_argument("--bot1", default="http://cambot1.local", help="URL or hostname of Bot 1 (default: http://cambot1.local)")
    parser.add_argument("--bot2", default="http://cambot3.local", help="URL or hostname of Bot 2 (default: http://cambot3.local)")
    parser.add_argument("--single", help="Test a single bot only (e.g. --single http://cambot1.local)")
    parser.add_argument("--stream-sec", type=float, default=8.0, help="Duration in seconds for stream load tests (default: 8.0)")
    parser.add_argument("--samples", type=int, default=20, help="Number of API latency probe samples (default: 20)")
    parser.add_argument("--output", help="Optional path to save full test results JSON (e.g. --output diag_results.json)")

    args = parser.parse_args()

    if args.single:
        suite = CamBotDiagnosticSuite(args.single, None, stream_sec=args.stream_sec, probe_samples=args.samples)
    else:
        suite = CamBotDiagnosticSuite(args.bot1, args.bot2, stream_sec=args.stream_sec, probe_samples=args.samples)

    suite.run_all()

    if args.output:
        try:
            with open(args.output, "w") as f:
                json.dump(suite.results, f, indent=2)
            print(f"\n{c_pass('Saved test results to')} {args.output}")
        except Exception as e:
            print(f"\n{c_fail('Failed to write output file:')} {e}")

if __name__ == "__main__":
    main()
