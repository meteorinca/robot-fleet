# mDNS-First Addressing & Automatic IP Resolution

FleetHub AND Robohub BOTH currently use IP addresses as the primary target for all device communication (RF polling, rule actions, device control, audio streaming). When a bot's IP changes via DHCP, the stored IP becomes stale and the bot is marked offline — even though the mDNS hostname (e.g., `simplebot1.local`) remains valid.

This change flips the priority: **mDNS hostnames become the default addressing method**, with IP used as fallback. It also adds an **hourly auto-resolve cycle** that re-resolves mDNS hostnames to fresh IPs, automatically healing stale entries.

## Proposed Changes

### Config: Add `PreferMDNS` Global Setting

#### [MODIFY] [config.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/config/config.go)

- Add `PreferMDNS bool` field to `Config` struct (default `true`)
- Add a new method `ResolveAddress(bot RobotNode) string` that returns the best address to use: when `PreferMDNS` is true, it returns `hostname:port` first (if hostname is set), falling back to `IP:port`; when false, it returns `IP:port` first (current behavior)
- Add a new method `BuildTargetCandidates(bot RobotNode) []string` that centralizes the target ordering logic used by every subsystem. When `PreferMDNS` is true, hostname goes first; when false, IP goes first. This removes the ~6 duplicate implementations of target-ordering logic scattered across packages

---

### Pinger: Add Hourly Auto-Resolve & IP Update on Successful mDNS Ping

#### [MODIFY] [pinger.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/pinger/pinger.go)

- Add an hourly ticker (`resolveAllTicker`) alongside the existing 15-second ping ticker
- When the hourly ticker fires, call a new `ResolveAndUpdateIPs()` method that:
  1. For each bot with a hostname, resolves the hostname via `net.LookupHost(hostname)`
  2. If resolution succeeds and the IP differs from the stored `bot.IP`, updates `bot.IP` via `cfg.UpsertBot` and logs the change
  3. Saves config and runtime state
- Modify `PingDevice` to **update the bot's stored IP** when a successful ping comes back via mDNS hostname (i.e., the resolved IP differs from the stored IP). Currently it only returns the `discoveredIP` but the caller in `PingAllSync` already updates the bot — we need to ensure this also persists properly
- When the pinger successfully reaches a bot via mDNS but the stored IP is stale, mark bot as `online` (not `offline`)


we also have known_devices.json that if I rememeber correctly is updated by both the user manually, via webui, and the problematic automated pinger (i don't rememebr if we fixed this issue already) but what we need done is an initial file for the humans (same file) but a generated file from the pinger that keeps track of the bot's updated IP address, last ping, latency, etc, also this pinger might find more devices then the user using autodiscovery for wled, and/or other potential devices which are stored and displayed in our webui. This file may be initialized from the userfile, but then is regularly updated by the pinger based on IP discovery, etc. If user file is edited manually or via webui and press save, the next time pinger runs, it'll regen the file and include the added sensor or whatever. 

---

### RF Poller: Flip Target Order to mDNS-First

#### [MODIFY] [poller.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/rfpoller/poller.go)

- In `pollBot()`, replace the inline target-building logic with `cfg.BuildTargetCandidates(bot)` to respect the global `PreferMDNS` setting

---

### Rules Engine: Flip Target Order to mDNS-First

#### [MODIFY] [engine.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/rules/engine.go)

- In `executeAction()`, replace the inline candidate-building logic with `cfg.BuildTargetCandidates(*matchedBot)` to respect the `PreferMDNS` setting

---

### WebUI Server: Flip Target Order to mDNS-First  

#### [MODIFY] [server.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/webui/server.go)

- Replace `buildBotTargets()`, `dispatchToBot()`, and the inline target-building in `handleDeviceControl()` to use `cfg.BuildTargetCandidates(bot)` — the centralized method that respects `PreferMDNS`

---

### Audio Streamer: Flip Target Order to mDNS-First

#### [MODIFY] [streamer.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/pkg/audio/streamer.go)

- Replace `buildTargetCandidates()` with a call to `cfg.BuildTargetCandidates(bot)`. The streamer currently doesn't have access to the config, so it will accept targets from the caller (service.go passes the RobotNode). We'll refactor to pass the pre-built candidate list from the config, or give the streamer a reference to config

> [!IMPORTANT]
> **Design Decision**: Rather than passing `*config.Config` into the streamer (tight coupling), the audio `Service` will call `cfg.BuildTargetCandidates(bot)` and pass the resulting `[]string` to the streamer, keeping the streamer decoupled.

---

### Main: Wire Up Hourly Resolver

#### [MODIFY] [main.go](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/fleethub/cmd/fleethub/main.go)

- The hourly resolve cycle is started inside pinger.Start(), so no change needed in main.go

---

## Summary of Behavioral Changes

| Aspect | Before | After |
|--------|--------|-------|
| Target ordering | IP → FallbackIP → mDNS hostname | **mDNS hostname → IP → FallbackIP** (when `PreferMDNS` is `true`) |
| Stale IP detection | Never | **Hourly auto-resolve** re-resolves all hostnames and updates IPs |
| Bot marked offline despite working mDNS | Yes ❌ | No ✅ — pinger tries mDNS first, updates IP if resolved | NO EMOTICONS IN WEBUI OR FILE, etc
| Config setting | N/A | `prefer_mdns: true` (default) |

## Open Questions

> [!IMPORTANT]
> **Audio Streamer Config Access**: The audio streamer currently has no config reference. 
> **Action**: Have `audio.Service` (which already has config access via the bot node) call `config.BuildTargetCandidates()` and pass the `[]string` to the streamer

## Verification Plan

### Build Verification
Use existing build scripts to build

### Unit Tests  
No additional tests needed