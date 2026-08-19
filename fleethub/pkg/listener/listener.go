package listener

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
)

// UDPListener listens for incoming RF packets over UDP.
type UDPListener struct {
	cfg    *config.Config
	port   int
	engine *rules.Engine
	conn   *net.UDPConn
	done   chan struct{}
}

// NewUDPListener creates a UDP socket listener.
func NewUDPListener(cfg *config.Config, port int, engine *rules.Engine) *UDPListener {
	return &UDPListener{
		cfg:    cfg,
		port:   port,
		engine: engine,
		done:   make(chan struct{}),
	}
}

// Start begins listening on the specified UDP port.
func (l *UDPListener) Start() error {
	addr := net.UDPAddr{
		Port: l.port,
		IP:   net.ParseIP("0.0.0.0"),
	}

	conn, err := net.ListenUDP("udp", &addr)
	if err != nil {
		return fmt.Errorf("failed to bind UDP port %d: %w", l.port, err)
	}
	l.conn = conn

	go l.listenLoop()
	return nil
}

func (l *UDPListener) listenLoop() {
	buf := make([]byte, 2048)
	for {
		select {
		case <-l.done:
			return
		default:
			_ = l.conn.SetReadDeadline(time.Now().Add(1 * time.Second))
			n, remoteAddr, err := l.conn.ReadFromUDP(buf)
			if err != nil {
				continue
			}

			senderIP := remoteAddr.IP.String()

			var payload rules.EventPayload
			if err := json.Unmarshal(buf[:n], &payload); err == nil {
				if payload.Gateway == "" {
					payload.Gateway = senderIP
				}
				if payload.Timestamp.IsZero() {
					payload.Timestamp = time.Now()
				}

				// Auto-learn/update gateway bot IP and online status
				if l.cfg != nil && senderIP != "" {
					l.cfg.UpsertBot(config.RobotNode{
						ID:       "rfbot1",
						Name:     "RFBot 1 (RX Gateway)",
						Hostname: "rfbot1.local",
						Platform: config.PlatformRFBot,
						IP:       senderIP,
						Status:   "online",
					})
				}

				l.engine.ProcessEvent(payload)
			}
		}
	}
}

// Stop closes the UDP connection.
func (l *UDPListener) Stop() {
	close(l.done)
	if l.conn != nil {
		_ = l.conn.Close()
	}
}

// HTTPHandler returns an http.HandlerFunc for receiving RF events over HTTP POST / GET.
func HTTPHandler(engine *rules.Engine) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var payload rules.EventPayload

		if r.Method == http.MethodPost {
			if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
				http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
				return
			}
		} else if r.Method == http.MethodGet {
			// Allow query parameter trigger (e.g. /api/v1/rf_event?code=123456)
			var code uint32
			_, err := fmt.Sscanf(r.URL.Query().Get("code"), "%d", &code)
			if err != nil || code == 0 {
				http.Error(w, "Missing or invalid code query parameter", http.StatusBadRequest)
				return
			}
			payload.Code = code
			payload.Bits = 24
			payload.Protocol = 1
		}

		if payload.Timestamp.IsZero() {
			payload.Timestamp = time.Now()
		}
		if payload.Gateway == "" {
			payload.Gateway = r.RemoteAddr
		}

		engine.ProcessEvent(payload)

		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	}
}
