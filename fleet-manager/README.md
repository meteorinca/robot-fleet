# Fleet Manager

Go Wi-Fi fleet monitor with an embedded dashboard and persistent JSON inventory.
Defaults to `simplebot1..9.local`, `rfbot1..9.local`, and `speakerbot1..9.local`.

From `fleet-manager`:

```powershell
go run ./cmd/fleet-manager serve
```

Open `http://127.0.0.1:8080`. Each response updates the dashboard through SSE and
the atomically replaced `fleet_state.generated.json`, without waiting for the
rest of the sweep. The dashboard initially shows discovered bots; select
"All targets" to see candidates that have never responded.

```powershell
go run ./cmd/fleet-manager scan
go run ./cmd/fleet-manager serve -interval 15s -timeout 3s -workers 12
go run ./cmd/fleet-manager serve -platforms simplebot,rfbot,speakerbot,newbot
go build -o fleet-manager.exe ./cmd/fleet-manager
```

Device numbers always start at 1. `-max-device` may reduce the range but cannot
exceed 9. `-state` changes the inventory path. `-bot-port` defaults to 80.
`-listen` defaults to loopback and can be changed for LAN access.

Each probe freshly resolves the mDNS hostname using multicast on active IPv4
interfaces, racing the OS resolver. It checks TCP port 80 first, then requests
`/api/info`, falling back to `/status` and `/info`. A single per-bot deadline
includes resolution, ping, and telemetry. HTTP URLs and Host headers retain the
mDNS name; observed IPs are informational and never become persistent targets.

`status` on the inventory entry is reachability (`unknown`, `online`, `offline`).
The complete unified firmware schema is stored under `telemetry`, including its
own `status`, `firmware`, `version`, `epoch`, and `time_synced`. A TCP response
marks the target discovered even if telemetry fails; `error` records the failure.
Telemetry must match the platform, hostname, and device number being probed.
Older deployed firmware with only `/status` is retained as `legacy_telemetry`
with a `reported_version`. Missing unified fields remain absent, and `error`
records the schema failure. Full telemetry replaces legacy data after upgrading.

Previously discovered bots retain their telemetry, first/last-seen times, and
offline-since timestamp across failures and restarts. `last_telemetry_at` indicates
the age of retained telemetry. Restarted entries remain `unknown` until checked.
All targets are retained; `exists` distinguishes discovered bots from candidates.
Offline timestamps record when the service observed the outage, not its exact
start. Inventory corruption stops startup rather than discarding history.

API: `GET /api/fleet`, `GET /api/events` (SSE), `POST /api/scan` (queues a scan).
Automatic sweeps never overlap. Slow clients and JSON writes do not block probes.
Scan requests are coalesced into one queued follow-up sweep.

This implementation covers discovery and monitoring. Firmware builds and Wi-Fi
OTA deployment remain in the existing tools. No USB discovery or flashing runs.

Verification:

```powershell
go test ./...
go vet ./...
```

`go test -race ./...` additionally requires CGO and a host C compiler.
