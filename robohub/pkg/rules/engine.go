package rules

import (
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/robohub/pkg/config"
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

// SnoozeInfo represents active snooze status for a rule.
type SnoozeInfo struct {
	RuleID       string    `json:"rule_id"`
	RuleName     string    `json:"rule_name"`
	SnoozedUntil time.Time `json:"snoozed_until"`
	RemainingSec int64     `json:"remaining_sec"`
}

type buttonTracker struct {
	count  int
	timer  *time.Timer
	button config.InputButton
}

// Engine evaluates incoming signals against active rules and executes actions.
type Engine struct {
	cfg            *config.Config
	client         *http.Client
	database       DatabaseRecorder
	onEventCb      func(EventPayload, []string) // callback for WebSockets / HomeKit
	onCountCb      func(code uint32, totalCount int64)
	mu             sync.RWMutex
	lastCodeTx     map[uint32]time.Time
	lastRuleExec   map[string]time.Time
	snoozedRules   map[string]time.Time
	buttonTrackers map[string]*buttonTracker
}

// DatabaseRecorder defines the interface for logging RF events to the database.
type DatabaseRecorder interface {
	RecordRFEvent(code uint32, bits, proto, pulse uint, gateway, ruleName string, ts time.Time) (int64, error)
}

// NewEngine creates a new fast rule engine instance.
func NewEngine(cfg *config.Config, eventCb func(EventPayload, []string)) *Engine {
	return &Engine{
		cfg:            cfg,
		onEventCb:      eventCb,
		lastCodeTx:     make(map[uint32]time.Time),
		lastRuleExec:   make(map[string]time.Time),
		snoozedRules:   make(map[string]time.Time),
		buttonTrackers: make(map[string]*buttonTracker),
		client: &http.Client{
			Timeout: 3 * time.Second,
		},
	}
}

// SetDB connects the database to the rule engine.
func (e *Engine) SetDB(d DatabaseRecorder) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.database = d
}

// SetCountCallback sets the callback when an RF signal count increments.
func (e *Engine) SetCountCallback(cb func(code uint32, totalCount int64)) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.onCountCb = cb
}

// SnoozeRule snoozes a rule (or "all") for a specified duration in minutes.
func (e *Engine) SnoozeRule(ruleID string, durationMinutes int) time.Time {
	e.mu.Lock()
	defer e.mu.Unlock()

	until := time.Now().Add(time.Duration(durationMinutes) * time.Minute)

	if ruleID == "all" || ruleID == "" {
		for _, r := range e.cfg.Rules {
			e.snoozedRules[r.ID] = until
		}
	} else {
		e.snoozedRules[ruleID] = until
	}

	return until
}

// CancelSnooze removes snooze for a rule (or "all").
func (e *Engine) CancelSnooze(ruleID string) {
	e.mu.Lock()
	defer e.mu.Unlock()

	if ruleID == "all" || ruleID == "" {
		e.snoozedRules = make(map[string]time.Time)
	} else {
		delete(e.snoozedRules, ruleID)
	}
}

// GetActiveSnoozes returns a list of active non-expired snoozes.
func (e *Engine) GetActiveSnoozes() []SnoozeInfo {
	e.mu.RLock()
	defer e.mu.RUnlock()

	now := time.Now()
	var list []SnoozeInfo

	for ruleID, until := range e.snoozedRules {
		if now.Before(until) {
			name := ruleID
			for _, r := range e.cfg.Rules {
				if r.ID == ruleID {
					name = r.Name
					break
				}
			}
			rem := int64(until.Sub(now).Seconds())
			list = append(list, SnoozeInfo{
				RuleID:       ruleID,
				RuleName:     name,
				SnoozedUntil: until,
				RemainingSec: rem,
			})
		}
	}

	return list
}

// TriggerInputButton processes an input button click with multi-click window aggregation.
func (e *Engine) TriggerInputButton(btn config.InputButton) {
	e.mu.Lock()
	defer e.mu.Unlock()

	tracker, exists := e.buttonTrackers[btn.ID]
	if exists && tracker.timer != nil {
		tracker.count++
	} else {
		tracker = &buttonTracker{
			count:  1,
			button: btn,
		}
		e.buttonTrackers[btn.ID] = tracker

		windowMs := btn.MultiClickWindowMs
		if windowMs <= 0 {
			windowMs = 2500
		}

		buttonID := btn.ID
		tracker.timer = time.AfterFunc(time.Duration(windowMs)*time.Millisecond, func() {
			e.finalizeButtonClick(buttonID)
		})
	}
}

func (e *Engine) finalizeButtonClick(buttonID string) {
	e.mu.Lock()
	tracker, exists := e.buttonTrackers[buttonID]
	if !exists {
		e.mu.Unlock()
		return
	}
	delete(e.buttonTrackers, buttonID)
	btn := tracker.button
	clicks := tracker.count
	e.mu.Unlock()

	minsPerClick := btn.MinutesPerClick
	if minsPerClick <= 0 {
		minsPerClick = 15
	}
	totalMins := clicks * minsPerClick

	var executedLogs []string

	switch btn.ActionType {
	case "snooze_rule", "snooze":
		ruleID := btn.TargetRuleID
		until := e.SnoozeRule(ruleID, totalMins)
		executedLogs = append(executedLogs, fmt.Sprintf("🔘 Button '%s' clicked %dx -> Snoozed rule '%s' for %dm (until %s)",
			btn.Name, clicks, ruleID, totalMins, until.Format("15:04:05")))

	case "snooze_all":
		until := e.SnoozeRule("all", totalMins)
		executedLogs = append(executedLogs, fmt.Sprintf("🔘 Button '%s' clicked %dx -> Snoozed ALL rules for %dm (until %s)",
			btn.Name, clicks, totalMins, until.Format("15:04:05")))

	default:
		executedLogs = append(executedLogs, fmt.Sprintf("🔘 Button '%s' clicked %dx -> Executing default trigger", btn.Name, clicks))
	}

	// Dispatch audio/visual feedback if target set
	if btn.FeedbackTarget != "" {
		go e.executeAction(config.RuleAction{
			Type:   "http_get",
			Target: btn.FeedbackTarget,
			Path:   fmt.Sprintf("/choola?snooze=%d", totalMins),
		})
	}

	// Notify WebSocket clients of button execution
	if e.onEventCb != nil {
		e.onEventCb(EventPayload{
			Code:      btn.TriggerCode,
			Timestamp: time.Now(),
			Gateway:   "InputButton",
		}, executedLogs)
	}
}

// ProcessEvent receives an incoming RF packet and triggers matching rules asynchronously.
func (e *Engine) ProcessEvent(event EventPayload) {
	e.mu.Lock()
	// Debounce identical RF code bursts (100ms micro-debounce)
	if last, exists := e.lastCodeTx[event.Code]; exists && time.Since(last) < 100*time.Millisecond {
		e.mu.Unlock()
		return
	}
	e.lastCodeTx[event.Code] = time.Now()
	now := time.Now()
	e.mu.Unlock()

	var executedLogs []string

	// 1. Check matching Input Buttons first
	for _, btn := range e.cfg.Buttons {
		if btn.TriggerCode == event.Code {
			executedLogs = append(executedLogs, fmt.Sprintf("Registered click for Input Button '%s' (Code %d)", btn.Name, btn.TriggerCode))
			go e.TriggerInputButton(btn)
		}
	}

	// 2. Find matching Automation Rules
	for _, rule := range e.cfg.Rules {
		if !rule.Enabled || rule.TriggerCode != event.Code {
			continue
		}

		e.mu.RLock()
		// Check Snooze state
		if snoozedUntil, snoozed := e.snoozedRules[rule.ID]; snoozed && now.Before(snoozedUntil) {
			e.mu.RUnlock()
			executedLogs = append(executedLogs, fmt.Sprintf("Rule '%s' SUPPRESSED (Snoozed until %s)", rule.Name, snoozedUntil.Format("15:04:05")))
			continue
		}

		// Check Rule Cooldown
		cooldownSec := rule.CooldownSec
		if cooldownSec <= 0 {
			cooldownSec = 30 // Default 30s cooldown to eliminate continuous spamming
		}
		if lastExec, exists := e.lastRuleExec[rule.ID]; exists && now.Sub(lastExec) < time.Duration(cooldownSec)*time.Second {
			e.mu.RUnlock()
			executedLogs = append(executedLogs, fmt.Sprintf("Rule '%s' SUPPRESSED (Cooldown %ds active)", rule.Name, cooldownSec))
			continue
		}
		e.mu.RUnlock()

		// Update last execution time for rule
		e.mu.Lock()
		e.lastRuleExec[rule.ID] = now
		e.mu.Unlock()

		// Dispatch Rule Actions
		for _, action := range rule.Actions {
			actionCopy := action
			ruleName := rule.Name
			executedLogs = append(executedLogs, fmt.Sprintf("Rule '%s' -> %s (%s)", ruleName, actionCopy.Type, actionCopy.Target))

			go e.executeAction(actionCopy)
		}
	}
	if event.Gateway != "" && !strings.Contains(event.Gateway, "[") && !strings.Contains(event.Gateway, ":") && !strings.EqualFold(event.Gateway, "InputButton") {
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

	// Persist to SQLite Database & increment aggregate counter
	if e.database != nil {
		ruleSummary := ""
		if len(executedLogs) > 0 {
			ruleSummary = strings.Join(executedLogs, " | ")
		}
		newCount, err := e.database.RecordRFEvent(event.Code, event.Bits, event.Protocol, event.Pulse, event.Gateway, ruleSummary, now)
		if err == nil && e.onCountCb != nil {
			e.onCountCb(event.Code, newCount)
		}
	}

	// Notify WebSockets and HomeKit bridge
	if e.onEventCb != nil {
		e.onEventCb(event, executedLogs)
	}
}

// executeAction dispatches a single HTTP GET/POST or SpeakerBot call using IP fallback.
func (e *Engine) executeAction(action config.RuleAction) {
	targetRaw := strings.TrimSpace(action.Target)
	if targetRaw == "" {
		return
	}

	cleanTarget := targetRaw
	cleanTarget = strings.TrimPrefix(cleanTarget, "http://")
	cleanTarget = strings.TrimPrefix(cleanTarget, "https://")
	cleanTarget = strings.TrimRight(cleanTarget, "/")

	// Extract host and possible port
	var hostOnly string
	if strings.Contains(cleanTarget, ":") {
		parts := strings.Split(cleanTarget, ":")
		hostOnly = parts[0]
	} else {
		hostOnly = cleanTarget
	}

	// Try matching against known bots in config
	var matchedBot *config.RobotNode
	for _, b := range e.cfg.Bots {
		cleanBotHost := strings.TrimSuffix(b.Hostname, ".local")
		cleanSearchHost := strings.TrimSuffix(hostOnly, ".local")
		if strings.EqualFold(b.ID, hostOnly) ||
			strings.EqualFold(b.Hostname, hostOnly) ||
			strings.EqualFold(b.IP, hostOnly) ||
			strings.EqualFold(b.Name, hostOnly) ||
			strings.EqualFold(cleanBotHost, cleanSearchHost) {
			matchedBot = &b
			break
		}
	}

	var candidateURLs []string
	if matchedBot != nil {
		port := matchedBot.Port
		if port == 0 || port == 4330 {
			port = 80
		}
		if matchedBot.IP != "" && isValidIP(matchedBot.IP) {
			candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", matchedBot.IP, port))
		}
		if matchedBot.FallbackIP != "" && isValidIP(matchedBot.FallbackIP) && matchedBot.FallbackIP != matchedBot.IP {
			candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", matchedBot.FallbackIP, port))
		}
		if matchedBot.Hostname != "" {
			candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s:%d", matchedBot.Hostname, port))
			clean := strings.TrimSuffix(matchedBot.Hostname, ".local")
			if !strings.HasSuffix(matchedBot.Hostname, ".local") {
				candidateURLs = append(candidateURLs, fmt.Sprintf("http://%s.local:%d", clean, port))
			}
		}
	}

	// Always append raw target URL as fallback
	if strings.HasPrefix(targetRaw, "http://") || strings.HasPrefix(targetRaw, "https://") {
		candidateURLs = append(candidateURLs, targetRaw)
	} else {
		candidateURLs = append(candidateURLs, "http://"+targetRaw)
	}

	// Deduplicate candidate URLs
	seen := make(map[string]bool)
	for _, base := range candidateURLs {
		if seen[base] {
			continue
		}
		seen[base] = true

		fullURL := strings.TrimRight(base, "/") + "/" + strings.TrimLeft(action.Path, "/")
		req, err := http.NewRequest("GET", fullURL, nil)
		if err != nil {
			continue
		}

		req.Header.Set("User-Agent", "FleetHub-Mothership/1.0")

		resp, err := e.client.Do(req)
		if err == nil {
			_ = resp.Body.Close()
			return // Success!
		}
	}
}

func isValidIP(ip string) bool {
	return ip != "" && strings.TrimSpace(ip) != "" && !strings.Contains(ip, ".local")
}
