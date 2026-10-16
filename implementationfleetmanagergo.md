# Implementation Plan — Fleet Manager Go Web Application

## 1. Executive Summary & Objectives

The Robot Fleet project currently relies on a fragmented set of legacy Windows batch files and Python scripts to build, flash, discover, and update robots:
- `createallbinsYo.bat`: Sequential ESP-IDF builds looping through device numbers (e.g., `12` to `22`), copying `.bin` files into `fleet-manager/device_bins/`.
- `flash_device.bat`: Windows COM port detection and sequential `esptool` flashing.
- `scripts/buildbinesp32.ps1` / `scripts/flashbinesp32.ps1`: Interactive PowerShell scripts for individual bot builds and factory image merging.
- `fleet-manager/fleet_ota.py`: Python CLI script running threaded OTA updates over HTTP POST `/ota`.
- `fleet-manager/pinginthembots.py`: Python script scanning mDNS and ICMP/TCP to check online status.

### Objective
Unify and modernize these disparate tools into a **single, self-contained Go web application (`fleet-manager-go`)** featuring:

CHECK EXISTING GO APP IN THE FLEETHUB FOLDER (most up to date) AND ROBOHUB (outdated)
for inspiration and theme, etc of webui. I do like the tab method of nav, and the pretty plots, etc. NO EMOTICONS. NO EM-DASHES. NO VERBOSE OR DESCRIPTIVE TEXT. Minimalistic, modern, amazing, transparency. 

1. **Automated Firmware Build Engine**: Web & CLI driven batch compilation across platforms and device number ranges (`DEVICE_NUMBER=12..22`), outputting organized binary artifacts.
2. **Concurrent Fleet OTA Deployer**: Real-time worker pool capable of flashing the entire fleet in parallel with per-device progress bars and post-reboot verification.
3. **Live Fleet Scanner & Inventory**: High-speed mDNS/HTTP discovery tracking device health, IP, signal strength, firmware version, and battery.
4. **Hardware Provisioning & Serial Flasher**: Auto-detection of USB COM ports with support for both server-side `esptool` (or use go to run the ./build and/or flash ps1 files (also add sh if we're on linux)) flashing and browser-based **WebSerial** flashing. (THE Webserial will be the primary tool so make sure it has nice ui and animated minimalistic (like old school mac progress bars) while uploading is taking place (accurate percentage).
5. **Shared Configuration Manager**: Native awareness and WebUI editing of centralized `firmware/common/secrets.h` and platform configs.
6. **Zero-Dependency Single Binary**: Embedded Web UI (`go:embed`) with live streaming logs via Server-Sent Events (SSE).

---

## 2. Legacy Scripts Audit & Functional Mapping

| Legacy Tool | Primary Function | Pain Points & Limitations | Go Web App Replacement |
|---|---|---|---|
| `createallbinsYo.bat` | Loops `DEVICE_NUMBER=12..22`, calls `idf.py -DBOARD=esp32c3_dog ...` | Hardcoded loop range, Windows-only, blocks terminal, no live web monitoring, manual file copying | **Build Studio Engine**: Configurable range, platform selector, live SSE console, automatic artifact indexing |
| `flash_device.bat` | Scrapes COM ports via PowerShell, calls `esptool.py write_flash` | Brittle regex parsing of `Get-PnpDevice`, Windows-only | **Serial Flasher**: Native COM/tty enumeration, one-click flash, WebSerial API support in browser |
| `fleet_ota.py` | Python `ThreadPoolExecutor` uploading binaries to `/ota` | Requires Python runtime, complex CLI flags, no persistent deployment history | **Concurrent OTA Pipeline**: Goroutine worker pool, live progress gauges, Canary rollout, auto rollback/retry |
| `pinginthembots.py` | Concurrently pings mDNS names (`paulbot12..22`) and queries `/api/info` | CLI terminal only, requires Python environment | **Live Fleet Monitor**: Real-time auto-refreshing matrix with battery, RSSI, and ping latency |
| `buildbinesp32.ps1` | Interactive PowerShell prompt for chip, bot, and device number | Interactive prompts not scriptable from web | **REST Build API + Web UI**: Preset dropdowns, batch queue, factory image merging | (I like this ps1 so if go can just use this or the other ones (and create .bat or .sh for linux. but ofc if esptool or some other method requires less work and is easier better faster and requires less installing, then go ahead.

---

## 3. High-Level System Architecture

```mermaid
graph TD
    subgraph Browser Client [Web UI - Embedded Single Page App]
        Dashboard[Live Fleet Dashboard]
        BuildUI[Build Studio UI]
        OTAUI[OTA Deployment Center]
        SerialUI[WebSerial Flasher]
        SecretsUI[Secrets / WiFi Editor]
    end

    subgraph GoBackend [fleet-manager-go Backend Service]
        HTTPRouter[net/http & Chi Router + SSE Hub]
        
        subgraph CoreSubsystems [Core Subsystems]
            Scanner[Fleet Scanner & mDNS Resolver]
            BuildMgr[Firmware Build Engine]
            OTAMgr[Concurrent OTA Deployer]
            SerialMgr[USB Serial & esptool Runner]
            ConfigMgr[Secrets & Config Store]
        end

        DB[(SQLite / JSON Store)]
    end

    subgraph External [External Tools & Fleet]
        ESPIDF[ESP-IDF idf.py / CMake]
        ESPTool[esptool / merge_bin]
        Robots[Fleet Robots (ESP32-C3 / S3)]
        CommonSecrets[firmware/common/secrets.h]
    end

    BrowserClient <-->|REST & SSE Events| HTTPRouter
    HTTPRouter --> CoreSubsystems
    CoreSubsystems --> DB

    BuildMgr -->|os/exec| ESPIDF
    BuildMgr -->|os/exec| ESPTool
    BuildMgr -->|Store Binaries| DB
    ConfigMgr <-->|Read / Write| CommonSecrets

    OTAMgr -->|Concurrent HTTP POST /ota| Robots
    Scanner -->|mDNS & /api/info| Robots
    SerialMgr -->|COM Port USB| Robots
```

---

## 4. Key Subsystem Specifications

### 4.1. Firmware Build Studio (`pkg/builder`)
- **Execution Model**:
  - Executes `idf.py` via Go `os/exec.CommandContext`.
  - Captures stdout/stderr via io.Pipe and streams chunked lines in real-time to an SSE topic (`/api/builds/{id}/stream`).
- **Batch Range Generation**:
  - Takes `Platform` (e.g. `dogbot_v1`, `rfbot`, `simplebot`), `Board` (`esp32c3_dog`), `Chip` (`esp32c3`), and `DeviceRange` (e.g. `12-22` or `[1, 2, 5, 10]`).
  - Iterates sequentially or with bounded concurrency (e.g., 2 parallel builds if CPU allows).
  - Automatically runs `python -m esptool merge_bin` to generate a unified `factory.bin` alongside individual partition binaries.
- **Artifact Organization**:
  ```text
  artifacts/
  └── builds/
      └── dogbot_v1/
          └── v1.9_2026-10-07_230000/
              ├── build_manifest.json
              ├── device_12/
              │   ├── factory.bin
              │   ├── mojDogv1.bin
              │   ├── bootloader.bin
              │   └── partition-table.bin
              ├── ...
              └── device_22/
                  └── ...
  ```

### 4.2. Concurrent Fleet OTA Deployer (`pkg/ota`)
- **Worker Pool Architecture**:
  - Configurable worker count (default: 8 goroutines).
  - Rate limiting and retry backoff for robots with weak Wi-Fi signal.
- **Rollout Modes**:
  1. **Canary Mode**: Updates 1 bot first, checks `/api/info` to ensure successful boot and FW version update before proceeding with the rest.
  2. **Batch Parallel Mode**: Flashes all selected online bots concurrently.
- **Progress Tracking**:
  - Custom `io.Reader` wrapper that tracks uploaded bytes and emits progress events (`0% -> 100%`) over SSE.
  - Verification stage: Waits 5–10 seconds for reboot, probes `http://<bot>.local/api/info`, and confirms reported `fw_version` matches target.

### 4.3. Fleet Scanner & Discovery Engine (`pkg/scanner`)
- **Multi-Vector Discovery**:
  - **mDNS Scanning**: Continuously listens for `_http._tcp.local.` and `_robot._tcp.local.`.
  - **Target Prefix Sweep**: Rapid TCP port 80 check against known naming templates:
    - `paulbot%d.local` (`12..22`)
    - `rfbot%d.local` (`1..10`)
    - `simplebot%d.local` (`1..10`)
    - `cambot%d.local` (`1..5`)
    - `carbot%d.local` (`1..5`)
- **Telemetry Probing**:
  - Hits `/api/info` to collect:
    - `device_name`, `device_number`, `fw_version`
    - `wifi_ssid`, `wifi_rssi_dbm`, `wifi_channel`
    - `battery_mv`, `battery_pct`
    - `uptime_seconds`, `free_heap`
  - Emits real-time state changes to the dashboard.

### 4.4. Serial Hardware Provisioning (`pkg/flasher`)
- **Backend COM Port Flasher**:
  - Enumerates USB Serial devices on Windows (`COMx`) and Linux/macOS (`/dev/ttyUSB*`, `/dev/ttyACM*`).
  - Triggers `esptool.py` directly from the server when the target device is plugged into the host machine.
- **Browser-Native WebSerial Flasher**:
  - Uses the WebSerial API (`navigator.serial`) directly inside Chrome/Edge.
  - Allows users to flash robots connected to their **client laptop** directly through the browser without needing Python or ESP-IDF installed locally.

### 4.5. Centralized Secrets & Config Manager (`pkg/config`)
- **Integration with `firmware/common/secrets.h`**:
  - Parses and safely modifies `#define WIFI_SSID_X` and `#define WIFI_PASS_X` in `firmware/common/secrets.h`.
  - Provides a clean WebUI form to add, edit, reorder, or test WiFi networks without manual C-header editing.
  - Detects if robots have connected to `StarBeam` or `Gandalf` and displays network affinity.

---

## 5. Web UI Design & Aesthetics

The web application will adhere to a **sleek dark-mode aesthetic** with glassmorphic cards, vibrant accent metrics, and high-density operational telemetry:
- **Navigation Bar**: Quick stats (Total Fleet Size, Online Count, Active Builds, WiFi Health).
- **Tab 1: Fleet Command Matrix**:
  - Filterable grid/table of all known bots.
  - Color-coded status pills: `ONLINE` (green), `UPDATING` (amber pulsing), `OFFLINE` (gray), `ERROR` (red).
  - Quick action buttons: **Reboot**, **Ping**, **Identify (LED flash)**, **OTA Update**.
- **Tab 2: Build Studio**:
  - Platform dropdown (`dogbot_v1`, `rfbot`, etc.).
  - Target range selector (e.g. `12` to `22`).
  - "Start Batch Build" button.
  - Real-time terminal output drawer with ANSI syntax color highlights.
- **Tab 3: OTA Deployment Center**:
  - Select built binary version.
  - Multi-select target bots with "Select All Online" helper.
  - Live animated progress bars showing per-robot upload speed and percent complete.
- **Tab 4: USB Provisioning**:
  - Port selector and baud rate setting (default 921600).
  - Drag-and-drop or select pre-built binary.
  - One-click flash button with real-time flash log.
- **Tab 5: Fleet Settings & Secrets**:
  - Visual editor for `firmware/common/secrets.h`.
  - ESP-IDF environment paths inspector.

---

## 6. Directory Structure for the Go Application

```text
fleet-manager/
├── cmd/
│   └── fleet-manager/
│       └── main.go                  # CLI entrypoint (subcommands: serve, build, ota, scan)
├── pkg/
│   ├── builder/
│   │   ├── idf_runner.go            # idf.py execution & log streaming
│   │   ├── merge.go                 # esptool merge_bin automation
│   │   └── types.go                 # Build job definitions & status
│   ├── ota/
│   │   ├── deployer.go              # Goroutine worker pool & HTTP client
│   │   ├── progress.go              # Byte tracking reader
│   │   └── verify.go                # Post-reboot /api/info check
│   ├── scanner/
│   │   ├── mdns.go                  # mDNS discovery
│   │   ├── ping.go                  # TCP/HTTP latency & status check
│   │   └── probe.go                 # /api/info capability parser
│   ├── flasher/
│   │   ├── serial.go                # OS COM port enumeration
│   │   └── esptool.go               # esptool execution wrapper
│   ├── secrets/
│   │   └── manager.go               # Parser/updater for firmware/common/secrets.h
│   ├── db/
│   │   ├── store.go                 # SQLite database initialization
│   │   └── models.go                # Bot records, build history, OTA logs
│   └── web/
│       ├── server.go                # HTTP routes, SSE broker, static file server
│       ├── handlers_build.go        # Build REST API
│       ├── handlers_ota.go          # OTA REST API
│       ├── handlers_fleet.go        # Fleet status REST API
│       └── static/                  # Embedded WebUI assets (go:embed)
│           ├── index.html
│           ├── app.js
│           ├── style.css
│           └── esp_serial_flasher.js # WebSerial client library
├── go.mod
├── go.sum
└── README.md
```

---

## 7. Implementation Roadmap & Phased Milestones

### Phase 1: Go Foundation & Fleet Scanner (Immediate)
- Initialize Go module (`go.mod`) with dependencies (`go-chi/chi`, `grandcat/zeroconf` or `miekg/dns`, `mattn/go-sqlite3`).
- Implement `pkg/scanner` to replace `pinginthembots.py`:
  - mDNS discovery and HTTP `/api/info` probing.
  - Verified against current online robots (e.g., `simplebot`, `rfbot`, `dogbot`).
- Set up embedded web server serving baseline dashboard.

### Phase 2: Firmware Build Studio (Replaces `createallbinsYo.bat`)
- Implement `pkg/builder/idf_runner.go`:
  - Locate ESP-IDF PowerShell / batch environment automatically (`C:\Espressif\tools\...`).
  - Support building individual bots and device number loops (`DEVICE_NUMBER=X..Y`).
  - Automated binary merging via `python -m esptool merge_bin`.
- Add SSE log streaming so compilation output displays in real time on the web dashboard.

### Phase 3: Concurrent Fleet OTA Engine (Replaces `fleet_ota.py`)
- Implement `pkg/ota/deployer.go`:
  - Goroutine worker pool with configurable concurrency.
  - Streaming multipart POST to `/ota` endpoint.
  - Post-flash health check polling `/api/info` until reboot completes.
- Add live per-bot progress bars to the WebUI.

### Phase 4: USB Flasher & WebSerial Provisioning (Replaces `flash_device.bat`)
- Implement COM port enumeration in `pkg/flasher`.
- Add server-side flashing via `esptool.py`.
- Implement client-side WebSerial flashing in `web/static/esp_serial_flasher.js` for zero-install client browser flashing.

### Phase 5: Centralized Secrets Integration & Migration
- Connect `pkg/secrets` to `firmware/common/secrets.h`:
  - Allow adding/editing WiFi SSIDs (like `Gandalf` and `StarBeam`) directly via the Web UI.
- Verify full end-to-end flow:
  1. Edit WiFi in WebUI -> saves to `firmware/common/secrets.h`.
  2. Click "Batch Build Dogbot (12-22)" -> builds all binaries into organized folder.
  3. Click "Deploy OTA" -> flashes online fleet with live progress.
- Deprecate legacy `.bat` and Python scripts into an archive directory.
