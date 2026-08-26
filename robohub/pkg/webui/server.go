package webui

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/meteorinca/robot-fleet/robohub/pkg/config"
	"github.com/meteorinca/robot-fleet/robohub/pkg/db"
	"github.com/meteorinca/robot-fleet/robohub/pkg/listener"
	"github.com/meteorinca/robot-fleet/robohub/pkg/mdns"
	"github.com/meteorinca/robot-fleet/robohub/pkg/pinger"
	"github.com/meteorinca/robot-fleet/robohub/pkg/rfpoller"
	"github.com/meteorinca/robot-fleet/robohub/pkg/rules"
	"github.com/meteorinca/robot-fleet/robohub/pkg/telemetry"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Server handles WebUI HTTP API endpoints and WebSocket broadcasting.
type Server struct {
	cfg        *config.Config
	engine     *rules.Engine
	poller     *rfpoller.RFPoller
	database   *db.DB
	telemetry  *telemetry.Service
	pinger     *pinger.Pinger
	scanner    *mdns.Scanner
	assets     fs.FS
	clients    map[*websocket.Conn]bool
	clientsMu  sync.Mutex
	broadcast  chan interface{}
	pairActive bool
	pairCode   uint32
	pairMu     sync.Mutex
}

// NewServer creates a new web API server.
func NewServer(cfg *config.Config, engine *rules.Engine, poller *rfpoller.RFPoller, assets fs.FS) *Server {
	s := &Server{
		cfg:       cfg,
		engine:    engine,
		poller:    poller,
		assets:    assets,
		clients:   make(map[*websocket.Conn]bool),
		broadcast: make(chan interface{}, 100),
	}

	go s.broadcastLoop()
	return s
}

// SetDB connects the SQLite database to the server.
func (s *Server) SetDB(d *db.DB) {
	s.database = d
}

// SetTelemetry connects the telemetry service to the server.
func (s *Server) SetTelemetry(t *telemetry.Service) {
	s.telemetry = t
}

// SetPinger connects the fleet pinger to the server for on-demand health sweeps.
func (s *Server) SetPinger(p *pinger.Pinger) {
	s.pinger = p
}

// SetScanner connects the mDNS scanner to the server for on-demand fleet discovery.
func (s *Server) SetScanner(sc *mdns.Scanner) {
	s.scanner = sc
}

// Handler returns the http.Handler for all routes.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// API Routes
	mux.HandleFunc("/api/status", s.handleStatus)
	mux.HandleFunc("/api/bots", s.handleBots)
	mux.HandleFunc("/api/bots/scan", s.handleScanBots)
	mux.HandleFunc("/api/bots/update", s.handleUpdateBot)
	mux.HandleFunc("/api/bots/save_all", s.handleSaveAllBots)
	mux.HandleFunc("/api/bots/delete", s.handleDeleteBot)
	mux.HandleFunc("/api/bots/range", s.handleRangeBots)
	mux.HandleFunc("/api/bots/ping_single", s.handlePingSingleBot)
	mux.HandleFunc("/api/sensors", s.handleSensors)
	mux.HandleFunc("/api/rules", s.handleRules)
	mux.HandleFunc("/api/buttons", s.handleButtons)
	mux.HandleFunc("/api/buttons/press", s.handleButtonPress)
	mux.HandleFunc("/api/snooze", s.handleSnooze)
	mux.HandleFunc("/api/pair", s.handlePair)
	mux.HandleFunc("/api/rfbots/listen", s.handleRFBotListen)
	mux.HandleFunc("/api/rfbots/poll_status", s.handleRFPollStatus)
	mux.HandleFunc("/api/outlet", s.handleDirectOutlet)
	// Fleet Multi-Bot REST API Routes
	mux.HandleFunc("/api/fleet/broadcast", s.handleFleetBroadcast)
	mux.HandleFunc("/api/fleet/preset", s.handleFleetPreset)
	mux.HandleFunc("/api/fleet/scan_subnet", s.handleFleetScanSubnet)
	mux.HandleFunc("/api/fleet/choreography", s.handleFleetChoreography)
	mux.HandleFunc("/api/devices/control", s.handleDeviceControl)
	mux.HandleFunc("/api/devices/add", s.handleAddDevice)
	mux.HandleFunc("/api/devices/homekit", s.handleToggleHomeKit)
	mux.HandleFunc("/api/v1/rf_event", listener.HTTPHandler(s.engine))

	// Direct Quick Action & Bot Proxy Endpoints
	mux.HandleFunc("/api/db/rf/counts", s.handleGetRFCounts)
	mux.HandleFunc("/api/db/rf/history", s.handleGetRFHistory)
	mux.HandleFunc("/api/db/telemetry/pings", s.handleGetPingTelemetry)
	mux.HandleFunc("/api/db/telemetry/system", s.handleGetSystemTelemetry)
	mux.HandleFunc("/api/speedtest/history", s.handleSpeedtestHistory)
	mux.HandleFunc("/api/speedtest/run", s.handleRunSpeedtest)
	mux.HandleFunc("/api/states", s.handleGetStates)
	mux.HandleFunc("/api/states/set", s.handleSetState)

	// Direct Quick Action & Bot Proxy Endpoints
	mux.HandleFunc("/s1on", s.handleSimpleBotProxy("/s1on"))
	mux.HandleFunc("/s1off", s.handleSimpleBotProxy("/s1off"))
	mux.HandleFunc("/s2on", s.handleSimpleBotProxy("/s2on"))
	mux.HandleFunc("/s2off", s.handleSimpleBotProxy("/s2off"))
	mux.HandleFunc("/choola", s.handleSpeakerBotProxy("/choola"))

	// WebSocket Endpoint
	mux.HandleFunc("/ws/traffic", s.handleWebSocket)

	// Embedded Static File Server (no-cache headers prevent stale browser disk caching on Pi)
	if s.assets != nil {
		fileServer := http.FileServer(http.FS(s.assets))
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
			w.Header().Set("Pragma", "no-cache")
			w.Header().Set("Expires", "0")
			fileServer.ServeHTTP(w, r)
		})
	} else {
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "text/html")
			_, _ = w.Write([]byte(`<h1>FleetHub Daemon Running</h1><p>API: /api/status</p>`))
		})
	}

	return mux
}

// BroadcastStateChange broadcasts instant entity state changes across all connected WebUI tabs/instances.
func (s *Server) BroadcastStateChange(entityID, state, source, attributes string) {
	msg := map[string]interface{}{
		"type":       "state_change",
		"entity_id":  entityID,
		"state":      state,
		"source":     source,
		"attributes": attributes,
		"timestamp":  time.Now().Format(time.RFC3339Nano),
	}

	select {
	case s.broadcast <- msg:
	default:
	}
}

// BroadcastCountUpdate sends updated aggregate RF signal counts to all clients.
func (s *Server) BroadcastCountUpdate(code uint32, totalCount int64) {
	msg := map[string]interface{}{
		"type":        "rf_count_update",
		"code":        code,
		"code_hex":    fmt.Sprintf("0x%X", code),
		"total_count": totalCount,
		"timestamp":   time.Now().Format(time.RFC3339Nano),
	}

	select {
	case s.broadcast <- msg:
	default:
	}
}

// BroadcastSpeedtestDone notifies clients that a speedtest benchmark completed.
func (s *Server) BroadcastSpeedtestDone(record db.SpeedtestRecord) {
	msg := map[string]interface{}{
		"type":          "speedtest_done",
		"download_mbps": record.DownloadMbps,
		"upload_mbps":   record.UploadMbps,
		"ping_ms":       record.PingMs,
		"server":        record.Server,
		"timestamp":     record.CreatedAt.Format(time.RFC3339Nano),
	}

	select {
	case s.broadcast <- msg:
	default:
	}
}

func (s *Server) BroadcastEvent(event rules.EventPayload, executed []string) {
	s.pairMu.Lock()
	if s.pairActive && (event.Bits >= 20 || event.Bits == 0) {
		s.pairCode = event.Code
	}
	s.pairMu.Unlock()

	msg := map[string]interface{}{
		"type":      "rf_event",
		"code":      event.Code,
		"code_hex":  fmt.Sprintf("0x%X", event.Code),
		"bits":      event.Bits,
		"protocol":  event.Protocol,
		"pulse":     event.Pulse,
		"gateway":   event.Gateway,
		"timestamp": event.Timestamp.Format(time.RFC3339Nano),
		"executed":  executed,
	}

	select {
	case s.broadcast <- msg:
	default:
	}
}

func (s *Server) broadcastLoop() {
	for msg := range s.broadcast {
		data, err := json.Marshal(msg)
		if err != nil {
			continue
		}

		s.clientsMu.Lock()
		for client := range s.clients {
			_ = client.WriteMessage(websocket.TextMessage, data)
		}
		s.clientsMu.Unlock()
	}
}

func (s *Server) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}

	s.clientsMu.Lock()
	s.clients[conn] = true
	s.clientsMu.Unlock()

	defer func() {
		s.clientsMu.Lock()
		delete(s.clients, conn)
		s.clientsMu.Unlock()
		_ = conn.Close()
	}()

	for {
		_, _, err := conn.ReadMessage()
		if err != nil {
			break
		}
	}
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"app":            "FleetHub Mothership Daemon",
		"version":        "1.0.0",
		"mode":           s.cfg.Mode,
		"http_port":      s.cfg.HTTPPort,
		"udp_port":       s.cfg.UDPPort,
		"homekit_pin":    s.cfg.HomeKitPIN,
		"bots_online":    len(s.cfg.Bots),
		"sensor_count":   len(s.cfg.Sensors),
		"rule_count":     len(s.cfg.Rules),
		"button_count":   len(s.cfg.Buttons),
		"active_snoozes": len(s.engine.GetActiveSnoozes()),
		"uptime_sec":     time.Since(startTime).Seconds(),
	})
}

var startTime = time.Now()

// isValidIP returns true if the given string is a parseable IP address.
func isValidIP(ip string) bool {
	return ip != "" && net.ParseIP(strings.TrimSpace(ip)) != nil
}

func (s *Server) handleBots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.URL.Query().Get("refresh") == "1" || r.URL.Query().Get("refresh") == "true" {
		if s.pinger != nil {
			s.pinger.PingAll()
		}
		if s.scanner != nil {
			go s.scanner.ScanOnce()
		}
	}

	_ = json.NewEncoder(w).Encode(s.cfg.Bots)
}

func (s *Server) handleScanBots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if s.pinger != nil {
		s.pinger.PingAll()
	}
	if s.scanner != nil {
		s.scanner.ScanOnce()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "ok",
		"bots":   s.cfg.Bots,
	})
}

func (s *Server) handleUpdateBot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost && r.Method != http.MethodPut {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var node config.RobotNode
	if err := json.NewDecoder(r.Body).Decode(&node); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	if node.ID == "" && node.Name != "" {
		node.ID = strings.ToLower(strings.ReplaceAll(node.Name, " ", "_"))
	}
	if node.ID == "" {
		http.Error(w, "Missing bot ID", http.StatusBadRequest)
		return
	}
	if node.Hostname != "" && !strings.Contains(node.Hostname, ".") && !isValidIP(node.Hostname) {
		node.Hostname = node.Hostname + ".local"
	}
	if node.Port == 0 {
		node.Port = 80
	}
	if len(node.Actions) == 0 {
		if node.Platform == config.PlatformDogBot {
			node.Actions = config.DogBotDefaultActions()
		} else if node.Platform == config.PlatformMyBot {
			node.Actions = config.MyBotDefaultActions()
		}
	}
	if node.Status == "" {
		node.Status = "online"
	}

	s.cfg.UpsertBot(node)
	_ = s.cfg.Save()

	if s.pinger != nil {
		go func() {
			latency, online, ip := s.pinger.PingDevice(node)
			status := "offline"
			if online {
				status = "online"
			}
			node.Status = status
			node.LatencyMs = latency
			if ip != "" {
				node.IP = ip
			}
			s.cfg.UpsertBot(node)
		}()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "ok",
		"bot":    node,
	})
}

func (s *Server) handleDeleteBot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost && r.Method != http.MethodDelete {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	id := r.URL.Query().Get("id")
	if id == "" {
		var req struct {
			ID string `json:"id"`
		}
		_ = json.NewDecoder(r.Body).Decode(&req)
		id = req.ID
	}

	if id == "" {
		http.Error(w, "Missing bot ID", http.StatusBadRequest)
		return
	}

	deleted := s.cfg.DeleteBot(id)
	_ = s.cfg.Save()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":  "ok",
		"deleted": deleted,
		"id":      id,
		"count":   len(s.cfg.Bots),
	})
}

func (s *Server) handleSaveAllBots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost && r.Method != http.MethodPut {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var bots []config.RobotNode
	if err := json.NewDecoder(r.Body).Decode(&bots); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	for i := range bots {
		if bots[i].ID == "" && bots[i].Name != "" {
			bots[i].ID = strings.ToLower(strings.ReplaceAll(bots[i].Name, " ", "_"))
		}
		if bots[i].Port == 0 {
			bots[i].Port = 80
		}
		if len(bots[i].Actions) == 0 {
			if bots[i].Platform == config.PlatformDogBot {
				bots[i].Actions = config.DogBotDefaultActions()
			} else if bots[i].Platform == config.PlatformMyBot {
				bots[i].Actions = config.MyBotDefaultActions()
			}
		}
	}

	s.cfg.SetBots(bots)
	_ = s.cfg.Save()

	if s.pinger != nil {
		go s.pinger.PingAll()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "ok",
		"count":  len(s.cfg.Bots),
		"bots":   s.cfg.Bots,
	})
}

func (s *Server) handleRangeBots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Platform   config.BotPlatform `json:"platform"`
		Prefix     string             `json:"prefix"`
		NamePrefix string             `json:"name_prefix"`
		Start      int                `json:"start"`
		End        int                `json:"end"`
		IPBase     string             `json:"ip_base"`
		IPOffset   int                `json:"ip_offset"`
		Port       int                `json:"port"`
		Role       string             `json:"role"`
		Mode       string             `json:"mode"` // "append", "replace_type", "replace_all"
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	if req.Prefix == "" {
		req.Prefix = "bot"
	}
	if req.NamePrefix == "" {
		req.NamePrefix = strings.Title(req.Prefix)
	}
	if req.Start <= 0 {
		req.Start = 1
	}
	if req.End < req.Start {
		req.End = req.Start
	}
	if req.Port == 0 {
		req.Port = 80
	}
	if req.Platform == "" {
		if strings.Contains(strings.ToLower(req.Prefix), "paul") || strings.Contains(strings.ToLower(req.Prefix), "dog") {
			req.Platform = config.PlatformDogBot
		} else if strings.Contains(strings.ToLower(req.Prefix), "mybot") || strings.Contains(strings.ToLower(req.Prefix), "bread") {
			req.Platform = config.PlatformMyBot
		} else {
			req.Platform = config.PlatformDogBot
		}
	}
	if req.Role == "" {
		if req.Platform == config.PlatformDogBot {
			req.Role = "quadruped_dogbot"
		} else if req.Platform == config.PlatformMyBot {
			req.Role = "breadboard_bot"
		} else {
			req.Role = string(req.Platform)
		}
	}

	var generated []config.RobotNode
	for i := req.Start; i <= req.End; i++ {
		id := fmt.Sprintf("%s%d", req.Prefix, i)
		hostname := fmt.Sprintf("%s%d.local", req.Prefix, i)
		fallbackIP := ""
		if req.IPBase != "" {
			fallbackIP = fmt.Sprintf("%s%d", req.IPBase, req.IPOffset+i)
		}
		name := fmt.Sprintf("%s %d", req.NamePrefix, i)

		var actions []config.DeviceAction
		if req.Platform == config.PlatformDogBot {
			actions = config.DogBotDefaultActions()
		} else if req.Platform == config.PlatformMyBot {
			actions = config.MyBotDefaultActions()
		}

		generated = append(generated, config.RobotNode{
			ID:          id,
			Name:        name,
			Hostname:    hostname,
			FallbackIP:  fallbackIP,
			Platform:    req.Platform,
			IP:          hostname,
			Port:        req.Port,
			Status:      "online",
			Role:        req.Role,
			PingEnabled: true,
			Actions:     actions,
		})
	}

	switch req.Mode {
	case "replace_all":
		s.cfg.SetBots(generated)
	case "replace_type":
		var preserved []config.RobotNode
		for _, b := range s.cfg.Bots {
			if b.Platform != req.Platform {
				preserved = append(preserved, b)
			}
		}
		s.cfg.SetBots(append(preserved, generated...))
	default: // "append" or upsert
		for _, b := range generated {
			s.cfg.UpsertBot(b)
		}
	}

	_ = s.cfg.Save()

	if s.pinger != nil {
		go s.pinger.PingAll()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":    "ok",
		"generated": len(generated),
		"count":     len(s.cfg.Bots),
		"bots":      s.cfg.Bots,
	})
}

func (s *Server) handlePingSingleBot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	id := r.URL.Query().Get("id")
	if id == "" && r.Method == http.MethodPost {
		var req struct {
			ID string `json:"id"`
		}
		_ = json.NewDecoder(r.Body).Decode(&req)
		id = req.ID
	}

	if id == "" {
		http.Error(w, "Missing bot ID", http.StatusBadRequest)
		return
	}

	var targetBot *config.RobotNode
	for _, b := range s.cfg.Bots {
		if b.ID == id || b.Hostname == id || strings.EqualFold(b.ID, id) {
			targetBot = &b
			break
		}
	}

	if targetBot == nil {
		http.Error(w, "Bot not found", http.StatusNotFound)
		return
	}

	if s.pinger != nil {
		latency, online, ip := s.pinger.PingDevice(*targetBot)
		status := "offline"
		if online {
			status = "online"
		}
		targetBot.Status = status
		targetBot.LatencyMs = latency
		if ip != "" {
			targetBot.IP = ip
		}
		s.cfg.UpsertBot(*targetBot)
		_ = s.cfg.Save()

		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":     "ok",
			"id":         targetBot.ID,
			"bot_status": status,
			"latency_ms": latency,
			"ip":         targetBot.IP,
		})
		return
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     "ok",
		"id":         targetBot.ID,
		"bot_status": targetBot.Status,
		"latency_ms": targetBot.LatencyMs,
		"ip":         targetBot.IP,
	})
}

func (s *Server) handleSensors(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		var sensor config.RFSensor
		if err := json.NewDecoder(r.Body).Decode(&sensor); err != nil {
			http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
			return
		}
		if sensor.Code == 0 {
			http.Error(w, "Missing sensor RF code", http.StatusBadRequest)
			return
		}
		s.cfg.UpsertSensor(sensor)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "saved", "sensor": sensor})
		return
	}

	if r.Method == http.MethodDelete {
		var code uint32
		_, err := fmt.Sscanf(r.URL.Query().Get("code"), "%d", &code)
		if err != nil || code == 0 {
			var payload struct {
				Code uint32 `json:"code"`
			}
			_ = json.NewDecoder(r.Body).Decode(&payload)
			code = payload.Code
		}
		if code == 0 {
			http.Error(w, "Missing or invalid sensor code", http.StatusBadRequest)
			return
		}
		deleted := s.cfg.DeleteSensor(code)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "deleted", "code": code, "success": deleted})
		return
	}

	_ = json.NewEncoder(w).Encode(s.cfg.Sensors)
}

func (s *Server) handleRules(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		var rule config.AutomationRule
		if err := json.NewDecoder(r.Body).Decode(&rule); err != nil {
			http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
			return
		}
		s.cfg.UpsertRule(rule)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "saved", "rule": rule})
		return
	}

	if r.Method == http.MethodDelete {
		id := r.URL.Query().Get("id")
		if id == "" {
			var payload struct {
				ID string `json:"id"`
			}
			_ = json.NewDecoder(r.Body).Decode(&payload)
			id = payload.ID
		}
		if id == "" {
			http.Error(w, "Missing rule ID", http.StatusBadRequest)
			return
		}
		deleted := s.cfg.DeleteRule(id)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "deleted", "id": id, "success": deleted})
		return
	}

	_ = json.NewEncoder(w).Encode(s.cfg.Rules)
}

func (s *Server) handleButtons(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		var btn config.InputButton
		if err := json.NewDecoder(r.Body).Decode(&btn); err != nil {
			http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
			return
		}
		s.cfg.UpsertButton(btn)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "saved", "button": btn})
		return
	}

	if r.Method == http.MethodDelete {
		id := r.URL.Query().Get("id")
		if id == "" {
			var payload struct {
				ID string `json:"id"`
			}
			_ = json.NewDecoder(r.Body).Decode(&payload)
			id = payload.ID
		}
		if id == "" {
			http.Error(w, "Missing button ID", http.StatusBadRequest)
			return
		}
		deleted := s.cfg.DeleteButton(id)
		_ = s.cfg.Save()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "deleted", "id": id, "success": deleted})
		return
	}

	_ = json.NewEncoder(w).Encode(s.cfg.Buttons)
}

func (s *Server) handleButtonPress(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		ID   string `json:"id"`
		Code uint32 `json:"code"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	var matchedBtn *config.InputButton
	for _, b := range s.cfg.Buttons {
		if (req.ID != "" && b.ID == req.ID) || (req.Code != 0 && b.TriggerCode == req.Code) {
			matchedBtn = &b
			break
		}
	}

	if matchedBtn == nil {
		// If custom button code press passed on the fly, construct temporary button
		matchedBtn = &config.InputButton{
			ID:                 req.ID,
			Name:               "Virtual WebUI Button",
			TriggerCode:        req.Code,
			ActionType:         "snooze_all",
			MinutesPerClick:    15,
			MultiClickWindowMs: 2500,
		}
	}

	s.engine.TriggerInputButton(*matchedBtn)

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "pressed",
		"button": matchedBtn.Name,
	})
}

func (s *Server) handleSnooze(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		var req struct {
			RuleID      string `json:"rule_id"`
			DurationMin int    `json:"duration_min"`
			Action      string `json:"action"` // "set", "cancel", "extend"
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
			return
		}

		if req.Action == "cancel" {
			s.engine.CancelSnooze(req.RuleID)
			_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "canceled", "rule_id": req.RuleID})
			return
		}

		duration := req.DurationMin
		if duration <= 0 {
			duration = 15
		}
		until := s.engine.SnoozeRule(req.RuleID, duration)
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":        "snoozed",
			"rule_id":       req.RuleID,
			"duration_min": duration,
			"snoozed_until": until.Format(time.RFC3339),
		})
		return
	}

	activeSnoozes := s.engine.GetActiveSnoozes()
	_ = json.NewEncoder(w).Encode(activeSnoozes)
}

func (s *Server) handlePair(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		s.pairMu.Lock()
		s.pairActive = true
		s.pairCode = 0
		s.pairMu.Unlock()
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "listening_for_rf"})
		return
	}

	s.pairMu.Lock()
	code := s.pairCode
	active := s.pairActive
	s.pairMu.Unlock()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"active":        active,
		"captured_code": code,
		"code_hex":      fmt.Sprintf("0x%X", code),
	})
}

func (s *Server) handleToggleHomeKit(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		ID      string `json:"id"`
		Enabled bool   `json:"enabled"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	s.cfg.UpsertBot(config.RobotNode{
		ID:             req.ID,
		HomeKitEnabled: req.Enabled,
	})
	_ = s.cfg.Save()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":          "ok",
		"id":              req.ID,
		"homekit_enabled": req.Enabled,
	})
}

func (s *Server) handleDeviceControl(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		ID          string `json:"id"`
		Endpoint    string `json:"endpoint"`
		Method      string `json:"method"`
		Payload     string `json:"payload"`
		ActionIndex int    `json:"action_index"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	var bot *config.RobotNode
	for _, b := range s.cfg.Bots {
		if b.ID == req.ID || b.Name == req.ID || b.Hostname == req.ID || strings.EqualFold(b.ID, req.ID) || strings.EqualFold(b.Hostname, req.ID) || (strings.HasPrefix(strings.ToLower(b.ID), strings.ToLower(req.ID)) && req.ID != "") {
			bot = &b
			break
		}
	}

	if bot == nil {
		targetHost := req.ID
		if !strings.Contains(targetHost, ".") && !isValidIP(targetHost) {
			targetHost = targetHost + ".local"
		}
		fallbackNode := config.RobotNode{
			ID:       req.ID,
			Name:     req.ID,
			Hostname: targetHost,
			Port:     80,
		}
		bot = &fallbackNode
	}

	endpoint := req.Endpoint
	method := strings.ToUpper(req.Method)
	payloadStr := req.Payload

	if endpoint == "" && req.ActionIndex < len(bot.Actions) {
		act := bot.Actions[req.ActionIndex]
		endpoint = act.Endpoint
		method = strings.ToUpper(act.Method)
		payloadStr = act.Payload
	}

	if method == "" {
		method = "GET"
	}

	port := bot.Port
	if port == 0 {
		port = 80
	}

	// Build target list. On Linux (Raspberry Pi), Go's pure-Go DNS resolver
	// cannot resolve .local mDNS hostnames. So we prioritise the known IP and
	// fallback_ip FIRST, then attempt the mDNS hostname as a last resort.
	targets := []string{}

	// 1. Known resolved IP (fastest, always works on Pi)
	if isValidIP(bot.IP) {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.IP, port))
	}
	// 2. Fallback static IP
	if bot.FallbackIP != "" && bot.FallbackIP != bot.IP {
		if isValidIP(bot.FallbackIP) {
			targets = append(targets, fmt.Sprintf("%s:%d", bot.FallbackIP, port))
		}
	}
	// 3. mDNS hostname — works on Windows/macOS, may fail on Pi without avahi
	if bot.Hostname != "" {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.Hostname, port))
	} else if bot.IP == "" && bot.FallbackIP == "" {
		// Last resort: use raw bot name as host
		targets = append(targets, fmt.Sprintf("%s:%d", bot.ID+".local", port))
	}

	client := &http.Client{Timeout: 5 * time.Second}
	var lastErr error
	var successTarget string

	for _, targetHost := range targets {
		url := targetHost
		if !strings.HasPrefix(url, "http://") && !strings.HasPrefix(url, "https://") {
			url = "http://" + url
		}
		fullURL := strings.TrimRight(url, "/") + "/" + strings.TrimLeft(endpoint, "/")

		httpReq, err := http.NewRequest(method, fullURL, bytes.NewBufferString(payloadStr))
		if err != nil {
			lastErr = err
			continue
		}
		if method == "POST" && payloadStr != "" {
			httpReq.Header.Set("Content-Type", "application/json")
		}

		resp, err := client.Do(httpReq)
		if err == nil {
			_ = resp.Body.Close()
			successTarget = targetHost
			break
		}
		lastErr = err
	}

	if successTarget != "" {
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":   "success",
			"target":   successTarget,
			"endpoint": endpoint,
		})
	} else {
		// Return 200 with fallback simulated message if offline in local dev mode
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":   "dispatched",
			"endpoint": endpoint,
			"notice":   fmt.Sprintf("Dispatched to targets %v (offline simulation mode: %v)", targets, lastErr),
		})
	}
}

func (s *Server) handleAddDevice(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var node config.RobotNode
	if err := json.NewDecoder(r.Body).Decode(&node); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	if node.ID == "" {
		node.ID = strings.ToLower(strings.ReplaceAll(node.Name, " ", "_"))
	}

	s.cfg.UpsertBot(node)
	_ = s.cfg.Save()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "created",
		"device": node,
	})
}

func (s *Server) handleRFBotListen(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	if r.Method == http.MethodPost {
		var req struct {
			BotID  string `json:"bot_id"`
			Action string `json:"action"` // "start" or "stop"
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
			return
		}

		botID := req.BotID
		if botID == "" {
			// Find primary RX bot
			for _, b := range s.cfg.Bots {
				if b.Platform == config.PlatformRFBot && (strings.ToLower(b.Role) == "receiver" || strings.ToLower(b.Role) == "transceiver" || b.ID == "rfbot1") {
					botID = b.ID
					break
				}
			}
		}

		if botID == "" {
			botID = "rfbot1"
		}

		enable := (req.Action == "start" || req.Action == "listen" || req.Action == "")
		if s.poller != nil {
			s.poller.SetBotListening(botID, enable)
		}

		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":    "ok",
			"bot_id":    botID,
			"listening": enable,
		})
		return
	}

	activeListeners := []string{}
	if s.poller != nil {
		activeListeners = s.poller.GetActiveListeners()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"active_listeners": activeListeners,
	})
}

func (s *Server) handleRFPollStatus(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	activeListeners := []string{}
	if s.poller != nil {
		activeListeners = s.poller.GetActiveListeners()
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"active_listeners": activeListeners,
		"poller_running":  s.poller != nil,
	})
}

func (s *Server) findTXGatewayBot() config.RobotNode {
	for _, b := range s.cfg.Bots {
		roleLower := strings.ToLower(b.Role)
		if b.Platform == config.PlatformRFBot && (roleLower == "transceiver" || roleLower == "transmitter" || b.ID == "rfbot6") {
			return b
		}
	}
	for _, b := range s.cfg.Bots {
		if b.Platform == config.PlatformRFBot {
			return b
		}
	}
	return config.RobotNode{
		ID:       "rfbot6",
		Hostname: "rfbot6.local",
		Platform: config.PlatformRFBot,
		Port:     80,
	}
}

func (s *Server) dispatchToBot(bot config.RobotNode, endpoint string) error {
	port := bot.Port
	if port == 0 || port == 4330 {
		port = 80
	}

	// Build ordered list of targets: IP-direct first (reliable on Pi),
	// then mDNS hostname as fallback (works on Windows/macOS).
	type candidate struct{ host string }
	var candidates []candidate

	if isValidIP(bot.IP) {
		candidates = append(candidates, candidate{fmt.Sprintf("%s:%d", bot.IP, port)})
	}
	if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP {
		candidates = append(candidates, candidate{fmt.Sprintf("%s:%d", bot.FallbackIP, port)})
	}
	if bot.Hostname != "" {
		candidates = append(candidates, candidate{fmt.Sprintf("%s:%d", bot.Hostname, port)})
	}
	if len(candidates) == 0 {
		candidates = append(candidates, candidate{fmt.Sprintf("rfbot6.local:%d", port)})
	}

	client := &http.Client{Timeout: 5 * time.Second}
	var lastErr error
	for _, c := range candidates {
		host := c.host
		if !strings.HasPrefix(host, "http") {
			host = "http://" + host
		}
		fullURL := strings.TrimRight(host, "/") + "/" + strings.TrimLeft(endpoint, "/")
		req, err := http.NewRequest("GET", fullURL, nil)
		if err != nil {
			lastErr = err
			continue
		}
		resp, err := client.Do(req)
		if err == nil {
			_ = resp.Body.Close()
			return nil // success
		}
		lastErr = err
	}
	return lastErr
}

func (s *Server) handleDirectOutlet(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	path := strings.Trim(r.URL.Path, "/")
	parts := strings.Split(path, "/")

	name := r.URL.Query().Get("name")
	state := r.URL.Query().Get("state")

	if len(parts) >= 3 {
		name = parts[2]
		if len(parts) >= 4 {
			state = parts[3]
		}
	}

	nameLower := strings.ToLower(strings.TrimSpace(name))
	stateLower := strings.ToLower(strings.TrimSpace(state))

	if stateLower == "" || stateLower == "1" || stateLower == "true" {
		stateLower = "on"
	} else if stateLower == "0" || stateLower == "false" {
		stateLower = "off"
	}

	outlets := map[string]map[string]uint32{
		"alpha":   {"on": 5576451, "off": 5576460},
		"bravo":   {"on": 5584131, "off": 5584140},
		"charlie": {"on": 5576131, "off": 5576140},
		"delta":   {"on": 5577987, "off": 5577996},
		"echo":    {"on": 5575987, "off": 5575996},
		"foxtrot": {"on": 1381827, "off": 1381836},
		"golf":    {"on": 1382147, "off": 1382156},
	}

	codes, exists := outlets[nameLower]
	if !exists {
		http.Error(w, fmt.Sprintf("Unknown outlet name '%s'. Available: alpha, bravo, charlie, delta, echo, foxtrot, golf", name), http.StatusBadRequest)
		return
	}

	code, validState := codes[stateLower]
	if !validState {
		http.Error(w, fmt.Sprintf("Invalid state '%s'. Use 'on' or 'off'.", state), http.StatusBadRequest)
		return
	}

	bot := s.findTXGatewayBot()
	endpoint := fmt.Sprintf("/rf/send?code=%d&bits=24&proto=1&pulse=185", code)
	err := s.dispatchToBot(bot, endpoint)

	dispatchStatus := "success"
	if err != nil {
		dispatchStatus = "dispatched_offline_sim"
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     dispatchStatus,
		"outlet":     strings.Title(nameLower),
		"state":      strings.ToUpper(stateLower),
		"code":       code,
		"code_hex":   fmt.Sprintf("0x%X", code),
		"target_bot": bot.Hostname,
		"error":      fmt.Sprintf("%v", err),
	})
}

func (s *Server) handleMothershipRFSend(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	codeStr := r.URL.Query().Get("code")
	if codeStr == "" {
		http.Error(w, "Missing ?code= parameter", http.StatusBadRequest)
		return
	}

	var code uint32
	clean := strings.TrimSpace(codeStr)
	if strings.HasPrefix(clean, "0x") || strings.HasPrefix(clean, "0X") {
		if val, err := strconv.ParseUint(clean[2:], 16, 32); err == nil {
			code = uint32(val)
		}
	}
	if code == 0 {
		if val, err := strconv.ParseUint(clean, 10, 32); err == nil {
			code = uint32(val)
		} else if val, err := strconv.ParseUint(clean, 16, 32); err == nil {
			code = uint32(val)
		}
	}

	if code == 0 {
		http.Error(w, "Invalid code parameter", http.StatusBadRequest)
		return
	}

	bits := 24
	if b := r.URL.Query().Get("bits"); b != "" {
		_, _ = fmt.Sscanf(b, "%d", &bits)
	}

	proto := 1
	if p := r.URL.Query().Get("proto"); p != "" {
		_, _ = fmt.Sscanf(p, "%d", &proto)
	}

	pulse := 185
	if pu := r.URL.Query().Get("pulse"); pu != "" {
		_, _ = fmt.Sscanf(pu, "%d", &pulse)
	}

	bot := s.findTXGatewayBot()
	endpoint := fmt.Sprintf("/rf/send?code=%d&bits=%d&proto=%d&pulse=%d", code, bits, proto, pulse)
	err := s.dispatchToBot(bot, endpoint)

	dispatchStatus := "success"
	if err != nil {
		dispatchStatus = "dispatched_offline_sim"
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     dispatchStatus,
		"code":       code,
		"code_hex":   fmt.Sprintf("0x%X", code),
		"bits":       bits,
		"proto":      proto,
		"pulse":      pulse,
		"target_bot": bot.Hostname,
		"error":      fmt.Sprintf("%v", err),
	})
}

func (s *Server) handleDirectAction(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	actionName := r.URL.Query().Get("name")
	botID := r.URL.Query().Get("bot")

	if actionName == "" {
		http.Error(w, "Missing ?name= parameter (e.g. ?name=Alpha+ON)", http.StatusBadRequest)
		return
	}

	var matchedBot *config.RobotNode
	var matchedAct *config.DeviceAction

	for i, b := range s.cfg.Bots {
		if botID != "" && b.ID != botID && b.Hostname != botID {
			continue
		}
		for j, a := range b.Actions {
			if strings.EqualFold(a.Name, actionName) {
				matchedBot = &s.cfg.Bots[i]
				matchedAct = &b.Actions[j]
				break
			}
		}
		if matchedBot != nil {
			break
		}
	}

	if matchedBot == nil || matchedAct == nil {
		http.Error(w, fmt.Sprintf("Action '%s' not found in fleet registry", actionName), http.StatusNotFound)
		return
	}

	err := s.dispatchToBot(*matchedBot, matchedAct.Endpoint)

	dispatchStatus := "success"
	if err != nil {
		dispatchStatus = "dispatched_offline_sim"
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     dispatchStatus,
		"action":     matchedAct.Name,
		"endpoint":   matchedAct.Endpoint,
		"target_bot": matchedBot.Hostname,
		"error":      fmt.Sprintf("%v", err),
	})
}

func (s *Server) findBotByPlatformOrID(platform config.BotPlatform, idPrefix string) config.RobotNode {
	for _, b := range s.cfg.Bots {
		if b.ID == idPrefix || strings.HasPrefix(strings.ToLower(b.ID), strings.ToLower(idPrefix)) || b.Platform == platform {
			return b
		}
	}
	return config.RobotNode{
		ID:       idPrefix,
		Hostname: idPrefix + ".local",
		Platform: platform,
		Port:     80,
	}
}

func (s *Server) handleSimpleBotProxy(endpoint string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		bot := s.findBotByPlatformOrID(config.PlatformSimpleBot, "simplebot1")
		err := s.dispatchToBot(bot, endpoint)
		dispatchStatus := "success"
		if err != nil {
			dispatchStatus = "dispatched_offline_sim"
		}

		// Persist state and broadcast over WebSocket
		var entityID, newState string
		switch endpoint {
		case "/s1on":
			entityID = "switch_kitchen"
			newState = "on"
		case "/s1off":
			entityID = "switch_kitchen"
			newState = "off"
		case "/s2on":
			entityID = "switch_hallway"
			newState = "on"
		case "/s2off":
			entityID = "switch_hallway"
			newState = "off"
		}
		if entityID != "" {
			if s.database != nil {
				_ = s.database.SetEntityState(entityID, newState, "api", "")
			}
			s.BroadcastStateChange(entityID, newState, "api", "")
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":     dispatchStatus,
			"target_bot": bot.Hostname,
			"endpoint":   endpoint,
			"error":      fmt.Sprintf("%v", err),
		})
	}
}

func (s *Server) handleSpeakerBotProxy(endpoint string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		bot := s.findBotByPlatformOrID(config.PlatformSpeakerBot, "speakerbot1")
		err := s.dispatchToBot(bot, endpoint)
		dispatchStatus := "success"
		if err != nil {
			dispatchStatus = "dispatched_offline_sim"
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":     dispatchStatus,
			"target_bot": bot.Hostname,
			"endpoint":   endpoint,
			"error":      fmt.Sprintf("%v", err),
		})
	}
}

func (s *Server) handleGetRFCounts(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode([]db.RFAggregate{})
		return
	}
	counts, err := s.database.GetRFCounts()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if counts == nil {
		counts = []db.RFAggregate{}
	}
	_ = json.NewEncoder(w).Encode(counts)
}

func (s *Server) handleGetRFHistory(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode([]db.RFEventRecord{})
		return
	}
	var code uint32
	if cStr := r.URL.Query().Get("code"); cStr != "" {
		if c, err := strconv.ParseUint(cStr, 10, 32); err == nil {
			code = uint32(c)
		}
	}
	limit := 100
	if lStr := r.URL.Query().Get("limit"); lStr != "" {
		if l, err := strconv.Atoi(lStr); err == nil {
			limit = l
		}
	}
	history, err := s.database.GetRFHistory(code, limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if history == nil {
		history = []db.RFEventRecord{}
	}
	_ = json.NewEncoder(w).Encode(history)
}

func (s *Server) handleGetPingTelemetry(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode([]db.PingRecord{})
		return
	}
	hours := 24
	if hStr := r.URL.Query().Get("hours"); hStr != "" {
		if h, err := strconv.Atoi(hStr); err == nil {
			hours = h
		}
	}
	records, err := s.database.GetPingHistory(hours)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if records == nil {
		records = []db.PingRecord{}
	}
	_ = json.NewEncoder(w).Encode(records)
}

func (s *Server) handleGetSystemTelemetry(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode([]db.SystemMetricRecord{})
		return
	}
	hours := 24
	if hStr := r.URL.Query().Get("hours"); hStr != "" {
		if h, err := strconv.Atoi(hStr); err == nil {
			hours = h
		}
	}
	records, err := s.database.GetSystemMetrics(hours)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if records == nil {
		records = []db.SystemMetricRecord{}
	}
	_ = json.NewEncoder(w).Encode(records)
}

func (s *Server) handleSpeedtestHistory(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode([]db.SpeedtestRecord{})
		return
	}
	limit := 30
	if lStr := r.URL.Query().Get("limit"); lStr != "" {
		if l, err := strconv.Atoi(lStr); err == nil {
			limit = l
		}
	}
	records, err := s.database.GetSpeedtestHistory(limit)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if records == nil {
		records = []db.SpeedtestRecord{}
	}
	_ = json.NewEncoder(w).Encode(records)
}

func (s *Server) handleRunSpeedtest(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.telemetry == nil {
		http.Error(w, "Telemetry service unavailable", http.StatusServiceUnavailable)
		return
	}
	go func() {
		_, _ = s.telemetry.RunSpeedtest()
	}()
	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":  "speedtest_started",
		"message": "Speedtest benchmark running in background",
	})
}

func (s *Server) handleGetStates(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if s.database == nil {
		_ = json.NewEncoder(w).Encode(map[string]db.EntityState{})
		return
	}
	states, err := s.database.GetEntityStates()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	_ = json.NewEncoder(w).Encode(states)
}

func (s *Server) handleSetState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	var req struct {
		EntityID   string `json:"entity_id"`
		State      string `json:"state"`
		Source     string `json:"source"`
		Attributes string `json:"attributes"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid payload", http.StatusBadRequest)
		return
	}
	if req.Source == "" {
		req.Source = "webui"
	}
	if s.database != nil {
		_ = s.database.SetEntityState(req.EntityID, req.State, req.Source, req.Attributes)
	}
	s.BroadcastStateChange(req.EntityID, req.State, req.Source, req.Attributes)
	_ = json.NewEncoder(w).Encode(map[string]interface{}{"status": "ok", "entity_id": req.EntityID, "state": req.State})
}

// ═══════════════════════════════════════════════════════════════════════════════
// FLEET MULTI-BOT DISPATCH & CHOREOGRAPHY HANDLERS
// ═══════════════════════════════════════════════════════════════════════════════

type BroadcastResult struct {
	BotID    string `json:"bot_id"`
	Name     string `json:"name"`
	Target   string `json:"target"`
	Success  bool   `json:"success"`
	Latency  int64  `json:"latency_ms"`
	Response string `json:"response,omitempty"`
	Error    string `json:"error,omitempty"`
}

func (s *Server) handleFleetBroadcast(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Target   string   `json:"target"`  // "all", "paulbot", "mybot", "selected"
		BotIDs   []string `json:"bot_ids"` // used when target == "selected"
		Endpoint string   `json:"endpoint"`
		Method   string   `json:"method"`
		Payload  string   `json:"payload"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	if req.Endpoint == "" {
		http.Error(w, "Missing endpoint", http.StatusBadRequest)
		return
	}
	if req.Method == "" {
		req.Method = "GET"
	}
	req.Method = strings.ToUpper(req.Method)

	// Filter targeted bots
	var targets []config.RobotNode
	targetMap := make(map[string]bool)
	for _, id := range req.BotIDs {
		targetMap[strings.ToLower(id)] = true
	}

	for _, b := range s.cfg.Bots {
		idLower := strings.ToLower(b.ID)
		hostLower := strings.ToLower(b.Hostname)
		platLower := strings.ToLower(string(b.Platform))

		switch strings.ToLower(req.Target) {
		case "paulbot", "dogbot":
			if strings.Contains(idLower, "paul") || strings.Contains(idLower, "dog") ||
				strings.Contains(hostLower, "paul") || strings.Contains(hostLower, "dog") ||
				platLower == "dogbot_v1" || platLower == "dogbot" {
				targets = append(targets, b)
			}
		case "mybot":
			if strings.Contains(idLower, "mybot") || strings.Contains(hostLower, "mybot") || platLower == "mybot" {
				targets = append(targets, b)
			}
		case "selected":
			if targetMap[idLower] || targetMap[hostLower] || targetMap[b.Name] {
				targets = append(targets, b)
			}
		case "all", "":
			targets = append(targets, b)
		}
	}

	if len(targets) == 0 {
		_ = json.NewEncoder(w).Encode(map[string]interface{}{
			"status":   "warning",
			"message":  "No matching bots found in fleet for target filter",
			"endpoint": req.Endpoint,
			"results":  []BroadcastResult{},
		})
		return
	}

	// Dispatch HTTP concurrently across all target bots with 3s timeout
	results := make([]BroadcastResult, len(targets))
	var wg sync.WaitGroup
	client := &http.Client{Timeout: 3 * time.Second}

	for i, b := range targets {
		wg.Add(1)
		go func(idx int, bot config.RobotNode) {
			defer wg.Done()
			start := time.Now()

			// Build list of dialable addresses in priority order
			hostCandidates := []string{}
			port := bot.Port
			if port == 0 {
				port = 80
			}

			if isValidIP(bot.IP) {
				hostCandidates = append(hostCandidates, fmt.Sprintf("%s:%d", bot.IP, port))
			}
			if bot.FallbackIP != "" && bot.FallbackIP != bot.IP && isValidIP(bot.FallbackIP) {
				hostCandidates = append(hostCandidates, fmt.Sprintf("%s:%d", bot.FallbackIP, port))
			}
			if bot.Hostname != "" {
				hostCandidates = append(hostCandidates, fmt.Sprintf("%s:%d", bot.Hostname, port))
			} else {
				hostCandidates = append(hostCandidates, fmt.Sprintf("%s.local:%d", bot.ID, port))
			}

			var successHost string
			var lastErr error

			for _, candidate := range hostCandidates {
				url := candidate
				if !strings.HasPrefix(url, "http://") && !strings.HasPrefix(url, "https://") {
					url = "http://" + url
				}
				fullURL := strings.TrimRight(url, "/") + "/" + strings.TrimLeft(req.Endpoint, "/")

				httpReq, err := http.NewRequest(req.Method, fullURL, bytes.NewBufferString(req.Payload))
				if err != nil {
					lastErr = err
					continue
				}
				if req.Method == "POST" && req.Payload != "" {
					httpReq.Header.Set("Content-Type", "application/json")
				}

				resp, err := client.Do(httpReq)
				if err == nil {
					_ = resp.Body.Close()
					successHost = candidate
					break
				}
				lastErr = err
			}

			elapsed := time.Since(start).Milliseconds()
			res := BroadcastResult{
				BotID:   bot.ID,
				Name:    bot.Name,
				Latency: elapsed,
			}

			if successHost != "" {
				res.Success = true
				res.Target = successHost
				res.Response = "OK"
			} else {
				res.Success = false
				res.Target = strings.Join(hostCandidates, ", ")
				if lastErr != nil {
					res.Error = lastErr.Error()
				} else {
					res.Error = "connection timed out"
				}
			}

			results[idx] = res
		}(i, b)
	}

	wg.Wait()

	// Notify WebSocket of broadcast
	s.broadcastEvent(map[string]interface{}{
		"type":        "fleet_broadcast",
		"target":      req.Target,
		"endpoint":    req.Endpoint,
		"count":       len(targets),
		"timestamp":   time.Now().Format(time.RFC3339Nano),
	})

	successCount := 0
	for _, r := range results {
		if r.Success {
			successCount++
		}
	}

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":        "completed",
		"total":         len(targets),
		"success_count": successCount,
		"endpoint":      req.Endpoint,
		"results":       results,
	})
}

func (s *Server) broadcastEvent(msg map[string]interface{}) {
	select {
	case s.broadcast <- msg:
	default:
	}
}

func (s *Server) handleFleetPreset(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Preset string `json:"preset"` // "all_stand", "all_wiggle", "all_bark", "all_mario", "dance_party", "all_neutral", "emergency_stop"
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	type actionItem struct {
		target   string
		endpoint string
	}

	var actions []actionItem

	switch req.Preset {
	case "all_stand":
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/stand"})
	case "all_wiggle":
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/wiggle"})
	case "all_bark":
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/bark"})
	case "all_mario":
		actions = append(actions, actionItem{target: "mybot", endpoint: "/anim_mario"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/demo?type=mario"})
	case "dance_party":
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/anim_fireworks"})
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/wiggle"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/anim_mario"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/demo?type=mario"})
	case "all_neutral", "emergency_stop":
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/stand"})
		actions = append(actions, actionItem{target: "paulbot", endpoint: "/anim_eyes"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/game_off"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/s1off"})
		actions = append(actions, actionItem{target: "mybot", endpoint: "/s2off"})
	default:
		actions = append(actions, actionItem{target: "all", endpoint: "/stand"})
	}

	// Trigger actions
	go func() {
		client := &http.Client{Timeout: 2 * time.Second}
		for _, act := range actions {
			for _, bot := range s.cfg.Bots {
				idLower := strings.ToLower(bot.ID)
				isPaul := strings.Contains(idLower, "paul") || strings.Contains(idLower, "dog")
				isMy := strings.Contains(idLower, "mybot")

				if act.target == "all" || (act.target == "paulbot" && isPaul) || (act.target == "mybot" && isMy) {
					targetHost := bot.Hostname
					if targetHost == "" {
						targetHost = bot.IP
					}
					if targetHost != "" {
						url := fmt.Sprintf("http://%s%s", strings.TrimSuffix(targetHost, "/"), act.endpoint)
						go func(u string) {
							resp, err := client.Get(u)
							if err == nil {
								_ = resp.Body.Close()
							}
						}(url)
					}
				}
			}
		}
	}()

	s.broadcastEvent(map[string]interface{}{
		"type":      "fleet_preset",
		"preset":    req.Preset,
		"timestamp": time.Now().Format(time.RFC3339Nano),
	})

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status": "triggered",
		"preset": req.Preset,
	})
}

func (s *Server) handleFleetScanSubnet(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")

	subnet := r.URL.Query().Get("subnet")
	if subnet == "" {
		subnet = "192.168.4"
	}
	startIP := 1
	endIP := 30

	if sStr := r.URL.Query().Get("start"); sStr != "" {
		if val, err := strconv.Atoi(sStr); err == nil && val >= 1 && val <= 254 {
			startIP = val
		}
	}
	if eStr := r.URL.Query().Get("end"); eStr != "" {
		if val, err := strconv.Atoi(eStr); err == nil && val >= startIP && val <= 254 {
			endIP = val
		}
	}

	type DiscoveredHost struct {
		IP       string  `json:"ip"`
		Port     int     `json:"port"`
		Latency  float64 `json:"latency_ms"`
		Platform string  `json:"platform"`
		Name     string  `json:"name"`
		Hostname string  `json:"hostname"`
	}

	var mu sync.Mutex
	var discovered []DiscoveredHost
	var wg sync.WaitGroup

	for i := startIP; i <= endIP; i++ {
		targetIP := fmt.Sprintf("%s.%d", subnet, i)
		wg.Add(1)

		go func(ip string) {
			defer wg.Done()
			addr := fmt.Sprintf("%s:80", ip)
			t0 := time.Now()
			conn, err := net.DialTimeout("tcp", addr, 400*time.Millisecond)
			if err != nil {
				return
			}
			latency := float64(time.Since(t0).Microseconds()) / 1000.0
			_ = conn.Close()

			platform := "esp32_device"
			name := fmt.Sprintf("Bot (%s)", ip)
			hostname := fmt.Sprintf("esp32-%s.local", strings.ReplaceAll(ip, ".", "-"))

			// Probe /status or /time via HTTP
			client := &http.Client{Timeout: 800 * time.Millisecond}
			resp, err := client.Get(fmt.Sprintf("http://%s/status", ip))
			if err == nil {
				var statusPayload struct {
					Platform string `json:"platform"`
					Name     string `json:"name"`
					Device   string `json:"device"`
					Version  string `json:"version"`
				}
				if json.NewDecoder(resp.Body).Decode(&statusPayload) == nil {
					if statusPayload.Platform != "" {
						platform = statusPayload.Platform
					}
					if statusPayload.Name != "" {
						name = statusPayload.Name
					}
				}
				_ = resp.Body.Close()
			}

			disc := DiscoveredHost{
				IP:       ip,
				Port:     80,
				Latency:  latency,
				Platform: platform,
				Name:     name,
				Hostname: hostname,
			}

			mu.Lock()
			discovered = append(discovered, disc)
			mu.Unlock()

			// Auto-upsert into config
			s.cfg.UpsertBot(config.RobotNode{
				ID:        strings.ToLower(strings.ReplaceAll(name, " ", "_")),
				Name:      name,
				Hostname:  hostname,
				Platform:  config.BotPlatform(platform),
				IP:        ip,
				Port:      80,
				Status:    "online",
				LatencyMs: latency,
			})
		}(targetIP)
	}

	wg.Wait()
	_ = s.cfg.Save()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":     "ok",
		"subnet":     subnet,
		"scanned":    endIP - startIP + 1,
		"discovered": discovered,
	})
}

type ChoreoStep struct {
	DelayMs  int    `json:"delay_ms"`
	Target   string `json:"target"` // "all", "paulbot", "mybot"
	Endpoint string `json:"endpoint"`
	Payload  string `json:"payload,omitempty"`
}

func (s *Server) handleFleetChoreography(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Name  string       `json:"name"`
		Steps []ChoreoStep `json:"steps"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	if len(req.Steps) == 0 {
		http.Error(w, "No steps provided", http.StatusBadRequest)
		return
	}

	// Execute choreography asynchronously
	go func() {
		client := &http.Client{Timeout: 3 * time.Second}
		for idx, step := range req.Steps {
			if step.DelayMs > 0 {
				time.Sleep(time.Duration(step.DelayMs) * time.Millisecond)
			}

			s.broadcastEvent(map[string]interface{}{
				"type":        "choreography_step",
				"name":        req.Name,
				"step_index":  idx,
				"total_steps": len(req.Steps),
				"endpoint":    step.Endpoint,
				"target":      step.Target,
				"timestamp":   time.Now().Format(time.RFC3339Nano),
			})

			for _, bot := range s.cfg.Bots {
				idLower := strings.ToLower(bot.ID)
				isPaul := strings.Contains(idLower, "paul") || strings.Contains(idLower, "dog")
				isMy := strings.Contains(idLower, "mybot")

				if step.Target == "all" || (step.Target == "paulbot" && isPaul) || (step.Target == "mybot" && isMy) {
					targetHost := bot.Hostname
					if targetHost == "" {
						targetHost = bot.IP
					}
					if targetHost != "" {
						url := fmt.Sprintf("http://%s%s", strings.TrimSuffix(targetHost, "/"), step.Endpoint)
						go func(u string) {
							resp, err := client.Get(u)
							if err == nil {
								_ = resp.Body.Close()
							}
						}(url)
					}
				}
			}
		}

		s.broadcastEvent(map[string]interface{}{
			"type":      "choreography_done",
			"name":      req.Name,
			"timestamp": time.Now().Format(time.RFC3339Nano),
		})
	}()

	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":      "running",
		"name":        req.Name,
		"total_steps": len(req.Steps),
	})
}
