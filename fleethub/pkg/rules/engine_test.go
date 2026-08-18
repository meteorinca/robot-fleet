package rules

import (
	"sync/atomic"
	"testing"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

func TestEngineCooldownAndSnooze(t *testing.T) {
	cfg := &config.Config{
		HTTPPort: 8080,
		UDPPort:  4330,
		Rules: []config.AutomationRule{
			{
				ID:          "stove-rule",
				Name:        "Photodetector Stove Alert",
				Enabled:     true,
				TriggerCode: 123456,
				CooldownSec: 2, // 2-second cooldown
				Snoozeable:  true,
				Actions: []config.RuleAction{
					{
						Type:   "http_get",
						Target: "http://localhost:18888",
						Path:   "/test",
					},
				},
			},
		},
		Buttons: []config.InputButton{
			{
				ID:                 "snooze-btn",
				Name:               "Kitchen Remote Snooze Button",
				TriggerCode:        5577987,
				ActionType:         "snooze_rule",
				TargetRuleID:       "stove-rule",
				MinutesPerClick:    15,
				MultiClickWindowMs: 200, // Fast 200ms window for unit test
			},
		},
	}

	var eventLogs []string
	var eventCount int32

	engine := NewEngine(cfg, func(ep EventPayload, logs []string) {
		atomic.AddInt32(&eventCount, 1)
		eventLogs = append(eventLogs, logs...)
	})

	// 1. Process initial event
	engine.ProcessEvent(EventPayload{Code: 123456, Timestamp: time.Now()})
	time.Sleep(50 * time.Millisecond)

	// 2. Fire immediate duplicate event (should be suppressed by cooldown)
	engine.ProcessEvent(EventPayload{Code: 123456, Timestamp: time.Now()})
	time.Sleep(50 * time.Millisecond)

	// 3. Trigger multi-click input button (3 clicks)
	btn := cfg.Buttons[0]
	engine.TriggerInputButton(btn)
	engine.TriggerInputButton(btn)
	engine.TriggerInputButton(btn)

	// Wait for multi-click window (200ms) to complete
	time.Sleep(350 * time.Millisecond)

	// Verify active snoozes
	snoozes := engine.GetActiveSnoozes()
	if len(snoozes) != 1 {
		t.Fatalf("Expected 1 active snooze, got %d", len(snoozes))
	}

	s := snoozes[0]
	if s.RuleID != "stove-rule" {
		t.Errorf("Expected snoozed rule ID 'stove-rule', got %s", s.RuleID)
	}

	// 3 clicks * 15 minutes = 45 minutes (2700 seconds)
	if s.RemainingSec < 2600 || s.RemainingSec > 2710 {
		t.Errorf("Expected remaining snooze time around 2700s (45m), got %d seconds", s.RemainingSec)
	}

	// 4. Fire event while snoozed — must be suppressed by Snooze
	engine.ProcessEvent(EventPayload{Code: 123456, Timestamp: time.Now()})
	time.Sleep(50 * time.Millisecond)

	// 5. Cancel Snooze
	engine.CancelSnooze("stove-rule")
	if len(engine.GetActiveSnoozes()) != 0 {
		t.Errorf("Expected 0 active snoozes after cancel")
	}
}
