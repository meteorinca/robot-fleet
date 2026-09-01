package rfpoller

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
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

func (p *RFPoller) pollBot(bot config.RobotNode) {
	port := bot.Port
	if port == 0 || port == 4330 {
		port = 80
	}

	// Build candidate URLs with IP-first ordering (reliable on Pi and low-latency)
	var candidateURLs []string
	if isValidIP(bot.IP) {
		candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", bot.IP, port))
	}
	if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP {
		candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", bot.FallbackIP, port))
	}
	if bot.Hostname != "" {
		candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", bot.Hostname, port))
	}
	if len(candidateURLs) == 0 {
		return
	}

	for _, baseURL := range candidateURLs {
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

		// If the receiver node reports listening is false, turn listen mode ON once.
		// Never call /rf/listen/start on every tick because rfbot firmware resets the packet buffer on start.
		if !pollResp.Listening {
			startURL := baseURL + "/rf/listen/start"
			reqStart, err := http.NewRequest("GET", startURL, nil)
			if err == nil {
				respStart, err := p.client.Do(reqStart)
				if err == nil {
					_ = respStart.Body.Close()
				}
			}
		}

		if len(pollResp.Packets) == 0 {
			// Successfully polled, no packets this tick
			return
		}

		gatewayName := bot.Name
		if gatewayName == "" {
			gatewayName = bot.Hostname
		}
		if gatewayName == "" {
			gatewayName = bot.ID
		}

		for _, pkt := range pollResp.Packets {
			// Filter noise fragments (RC-switch remotes and sensors use >= 12 bits, typically 24)
			if pkt.Bits > 0 && pkt.Bits < 12 {
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

			log.Printf("[RFPoller] Captured RF signal 0x%X (%d) [%db proto=%d pulse=%dus] from gateway %s",
				codeUint, codeUint, pkt.Bits, pkt.Proto, pkt.Pulse, gatewayName)
			p.engine.ProcessEvent(payload)
		}

		// Successfully polled from this candidate
		return
	}
}

func isValidIP(ip string) bool {
	return ip != "" && net.ParseIP(strings.TrimSpace(ip)) != nil
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
