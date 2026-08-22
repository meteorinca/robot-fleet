package rfpoller

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/rules"
)

// RFPacket represents a raw RF signal packet returned by rfbot's /rf/poll endpoint.
type RFPacket struct {
	Code    interface{} `json:"code"` // Can be hex string ("1E240"), dec string ("123456"), or integer
	Bits    uint        `json:"bits"`
	Proto   uint        `json:"proto"`
	Pulse   uint        `json:"pulse"`
	Relayed bool        `json:"relayed,omitempty"`
}

// PollResponse represents the JSON returned by GET /rf/poll on rfbot.
type PollResponse struct {
	Listening bool       `json:"listening"`
	Packets   []RFPacket `json:"packets"`
}

// RFPoller manages active HTTP polling against rfbot receiver gateways.
type RFPoller struct {
	cfg        *config.Config
	engine     *rules.Engine
	client     *http.Client
	done       chan struct{}
	activeBots map[string]bool
	mu         sync.RWMutex
}

// NewRFPoller creates an RF poller service.
func NewRFPoller(cfg *config.Config, engine *rules.Engine) *RFPoller {
	return &RFPoller{
		cfg:        cfg,
		engine:     engine,
		client:     &http.Client{Timeout: 2 * time.Second},
		done:       make(chan struct{}),
		activeBots: make(map[string]bool),
	}
}

// Start begins the background polling loop.
func (p *RFPoller) Start() {
	go p.pollLoop()
}

// Stop halts the background poller.
func (p *RFPoller) Stop() {
	close(p.done)
}

// SetBotListening explicitly enables or disables polling for a specific bot ID.
func (p *RFPoller) SetBotListening(botID string, enable bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.activeBots[botID] = enable
}

// GetActiveListeners returns the list of bot IDs currently being polled.
func (p *RFPoller) GetActiveListeners() []string {
	p.mu.RLock()
	defer p.mu.RUnlock()

	var list []string
	for botID, active := range p.activeBots {
		if active {
			list = append(list, botID)
		}
	}
	return list
}

func (p *RFPoller) pollLoop() {
	ticker := time.NewTicker(800 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-p.done:
			return
		case <-ticker.C:
			p.pollAllReceivers()
		}
	}
}

func (p *RFPoller) pollAllReceivers() {
	// Find candidate RFBot receiver nodes
	var candidates []config.RobotNode
	for _, bot := range p.cfg.Bots {
		if bot.Platform != config.PlatformRFBot {
			continue
		}

		roleLower := strings.ToLower(bot.Role)
		// Check if role is receiver, transceiver, or if manually marked active
		p.mu.RLock()
		overrideActive, hasOverride := p.activeBots[bot.ID]
		p.mu.RUnlock()

		if (hasOverride && overrideActive) || (!hasOverride && (roleLower == "receiver" || roleLower == "transceiver" || roleLower == "")) {
			candidates = append(candidates, bot)
		}
	}

	for _, bot := range candidates {
		go p.pollBot(bot)
	}
}

func isValidIP(ip string) bool {
	return ip != "" && strings.TrimSpace(ip) != "" && !strings.Contains(ip, ".local")
}

func (p *RFPoller) pollBot(bot config.RobotNode) {
	port := bot.Port
	if port == 0 || port == 4330 {
		port = 80
	}

	// Build candidate targets in priority order: IP-first (most reliable on Pi/Linux),
	// Fallback IP second, and mDNS hostname third.
	var candidates []string
	if bot.IP != "" && isValidIP(bot.IP) {
		candidates = append(candidates, bot.IP)
	}
	if bot.FallbackIP != "" && isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP {
		candidates = append(candidates, bot.FallbackIP)
	}
	if bot.Hostname != "" {
		candidates = append(candidates, bot.Hostname)
		clean := strings.TrimSuffix(bot.Hostname, ".local")
		if !strings.HasSuffix(bot.Hostname, ".local") {
			candidates = append(candidates, clean+".local")
		}
	}
	if bot.ID != "" && !strings.Contains(bot.ID, ":") {
		cleanID := strings.TrimSuffix(bot.ID, ".local")
		candidates = append(candidates, cleanID+".local")
	}

	if len(candidates) == 0 {
		return
	}

	// Deduplicate candidates
	seen := make(map[string]bool)
	var uniqueCandidates []string
	for _, c := range candidates {
		if !seen[c] {
			seen[c] = true
			uniqueCandidates = append(uniqueCandidates, c)
		}
	}

	gatewayName := bot.Name
	if gatewayName == "" {
		gatewayName = bot.Hostname
	}
	if gatewayName == "" {
		gatewayName = bot.ID
	}

	for _, targetHost := range uniqueCandidates {
		baseURL := fmt.Sprintf("http://%s:%d", targetHost, port)

		// Ensure listen mode is started on rfbot
		startURL := baseURL + "/rf/listen/start"
		reqStart, err := http.NewRequest("GET", startURL, nil)
		if err == nil {
			resp, err := p.client.Do(reqStart)
			if err == nil {
				_ = resp.Body.Close()
			}
		}

		// Poll for received RF packets
		pollURL := baseURL + "/rf/poll"
		reqPoll, err := http.NewRequest("GET", pollURL, nil)
		if err != nil {
			continue
		}

		resp, err := p.client.Do(reqPoll)
		if err != nil {
			continue
		}

		if resp.StatusCode != http.StatusOK {
			_ = resp.Body.Close()
			continue
		}

		var pollResp PollResponse
		err = json.NewDecoder(resp.Body).Decode(&pollResp)
		_ = resp.Body.Close()
		if err != nil {
			continue
		}

		// Successfully contacted gateway
		if len(pollResp.Packets) > 0 {
			for _, pkt := range pollResp.Packets {
				// Filter out noise fragments (valid RF remotes & sensors use >= 20 bits, usually 24)
				if pkt.Bits > 0 && pkt.Bits < 20 {
					continue
				}

				codeUint := parseCode(pkt.Code)
				if codeUint == 0 {
					continue
				}

				payload := rules.EventPayload{
					Code:      codeUint,
					Bits:      pkt.Bits,
					Protocol:  pkt.Proto,
					Pulse:     pkt.Pulse,
					Gateway:   gatewayName,
					Timestamp: time.Now(),
				}

				log.Printf("[RFPoller] Captured signal 0x%X (%d) from gateway %s", codeUint, codeUint, gatewayName)
				p.engine.ProcessEvent(payload)
			}
		}

		// Reached bot successfully, no need to query other candidate IPs for this cycle
		break
	}
}

// parseCode converts various JSON types (hex string "1E240" from rfbot's %lX format, dec int 123456, etc) to uint32.
func parseCode(raw interface{}) uint32 {
	if raw == nil {
		return 0
	}

	switch v := raw.(type) {
	case float64:
		return uint32(v)
	case int:
		return uint32(v)
	case int64:
		return uint32(v)
	case uint32:
		return v
	case string:
		clean := strings.TrimSpace(v)
		if clean == "" {
			return 0
		}
		// rfbot formats packets as hex strings via snprintf(hex, "%lX", code)
		if strings.HasPrefix(clean, "0x") || strings.HasPrefix(clean, "0X") {
			if val, err := strconv.ParseUint(clean[2:], 16, 32); err == nil {
				return uint32(val)
			}
		}
		// Try parsing as hex (standard rfbot output format)
		if val, err := strconv.ParseUint(clean, 16, 32); err == nil {
			return uint32(val)
		}
		// Fallback decimal parse
		if val, err := strconv.ParseUint(clean, 10, 32); err == nil {
			return uint32(val)
		}
	}
	return 0
}
