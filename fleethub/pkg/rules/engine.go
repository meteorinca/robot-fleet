package rules

import (
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

// EventPayload represents an incoming RF signal or event trigger.
type EventPayload struct {
	Code      uint32    `json:"code"`
	Bits      uint      `json:"bits"`
	Protocol  uint      `json:"protocol"`
	Pulse     uint      `json:"pulse"`
	Gateway   string    `json:"gateway"`
	Timestamp time.Time `json:"timestamp"`
}

// Engine evaluates incoming signals against active rules and executes actions.
type Engine struct {
	cfg        *config.Config
	client     *http.Client
	onEventCb  func(EventPayload, []string) // callback for WebSockets / HomeKit
	mu         sync.RWMutex
	lastRuleTx map[uint32]time.Time
}

// NewEngine creates a new fast rule engine instance.
func NewEngine(cfg *config.Config, eventCb func(EventPayload, []string)) *Engine {
	return &Engine{
		cfg:        cfg,
		onEventCb:  eventCb,
		lastRuleTx: make(map[uint32]time.Time),
		client: &http.Client{
			Timeout: 3 * time.Second,
		},
	}
}

// ProcessEvent receives an incoming RF packet and triggers matching rules asynchronously.
func (e *Engine) ProcessEvent(event EventPayload) {
	e.mu.Lock()
	// Rate-limit identical RF codes at the hub level to prevent duplicate triggers (1-second debounce window)
	if last, exists := e.lastRuleTx[event.Code]; exists && time.Since(last) < 1000*time.Millisecond {
		e.mu.Unlock()
		return
	}
	e.lastRuleTx[event.Code] = time.Now()
	e.mu.Unlock()

	var executedLogs []string

	// Find matching rules
	for _, rule := range e.cfg.Rules {
		if !rule.Enabled || rule.TriggerCode != event.Code {
			continue
		}

		for _, action := range rule.Actions {
			actionCopy := action
			ruleName := rule.Name
			executedLogs = append(executedLogs, fmt.Sprintf("Rule '%s' -> %s (%s)", ruleName, actionCopy.Type, actionCopy.Target))

			// Dispatch each action concurrently in a goroutine for zero latency blocking
			go e.executeAction(actionCopy)
		}
	}

	// Auto-register transceiving gateway bot in Fleet Registry if present
	if event.Gateway != "" {
		platform := config.PlatformRFBot
		lower := strings.ToLower(event.Gateway)
		if strings.Contains(lower, "speaker") {
			platform = config.PlatformSpeakerBot
		} else if strings.Contains(lower, "simple") {
			platform = config.PlatformSimpleBot
		} else if strings.Contains(lower, "dog") {
			platform = config.PlatformDogBot
		}
		e.cfg.UpsertBot(config.RobotNode{
			ID:       event.Gateway,
			Name:     event.Gateway,
			Platform: platform,
			IP:       event.Gateway,
			Port:     4330,
			Status:   "online",
		})
	}

	// Update sensor state timestamp if mapped
	for i, s := range e.cfg.Sensors {
		if s.Code == event.Code {
			e.cfg.Sensors[i].LastTriggered = time.Now()
			e.cfg.Sensors[i].State = !e.cfg.Sensors[i].State
			break
		}
	}

	// Notify WebSockets and HomeKit bridge
	if e.onEventCb != nil {
		e.onEventCb(event, executedLogs)
	}
}

// executeAction dispatches a single HTTP GET/POST or SpeakerBot call.
func (e *Engine) executeAction(action config.RuleAction) {
	target := action.Target
	if !strings.HasPrefix(target, "http://") && !strings.HasPrefix(target, "https://") {
		target = "http://" + target
	}

	fullURL := strings.TrimRight(target, "/") + "/" + strings.TrimLeft(action.Path, "/")
	req, err := http.NewRequest("GET", fullURL, nil)
	if err != nil {
		return
	}

	req.Header.Set("User-Agent", "FleetHub-Mothership/1.0")

	resp, err := e.client.Do(req)
	if err != nil {
		return
	}
	_ = resp.Body.Close()
}
