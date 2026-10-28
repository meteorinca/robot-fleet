/**
 * IsoView - Floorplan Control Dashboard
 * Clean, minimal, alert-free, SVG clipping, stove visual heat hue, hardware bots layer
 */

const BOT_TYPE_COLORS = {
  rfbot: "#38bdf8",      // Cyan
  speakerbot: "#a855f7", // Violet
  simplebot: "#f59e0b",  // Amber
  cambot: "#10b981",     // Emerald
  sensor: "#f43f5e"      // Rose / Flame
};

class IsoViewController {
  constructor() {
    this.config = null;
    this.bots = [];
    this.showBots = false;
    this.roomStates = {}; // roomId -> boolean (lit/active)
    this.selectedRoomId = null;
    this.stoveActive = false;

    // Viewport transform
    this.viewBox = { x: 0, y: 0, w: 1189, h: 1323 };
    this.initialViewBox = { ...this.viewBox };
    this.isPanning = false;
    this.startPoint = { x: 0, y: 0 };

    // DOM Elements
    this.svg = document.getElementById("svgStage");
    this.defs = document.getElementById("svgDefs");
    this.layersGroup = document.getElementById("layersGroup");
    this.drawer = document.getElementById("sideDrawer");

    this.initAudio();
    this.loadAllData();
  }

  // Audio synthesizer for UI haptics
  initAudio() {
    try {
      this.audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    } catch (e) {
      this.audioCtx = null;
    }
  }

  playFeedbackSound(type = "click") {
    if (!this.audioCtx) return;
    if (this.audioCtx.state === "suspended") {
      this.audioCtx.resume();
    }
    const osc = this.audioCtx.createOscillator();
    const gain = this.audioCtx.createGain();
    osc.connect(gain);
    gain.connect(this.audioCtx.destination);

    if (type === "toggleOn") {
      osc.frequency.setValueAtTime(440, this.audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(880, this.audioCtx.currentTime + 0.07);
      gain.gain.setValueAtTime(0.06, this.audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, this.audioCtx.currentTime + 0.09);
      osc.start();
      osc.stop(this.audioCtx.currentTime + 0.09);
    } else if (type === "toggleOff") {
      osc.frequency.setValueAtTime(600, this.audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(300, this.audioCtx.currentTime + 0.07);
      gain.gain.setValueAtTime(0.06, this.audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, this.audioCtx.currentTime + 0.09);
      osc.start();
      osc.stop(this.audioCtx.currentTime + 0.09);
    } else if (type === "stoveOn") {
      osc.type = "sine";
      osc.frequency.setValueAtTime(220, this.audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(330, this.audioCtx.currentTime + 0.15);
      gain.gain.setValueAtTime(0.07, this.audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, this.audioCtx.currentTime + 0.2);
      osc.start();
      osc.stop(this.audioCtx.currentTime + 0.2);
    } else if (type === "stoveOff") {
      osc.type = "sine";
      osc.frequency.setValueAtTime(330, this.audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(180, this.audioCtx.currentTime + 0.15);
      gain.gain.setValueAtTime(0.07, this.audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, this.audioCtx.currentTime + 0.2);
      osc.start();
      osc.stop(this.audioCtx.currentTime + 0.2);
    }
  }

  async loadAllData() {
    // 1. Rooms
    try {
      const resp = await fetch("rooms.json?t=" + Date.now());
      if (resp.ok) {
        this.config = await resp.json();
      }
    } catch (err) {
      console.log("Loading rooms.json error:", err);
    }

    // 2. Hardware Bots
    try {
      const respB = await fetch("bots.json?t=" + Date.now());
      if (respB.ok) {
        const bData = await respB.json();
        if (bData && bData.bots) {
          this.bots = bData.bots;
        }
      }
    } catch (err) {
      console.log("Loading bots.json error:", err);
    }

    if (!this.config || !this.config.rooms) {
      return;
    }

    // Initialize room states
    this.config.rooms.forEach(room => {
      const primaryDevice = room.devices ? room.devices[0] : null;
      this.roomStates[room.id] = primaryDevice ? primaryDevice.state : true;
    });

    this.renderStage();
    this.bindEvents();
    this.updateHUD();
  }

  renderStage() {
    const { w, h } = this.config.image;
    this.svg.setAttribute("viewBox", `0 0 ${w} ${h}`);

    // Clear dynamic layers
    this.layersGroup.innerHTML = "";

    // 1. Base Layer (Off image)
    const baseImg = document.createElementNS("http://www.w3.org/2000/svg", "image");
    baseImg.setAttribute("href", "isoviewOff.png");
    baseImg.setAttribute("width", w);
    baseImg.setAttribute("height", h);
    baseImg.setAttribute("id", "baseOffImage");
    this.layersGroup.appendChild(baseImg);

    // 2. Room Lit Layers & ClipPaths
    this.config.rooms.forEach(room => {
      const clipId = `clip-${room.id}`;
      let clipPath = document.getElementById(clipId);
      if (!clipPath) {
        clipPath = document.createElementNS("http://www.w3.org/2000/svg", "clipPath");
        clipPath.setAttribute("id", clipId);
        const clipPoly = document.createElementNS("http://www.w3.org/2000/svg", "polygon");
        const pts = room.polygon.map(p => `${p[0]},${p[1]}`).join(" ");
        clipPoly.setAttribute("points", pts);
        clipPoly.setAttribute("id", `clip-poly-${room.id}`);
        clipPath.appendChild(clipPoly);
        this.defs.appendChild(clipPath);
      }

      // Lit overlay image clipped to this room's polygon
      const litImg = document.createElementNS("http://www.w3.org/2000/svg", "image");
      litImg.setAttribute("href", "isoViewOn.png");
      litImg.setAttribute("width", w);
      litImg.setAttribute("height", h);
      litImg.setAttribute("clip-path", `url(#${clipId})`);
      litImg.setAttribute("class", "room-lit-layer");
      litImg.setAttribute("id", `lit-${room.id}`);
      litImg.style.opacity = this.roomStates[room.id] ? "1" : "0";
      this.layersGroup.appendChild(litImg);
    });

    // 3. Stove Visual Heat Hue Group (Stationary, anchored to stove node position)
    const stoveNode = this.bots.find(b => b.id.includes("stove") || b.type === "sensor");
    const stoveX = stoveNode ? stoveNode.x : 128;
    const stoveY = stoveNode ? stoveNode.y : 498;

    const stoveGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    stoveGroup.setAttribute("id", "stoveHueGroup");
    stoveGroup.setAttribute("class", `stove-hue-group ${this.stoveActive ? 'active' : ''}`);
    stoveGroup.innerHTML = `
      <!-- Soft ambient thermal heat bloom -->
      <ellipse cx="${stoveX}" cy="${stoveY}" rx="52" ry="30" fill="url(#stoveGlowGradient)" filter="url(#stoveHeatBlur)" class="stove-heat-core" />
      <!-- Pulsing thermal radiation rings -->
      <ellipse cx="${stoveX}" cy="${stoveY}" rx="34" ry="19" fill="none" stroke="#ff3b00" stroke-width="2" class="stove-heat-ring ring-1" />
      <ellipse cx="${stoveX}" cy="${stoveY}" rx="52" ry="29" fill="none" stroke="#ff8800" stroke-width="1.5" class="stove-heat-ring ring-2" />
    `;
    this.layersGroup.appendChild(stoveGroup);

    // 4. Presence Indicator Layer
    const presenceGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    presenceGroup.setAttribute("id", "presenceGroup");
    this.config.rooms.forEach(room => {
      if (room.presence && room.presence.active && room.presence.pos) {
        const [px, py] = room.presence.pos;
        const ring = document.createElementNS("http://www.w3.org/2000/svg", "circle");
        ring.setAttribute("cx", px);
        ring.setAttribute("cy", py);
        ring.setAttribute("r", 18);
        ring.setAttribute("fill", "rgba(56, 189, 248, 0.15)");
        ring.setAttribute("stroke", "#38bdf8");
        ring.setAttribute("class", "presence-indicator");
        ring.setAttribute("id", `presence-${room.id}`);
        presenceGroup.appendChild(ring);
      }
    });
    this.layersGroup.appendChild(presenceGroup);

    // 5. Interactive Room Hit Polygons
    const hitGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    hitGroup.setAttribute("id", "hitGroup");

    this.config.rooms.forEach(room => {
      const poly = document.createElementNS("http://www.w3.org/2000/svg", "polygon");
      const pts = room.polygon.map(p => `${p[0]},${p[1]}`).join(" ");
      poly.setAttribute("points", pts);
      poly.setAttribute("class", room.id === "projector" ? "projector-polygon" : "room-polygon");
      if (this.roomStates[room.id]) poly.classList.add("is-active");
      poly.setAttribute("id", `poly-${room.id}`);
      poly.setAttribute("data-room-id", room.id);
      hitGroup.appendChild(poly);
    });
    this.layersGroup.appendChild(hitGroup);

    // 6. Hardware Bot Nodes Layer (ESP32 / Arduino hardware locations)
    const botsGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    botsGroup.setAttribute("id", "botNodesGroup");
    botsGroup.style.display = this.showBots ? "block" : "none";

    this.bots.forEach(bot => {
      const color = BOT_TYPE_COLORS[bot.type] || "#38bdf8";
      const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
      g.setAttribute("class", "bot-node-group");
      g.setAttribute("transform", `translate(${bot.x}, ${bot.y})`);
      g.innerHTML = `
        <circle cx="0" cy="0" r="13" fill="${color}" fill-opacity="0.28" stroke="${color}" stroke-width="2" class="bot-pin-circle" />
        <circle cx="0" cy="0" r="4" fill="${color}" />
        <text x="0" y="22" text-anchor="middle" fill="#ffffff" font-size="9.5" font-weight="600" style="paint-order: stroke; stroke: #0a0d14; stroke-width: 3px;">${bot.label}</text>
      `;

      g.onclick = (e) => {
        e.stopPropagation();
        this.openBotDetails(bot);
      };

      botsGroup.appendChild(g);
    });
    this.layersGroup.appendChild(botsGroup);
  }

  toggleRoom(roomId) {
    const newState = !this.roomStates[roomId];
    this.roomStates[roomId] = newState;

    const litEl = document.getElementById(`lit-${roomId}`);
    if (litEl) {
      litEl.style.opacity = newState ? "1" : "0";
    }

    const polyEl = document.getElementById(`poly-${roomId}`);
    if (polyEl) {
      if (newState) polyEl.classList.add("is-active");
      else polyEl.classList.remove("is-active");
    }

    this.playFeedbackSound(newState ? "toggleOn" : "toggleOff");
    this.updateHUD();

    if (this.selectedRoomId === roomId) {
      this.populateDrawer(roomId);
    }
  }

  toggleStove() {
    this.stoveActive = !this.stoveActive;
    const stoveHue = document.getElementById("stoveHueGroup");
    const stoveBtn = document.getElementById("btnStoveToggle");

    if (stoveHue) {
      if (this.stoveActive) stoveHue.classList.add("active");
      else stoveHue.classList.remove("active");
    }

    if (stoveBtn) {
      if (this.stoveActive) stoveBtn.classList.add("active");
      else stoveBtn.classList.remove("active");
    }

    this.playFeedbackSound(this.stoveActive ? "stoveOn" : "stoveOff");
  }

  toggleBotsLayer() {
    this.showBots = !this.showBots;
    const g = document.getElementById("botNodesGroup");
    const btn = document.getElementById("btnToggleBots");
    if (g) g.style.display = this.showBots ? "block" : "none";
    if (btn) btn.classList.toggle("active", this.showBots);
  }

  updateHUD() {
    const totalRooms = this.config.rooms.filter(r => !r.isSpecial).length;
    const litRooms = this.config.rooms.filter(r => !r.isSpecial && this.roomStates[r.id]).length;
    const projState = this.roomStates["projector"];
    const presenceCount = this.config.rooms.filter(r => r.presence && r.presence.active).length;

    const litEl = document.getElementById("hudLitCount");
    const projEl = document.getElementById("hudProjState");
    const projChip = document.getElementById("hudProjChip");
    const presEl = document.getElementById("hudPresCount");

    if (litEl) litEl.textContent = `${litRooms} / ${totalRooms}`;
    if (projEl) {
      projEl.textContent = projState ? "ACTIVE" : "OFF";
    }
    if (projChip) {
      if (projState) projChip.classList.add("active");
      else projChip.classList.remove("active");
    }
    if (presEl) presEl.textContent = `${presenceCount}`;
  }

  openDrawer(roomId) {
    this.selectedRoomId = roomId;
    this.populateDrawer(roomId);
    this.drawer.classList.remove("hidden");
  }

  openBotDetails(bot) {
    document.getElementById("drawerTitle").textContent = bot.label;
    document.getElementById("drawerCategory").textContent = `${bot.type.toUpperCase()} | Room: ${bot.room || 'General'}`;

    const list = document.getElementById("drawerDeviceList");
    list.innerHTML = `
      <div class="device-card">
        <div class="device-info">
          <div class="device-name">IP / Bridge</div>
          <div class="device-bot">${bot.ip || 'Local WiFi'}</div>
        </div>
      </div>
      <div class="device-card">
        <div class="device-info">
          <div class="device-name">Coordinates</div>
          <div class="device-bot">X: ${bot.x}, Y: ${bot.y}</div>
        </div>
      </div>
      ${bot.desc ? `
      <div style="font-size: 0.76rem; color: var(--text-secondary); padding: 8px 4px;">
        ${bot.desc}
      </div>
      ` : ''}
    `;

    this.drawer.classList.remove("hidden");
  }

  closeDrawer() {
    this.selectedRoomId = null;
    this.drawer.classList.add("hidden");
  }

  populateDrawer(roomId) {
    const room = this.config.rooms.find(r => r.id === roomId);
    if (!room) return;

    document.getElementById("drawerTitle").textContent = room.label;
    document.getElementById("drawerCategory").textContent = room.category || "Zone";

    const deviceList = document.getElementById("drawerDeviceList");
    deviceList.innerHTML = "";

    room.devices.forEach(dev => {
      const isLit = this.roomStates[roomId];
      const card = document.createElement("div");
      card.className = "device-card";
      card.innerHTML = `
        <div class="device-info">
          <div class="device-name">${dev.label}</div>
          <div class="device-bot">${dev.bot} ${dev.channel ? '- Ch ' + dev.channel : ''}</div>
        </div>
        <label class="toggle-switch">
          <input type="checkbox" id="switch-${dev.id}" ${isLit ? 'checked' : ''}>
          <span class="slider ${dev.type === 'projector' ? 'projector-slider' : ''}"></span>
        </label>
      `;

      card.querySelector("input").addEventListener("change", () => {
        this.toggleRoom(roomId);
      });
      deviceList.appendChild(card);
    });
  }

  bindEvents() {
    let isDragging = false;
    let didMove = false;
    let startCoords = { x: 0, y: 0 };

    this.svg.addEventListener("pointerdown", (e) => {
      isDragging = true;
      didMove = false;
      startCoords = { x: e.clientX, y: e.clientY };
    });

    window.addEventListener("pointermove", (e) => {
      if (!isDragging) return;
      const dx = e.clientX - startCoords.x;
      const dy = e.clientY - startCoords.y;
      if (Math.hypot(dx, dy) > 5) {
        didMove = true;
      }
      this.viewBox.x -= dx * (this.viewBox.w / this.svg.clientWidth);
      this.viewBox.y -= dy * (this.viewBox.h / this.svg.clientHeight);
      startCoords = { x: e.clientX, y: e.clientY };
      this.updateViewBox();
    });

    window.addEventListener("pointerup", () => {
      isDragging = false;
    });

    // Zoom via wheel
    this.svg.addEventListener("wheel", (e) => {
      e.preventDefault();
      const zoomFactor = e.deltaY > 0 ? 1.08 : 0.92;
      this.zoomAtPoint(zoomFactor, e.clientX, e.clientY);
    }, { passive: false });

    // Click / Tap on hit polygons
    this.svg.addEventListener("click", (e) => {
      if (didMove) return;

      const target = e.target.closest(".room-polygon, .projector-polygon");
      if (target) {
        const roomId = target.getAttribute("data-room-id");
        if (roomId) {
          this.toggleRoom(roomId);
        }
      }
    });

    // Right-click opens compact details drawer
    this.svg.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      const target = e.target.closest(".room-polygon, .projector-polygon");
      if (target) {
        const roomId = target.getAttribute("data-room-id");
        if (roomId) this.openDrawer(roomId);
      }
    });

    // Stove Button
    document.getElementById("btnStoveToggle").addEventListener("click", () => {
      this.toggleStove();
    });

    // Bots Layer Toggle
    const btnBots = document.getElementById("btnToggleBots");
    if (btnBots) {
      btnBots.addEventListener("click", () => {
        this.toggleBotsLayer();
      });
    }

    // All On
    document.getElementById("btnAllOn").addEventListener("click", () => {
      this.config.rooms.forEach(r => {
        this.roomStates[r.id] = true;
        const lit = document.getElementById(`lit-${r.id}`);
        if (lit) lit.style.opacity = "1";
        const poly = document.getElementById(`poly-${r.id}`);
        if (poly) poly.classList.add("is-active");
      });
      this.playFeedbackSound("toggleOn");
      this.updateHUD();
    });

    // All Off
    document.getElementById("btnAllOff").addEventListener("click", () => {
      this.config.rooms.forEach(r => {
        this.roomStates[r.id] = false;
        const lit = document.getElementById(`lit-${r.id}`);
        if (lit) lit.style.opacity = "0";
        const poly = document.getElementById(`poly-${r.id}`);
        if (poly) poly.classList.remove("is-active");
      });
      this.playFeedbackSound("toggleOff");
      this.updateHUD();
    });

    // Zoom Controls
    document.getElementById("btnZoomIn").addEventListener("click", () => {
      this.zoomAtPoint(0.8, window.innerWidth / 2, window.innerHeight / 2);
    });
    document.getElementById("btnZoomOut").addEventListener("click", () => {
      this.zoomAtPoint(1.25, window.innerWidth / 2, window.innerHeight / 2);
    });
    document.getElementById("btnZoomReset").addEventListener("click", () => {
      this.viewBox = { ...this.initialViewBox };
      this.updateViewBox();
    });

    // Drawer close
    document.getElementById("closeDrawerBtn").addEventListener("click", () => {
      this.closeDrawer();
    });
  }

  zoomAtPoint(scale, clientX, clientY) {
    const pt = this.svg.createSVGPoint();
    pt.x = clientX;
    pt.y = clientY;
    const svgPt = pt.matrixTransform(this.svg.getScreenCTM().inverse());

    this.viewBox.x = svgPt.x - (svgPt.x - this.viewBox.x) * scale;
    this.viewBox.y = svgPt.y - (svgPt.y - this.viewBox.y) * scale;
    this.viewBox.w *= scale;
    this.viewBox.h *= scale;
    this.updateViewBox();
  }

  updateViewBox() {
    this.svg.setAttribute("viewBox", `${this.viewBox.x} ${this.viewBox.y} ${this.viewBox.w} ${this.viewBox.h}`);
  }
}

window.addEventListener("DOMContentLoaded", () => {
  window.isoView = new IsoViewController();
});
