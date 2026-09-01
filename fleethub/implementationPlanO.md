# FleetHub Optimization, RF Traffic Management, and Visual Automation Builder

## Goal Description
Enhance FleetHub with robust memory, bandwidth, and storage controls for high-volume RF telemetry; register CamBot 3 (`cambot3.local`); clean up descriptive text under the FleetHub Daemon node in the mesh topology view; and introduce an intuitive drag-and-drop automation builder ("FleetFlow") supporting complex multi-condition logic (e.g. weather conditions, solar elevation, sensor triggers, and WLED actuators) without hardcoding any specific appliance.

Per user instruction, emoticons are strictly excluded throughout the plan, code, and documentation.

---

## User Review Required

> [!IMPORTANT]
> **RF Telemetry & Aggregate Retention Policy**:
> High-frequency RF noise can rapidly generate dozens of distinct single-hit codes and thousands of event rows. We propose:
> 1. SQLite circular retention for `rf_events`: Maintain the latest 1,000 events automatically using an asynchronous database trigger or prune routine upon ingest.
> 2. Ephemeral noise isolation for `rf_aggregates`: Add a default query filter to display codes with at least 2 receptions (or registered/mapped sensors) and provide a "Purge Ephemeral Noise" action to delete 1-hit noise records older than 1 hour.
> 3. Browser memory cap: Restrict real-time in-DOM table rows to 50 entries, shedding older rows to prevent browser memory leaks.

> [!NOTE]
> **Device Registration**:
> `cambot3.local` will be registered with fallback IP `10.0.0.68`, port 80, role `camera`, and streaming/snapshot action endpoints consistent with `cambot1`.

---

## Proposed Changes

### 1. Device Registry Configuration

#### [MODIFY] fleethub_config.json
#### [MODIFY] known_devices.json
* Append `cambot3` (`CamBot 3`) with hostname `cambot3.local`, IP `10.0.0.68`, port 80, role `camera`, ping enabled, and standard camera action definitions (`/cam_on`, `/cam_off`, `/snapshot`, `/toggle`).

---

### 2. Mesh Topology & UI Cleanup

#### [MODIFY] web/public/index.html
* **Remove descriptive text under FleetHub Daemon**: Remove the secondary labels (`Go Runtime Port 8126` and `Rule Engine SQLite Relay`) from `#node-fleethub` so the node remains minimal, displaying only the node title and active status indicator.
* **Real-time RF DOM Memory Management**:
  * Enforce strict upper bound (50 items) on `#telemetry-rf-tbody`, `#mesh-terminal-log`, and `#hud-rf-feed`.
  * Ensure incoming WebSocket bursts recycle DOM nodes rather than appending unbound elements.
* **Signal Frequency Aggregates Section Overhaul**:
  * Replace the wide grid of individual tile cards (which currently renders 60+ cards) with a compact, searchable, and sortable table view.
  * Add controls: "Filter Noise (Hits >= 2)", "Show All", and "Purge Noise".
  * Display a summary bar: Total Tracked Codes, Active Mapped Sensors, and Noise Count.

---

### 3. Backend RF Data & Storage Management

#### [MODIFY] pkg/db/db.go
* Add `PruneRFEphemeralNoise(minHits int, olderThan time.Duration)`: Removes noise codes from `rf_aggregates` that have fewer than `minHits` receptions and are not assigned to a known sensor.
* Add `EnforceRFEventRetention(maxRows int)`: Keeps the `rf_events` log within a safe bound (e.g. 1,000 rows) by deleting older entries.
* Update `GetRFCounts(minHits int, limit int)`: Allows the frontend to request top aggregates without loading every 1-hit noise code.

#### [MODIFY] pkg/webui/server.go
* Update `/api/telemetry/rf_aggregates` to accept query parameters: `?min_hits=...&limit=...`.
* Add `POST /api/telemetry/rf_aggregates/purge`: Calls `PruneRFEphemeralNoise`.
* Add `DELETE /api/telemetry/rf_events`: Clears historic event logs if requested by the user.

---

### 4. Drag-and-Drop Visual Automation Builder ("FleetFlow")

#### Backend Rule Engine Extension
#### [MODIFY] pkg/config/config.go
* Extend rule definitions to support compound conditions and logic blocks:
  ```go
  type Condition struct {
      Type     string `json:"type"`     // "weather", "sun_position", "sensor_state", "time_window"
      Operator string `json:"operator"` // "equals", "is_raining", "is_down", "is_up", "between"
      Value    string `json:"value"`
  }

  type AutomationRule struct {
      ID          string       `json:"id"`
      Name        string       `json:"name"`
      Enabled     bool         `json:"enabled"`
      TriggerType string       `json:"trigger_type"` // "rf_code", "interval", "weather_change"
      TriggerCode uint32       `json:"trigger_code,omitempty"`
      LogicMode   string       `json:"logic_mode"`   // "AND", "OR"
      Conditions  []Condition  `json:"conditions"`
      Actions     []RuleAction `json:"actions"`
      CanvasData  string       `json:"canvas_data"`  // Serialized flow graph positions and wires
  }
  ```

#### [MODIFY] pkg/rules/engine.go
* Evaluate conditions prior to dispatching actions:
  * Check weather state (e.g. rain, overcast, clear).
  * Check sun position (sun up / sun down based on solar zenith algorithm).
  * Check sensor state.
* Add native action handler for WLED:
  * Dispatches JSON state payloads directly to WLED devices (`POST http://<wled-target>/json/state`) with color, brightness, and effect settings.

#### Environment & Solar Service
#### [NEW] pkg/environment/environment.go
* Self-contained solar zenith / elevation calculator computing solar elevation angle from latitude, longitude, and system UTC time (no external API dependency).
* Periodic Open-Meteo weather fetcher (free, keyless API) polling current precipitation, cloud cover, and weather code every 15 minutes.

#### Frontend Visual Flow Editor
#### [MODIFY] web/public/index.html
* Add "Automations" tab to the top navigation header (`#tab-sec-automations`).
* Visual Canvas Component:
  * Left palette containing draggable node types:
    * Triggers: RF Sensor Code, Schedule/Cron, Weather Change, Sun Position.
    * Logic Gates: AND Gate, OR Gate, Cooldown Gate, Delay.
    * Actuators: WLED Strip (Color picker, Brightness, Effect), Audio Tone (/choola), Robot Action (Servo, Drive), RF Transmitter Pulse.
  * Interactive SVG/Canvas workspace: Drag nodes, connect input/output ports with animated Bezier wires, inspect properties, and click "Test Flow".
  * Flow persistence: Persisted to `fleethub_config.json` via `/api/rules/save`.

---

## Verification Plan

### Automated Tests
* Run unit tests on database retention and noise pruning:
  `go test -v ./pkg/db/...`
* Run unit tests on expanded rule engine logic (evaluating AND conditions, weather triggers, and WLED action formatting):
  `go test -v ./pkg/rules/...`

### Manual Verification
1. Inspect the RF Mesh Gateway Topology tab: Confirm `#node-fleethub` only shows its title and status dot without the removed text underneath.
2. Confirm `cambot3.local` appears in the Multi-Bot Fleet Registry and responds to ping/status monitoring.
3. Transmit test RF pulses: Confirm that `rf_events` records properly and does not grow beyond the configured retention limit.
4. Verify Signal Frequency Aggregates: Confirm the UI remains responsive without generating dozens of unconstrained tile cards, and verify the noise filtering/purge action.
5. Create an automation flow connecting: `[RF Sensor]` + `[Weather: Rain]` + `[Sun: Down]` -> `[AND Gate]` -> `[WLED: Pink (#FF1493)]` and test execution.
