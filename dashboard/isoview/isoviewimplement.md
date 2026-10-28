# IsoView — Isometric Home Control Dashboard

A small touch/click dashboard built on an isometric floor plan. Each room is a polygon you can tap to toggle its devices (lights, projector). The room's live state shows by "unmasking" that part of the **On** image over the **Off** base image.

Runs two ways:
1. **Embedded** inside FleetHub (tab/iframe/route).
2. **Standalone** webapp, served by a tiny Go binary (or any static server) that talks to the same APIs.

---

## 1. System Context (Summary)

### Control topology
```
              ┌──────────── FleetHub (primary bridge / brain) ────────────┐
              │  REST/WS API · device registry · rules · logs             │
              └───▲──────────▲───────────▲────────────▲───────────────────┘
                  │ WiFi     │ WiFi      │ WiFi       │ WiFi
          ┌───────┴──┐  ┌────┴─────┐ ┌───┴─────┐ ┌────┴──────┐
          │SimpleBot │  │ CamBot   │ │ RFBot   │ │SpeakerBot │
          │2 servos→ │  │ camera   │ │433 MHz  │ │ TTS/audio │
          │2 lights  │  │ stream   │ │ TX + RX │ │           │
          │+presence*│  └──────────┘ └──▲───┬──┘ └───────────┘
          └──────────┘                  │RF │RF
                              ┌─────────┴┐ ┌┴────────────────┐
                              │Arduino RF│ │RF appliances    │
                              │sensors   │ │(projector, etc.)│
                              │(stove…)  │ └─────────────────┘
                              └──────────┘
```
`*` = presence detection still needs to be added to the firmware.

### Devices
| Bot | Role | Feedback | Notes |
|-----|------|----------|-------|
| **SimpleBot** | Flips 2 physical light switches using servos | None today (state is *assumed*) | Add a **presence sensor** (PIR/mmWave) to firmware and report it |
| **CamBot** | Live camera view | Video stream | IsoView can open the stream for a room |
| **RFBot** | 433 MHz backbone: TX, RX, **learn** new codes | RX events | Any Arduino + 433 MHz TX becomes a sensor. Also works as a **fallback bridge** to other ESP32s over WiFi when FleetHub is down |
| **SpeakerBot** | Spoken alerts | — | Gets commands from FleetHub or directly from RFBot |
| **Projector / lights** | Already controllable per room | Depends on path | Controlled through SimpleBot (servo) or RFBot (RF codes) |

### Bridging rule
- **FleetHub online** → FleetHub handles all routing and rules (RF event → action).
- **FleetHub offline** → RFBot falls back to peer mode and calls other bots' HTTP APIs directly.

**Example (stove alert):** the stove is on → an Arduino sends an RF code → RFBot1 receives it →
- hub up: RFBot → FleetHub → rule → SpeakerBot says *"stove is on"*
- hub down: RFBot → SpeakerBot directly (WiFi API) → *"stove is on"*

IsoView never talks RF itself. It uses the **same HTTP API as FleetHub**. In standalone mode it can also talk to bots directly (see §4).

---

## 2. Visual Approach

Assets (same size, pixel-aligned):
- `isoviewOff.png` — base layer, all rooms dark/off.
- `isoViewOn.png` — every room lit, projector on.

Rendering:
1. Show `isoviewOff.png` as the base layer.
2. Put `isoViewOn.png` on top, clipped to the polygons of rooms that are currently **on**.
3. Use an SVG overlay with the same `viewBox` as the image's natural size. Each room is a `<polygon>` that:
   - is the clickable hit target (transparent fill, glow on hover/press)
   - is added to the `<clipPath>` that reveals the On image when that room is on.
4. Use a **separate small polygon for the projector** (the screen and beam in the living room) so it can be toggled on its own, apart from the room lights.
5. Fade with a CSS `opacity` transition on each room's clipped `<image>` (one clipped image per room keeps the fade per-room and cheap).

Why SVG: polygons scale with the image, give exact hit-testing, need no canvas redraws, and work the same for touch and mouse.

```html
<svg viewBox="0 0 W H" id="iso">
  <image href="isoviewOff.png" width="W" height="H"/>
  <defs><clipPath id="clip-livingroom"><polygon points="..."/></clipPath></defs>
  <image href="isoViewOn.png" width="W" height="H"
         clip-path="url(#clip-livingroom)" class="lit" data-room="livingroom"/>
  <polygon class="hit" data-room="livingroom" points="..."/>
</svg>
```

---

## 3. Room / Device Config (`rooms.json`)

All polygons and device bindings live in one config file, so new rooms need no code changes.

```json
{
  "image": { "w": 1190, "h": 1330 },
  "rooms": [
    {
      "id": "livingroom",
      "label": "Living Room",
      "polygon": [[500,250],[730,230],[760,560],[430,560]],
      "devices": [
        { "id": "lr_light",  "type": "light",     "bot": "simplebot1", "channel": 1, "feedback": false },
        { "id": "projector", "type": "projector", "bot": "rfbot1", "code": "proj_power",
          "polygon": [[440,260],[510,250],[500,460],[450,470]] }
      ],
      "presence": { "bot": "simplebot1", "sensor": "pir1" },
      "camera":   { "bot": "cambot1" }
    }
  ]
}
```

Rooms visible in the plan: Bedroom NW, Bedroom NE (master), Bedroom S, Living Room (projector), Kitchen/Dining, Balcony N, Balcony E, and the bathrooms/closets (optional).

**Polygon authoring:** add an `?edit=1` mode. Click points on the image, then copy out the JSON array. This avoids measuring by hand.

---

## 4. API Integration

Reuse FleetHub's existing endpoints. Check the exact paths in `fleethub/` before you implement.

| Action | Via FleetHub | Direct (standalone fallback) |
|--------|--------------|------------------------------|
| List bots + status | `GET /api/bots` | `GET http://<bot>/status` |
| Toggle light (servo) | `POST /api/bots/{id}/command` `{cmd:"light",ch,state}` | `POST http://<simplebot>/light` |
| Send RF code | `POST /api/bots/{rfbot}/command` `{cmd:"tx",code}` | `POST http://<rfbot>/tx` |
| Learn RF code | `POST /api/bots/{rfbot}/command` `{cmd:"learn"}` | `POST http://<rfbot>/learn` |
| Presence | `GET /api/bots/{id}/status` or WS event | `GET http://<simplebot>/status` |
| Live updates | WebSocket / SSE from FleetHub | Poll every 2–5 s |

**Base URL detection:** if the app is served from FleetHub, use relative `/api`. Otherwise use `config.hubUrl`. If the hub can't be reached, switch to direct mode using bot IPs from `rooms.json` (or the last cached `/api/bots` result).

### State model
- `assumed` — no feedback (SimpleBot servos, RF-only devices). The UI applies the change right away and shows a small "assumed" marker.
- `confirmed` — the device or a sensor reported the state.
- `presence` — a room with someone in it gets a soft pulse outline. Lights stay separate.
- `offline` — bot unreachable. The room polygon is hatched/greyed and taps are disabled.

---

## 5. File Layout

```
dashboard/isoview/
├── isoviewimplement.md   (this file)
├── index.html            # SVG stage + minimal HUD (hub status, mode)
├── isoview.css           # dark theme, glow on hover, fade transitions
├── isoview.js            # load rooms.json, build SVG, API client, state, edit mode
├── rooms.json            # polygons + device bindings
├── isoViewOn.png
├── isoviewOff.png
└── server/
    └── main.go           # optional standalone server (static + reverse proxy)
```

### Go standalone server (`server/main.go`, about 60 lines)
- Serves `dashboard/isoview/` as static files (`embed.FS` so it ships as a single binary).
- `/api/*` → reverse proxy to FleetHub (`-hub http://fleethub:PORT`). This avoids CORS.
- If the hub is down, `/api/direct/{bot}/*` proxies straight to the bot IP.
- Flags: `-addr :8090 -hub URL -config rooms.json`.

### FleetHub embedding
- Mount the same folder at `/isoview/` in FleetHub (static route) and add a nav link/tab.
- No separate build. The same files serve both modes.

---

## 6. Interaction

- **Tap/click a room:** toggles that room's main light(s).
- **Tap the projector polygon:** toggles the projector.
- **Long-press (500 ms) or right-click:** opens a small radial/popover with every device in the room, presence, the camera snapshot (CamBot), and "Learn RF" for unbound devices.
- Use pointer events only (`pointerdown/up`), so mouse and touch behave the same. Set `touch-action: manipulation`.
- Give haptic feedback with `navigator.vibrate(10)` where supported.

---

## 7. Firmware TODO

- **SimpleBot:** add a presence sensor (PIR or LD2410 mmWave) and expose `presence` in `/status`. Push changes to FleetHub. Optionally add a reed/light sensor so light state is confirmed instead of assumed.
- **RFBot:** confirm `learn`/`tx` endpoints. Keep a table of RF code → event name (e.g. `stove_on`). In peer-fallback mode, send events directly to SpeakerBot and other bots when FleetHub heartbeats fail.
- **FleetHub:** add a WS/SSE event stream (if missing) for state and presence. Add a rule `rf:stove_on → speakerbot.say("stove is on")`.

---

## 8. Implementation Steps

1. Scaffold `index.html`, `isoview.css`, `isoview.js`. Render the Off/On layers from `rooms.json`.
2. Build `?edit=1` polygon authoring and trace all rooms plus the projector.
3. Write the API client: hub detection, command send, polling/WS, direct fallback.
4. Add state rendering: assumed/confirmed/offline/presence styles.
5. Add the long-press device popover (camera, learn RF).
6. Write the Go standalone server with embed + proxy.
7. Add the FleetHub route and nav link.
8. Firmware: SimpleBot presence; RFBot fallback bridging.

## 9. Constraints
- No frameworks or build step. Vanilla HTML/CSS/JS plus Go stdlib only.
- Images load once and the per-room fades run on CSS opacity, so it runs well on cheap tablets and wall panels.
- All room and device data lives in `rooms.json`. No hard-coded rooms in the JS.
