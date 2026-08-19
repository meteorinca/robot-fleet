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
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/listener"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rfpoller"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Server handles WebUI HTTP API endpoints and WebSocket broadcasting.
type Server struct {
	cfg        *config.Config
	engine     *rules.Engine
	poller     *rfpoller.RFPoller
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

// Handler returns the http.Handler for all routes.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// API Routes
	mux.HandleFunc("/api/status", s.handleStatus)
	mux.HandleFunc("/api/bots", s.handleBots)
	mux.HandleFunc("/api/sensors", s.handleSensors)
	mux.HandleFunc("/api/rules", s.handleRules)
	mux.HandleFunc("/api/buttons", s.handleButtons)
	mux.HandleFunc("/api/buttons/press", s.handleButtonPress)
	mux.HandleFunc("/api/snooze", s.handleSnooze)
	mux.HandleFunc("/api/pair", s.handlePair)
	mux.HandleFunc("/api/rfbots/listen", s.handleRFBotListen)
	mux.HandleFunc("/api/rfbots/poll_status", s.handleRFPollStatus)
	mux.HandleFunc("/api/outlet", s.handleDirectOutlet)
	mux.HandleFunc("/api/outlet/", s.handleDirectOutlet)
	mux.HandleFunc("/api/rf/send", s.handleMothershipRFSend)
	mux.HandleFunc("/api/action", s.handleDirectAction)
	mux.HandleFunc("/api/devices/homekit", s.handleToggleHomeKit)
	mux.HandleFunc("/api/devices/control", s.handleDeviceControl)
	mux.HandleFunc("/api/devices/add", s.handleAddDevice)
	mux.HandleFunc("/api/v1/rf_event", listener.HTTPHandler(s.engine))

	// WebSocket Endpoint
	mux.HandleFunc("/ws/traffic", s.handleWebSocket)

	// Embedded Static File Server
	if s.assets != nil {
		fileServer := http.FileServer(http.FS(s.assets))
		mux.Handle("/", fileServer)
	} else {
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "text/html")
			_, _ = w.Write([]byte(`<h1>FleetHub Daemon Running</h1><p>API: /api/status</p>`))
		})
	}

	return mux
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
	_ = json.NewEncoder(w).Encode(s.cfg.Bots)
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
		if b.ID == req.ID || b.Name == req.ID || b.Hostname == req.ID {
			bot = &b
			break
		}
	}

	if bot == nil {
		http.Error(w, "Device not found in registry", http.StatusNotFound)
		return
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


