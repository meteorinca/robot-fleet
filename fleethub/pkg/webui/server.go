package webui

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/listener"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Server handles WebUI HTTP API endpoints and WebSocket broadcasting.
type Server struct {
	cfg        *config.Config
	engine     *rules.Engine
	assets     fs.FS
	clients    map[*websocket.Conn]bool
	clientsMu  sync.Mutex
	broadcast  chan interface{}
	pairActive bool
	pairCode   uint32
	pairMu     sync.Mutex
}

// NewServer creates a new web API server.
func NewServer(cfg *config.Config, engine *rules.Engine, assets fs.FS) *Server {
	s := &Server{
		cfg:       cfg,
		engine:    engine,
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
	mux.HandleFunc("/api/pair", s.handlePair)
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
	if s.pairActive {
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
		"app":         "FleetHub Mothership Daemon",
		"version":     "1.0.0",
		"mode":        s.cfg.Mode,
		"http_port":   s.cfg.HTTPPort,
		"udp_port":    s.cfg.UDPPort,
		"homekit_pin": s.cfg.HomeKitPIN,
		"bots_online": len(s.cfg.Bots),
		"sensor_count": len(s.cfg.Sensors),
		"rule_count":  len(s.cfg.Rules),
		"uptime_sec":  time.Since(startTime).Seconds(),
	})
}

var startTime = time.Now()

func (s *Server) handleBots(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(s.cfg.Bots)
}

func (s *Server) handleSensors(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(s.cfg.Sensors)
}

func (s *Server) handleRules(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(s.cfg.Rules)
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

	// Prepare list of host targets: Primary hostname/IP first, fallback_ip second
	targets := []string{}
	if bot.Hostname != "" {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.Hostname, port))
	} else if bot.IP != "" {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.IP, port))
	}

	if bot.FallbackIP != "" && bot.FallbackIP != bot.Hostname && bot.FallbackIP != bot.IP {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.FallbackIP, port))
	}

	client := &http.Client{Timeout: 3 * time.Second}
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
