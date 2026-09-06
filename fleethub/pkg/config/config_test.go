package config

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestConfigRuntimeStateSeparation(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "fleethub_config_test")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	configPath := filepath.Join(tempDir, "fleethub_config.json")
	knownDevicesPath := filepath.Join(tempDir, "known_devices.json")
	statePath := filepath.Join(tempDir, "fleethub_state.generated.json")

	// 1. Create a clean base config
	initialCfg := DefaultConfig(configPath)
	if err := initialCfg.Save(); err != nil {
		t.Fatalf("Failed to save initial config: %v", err)
	}

	// 2. Create known_devices.json with a custom bot
	customBots := []RobotNode{
		{
			ID:         "testbot1",
			Name:       "TestBot 1",
			Platform:   PlatformSimpleBot,
			IP:         "10.0.0.99",
			Port:       80,
			Status:     "online",
			FallbackIP: "10.0.0.99",
		},
	}
	knownData, err := json.MarshalIndent(customBots, "", "  ")
	if err != nil {
		t.Fatalf("Failed to marshal known bots: %v", err)
	}
	if err := os.WriteFile(knownDevicesPath, knownData, 0644); err != nil {
		t.Fatalf("Failed to write known_devices.json: %v", err)
	}

	// 3. Load config and verify custom bot was merged
	cfg, err := LoadConfig(configPath)
	if err != nil {
		t.Fatalf("Failed to load config: %v", err)
	}

	foundTestBot := false
	for _, b := range cfg.Bots {
		if b.ID == "testbot1" {
			foundTestBot = true
			break
		}
	}
	if !foundTestBot {
		t.Fatalf("Expected testbot1 to be merged from known_devices.json")
	}

	// 4. Simulate runtime ping updates (churning latency and last_seen)
	testTimestamp := time.Now().Add(-5 * time.Minute).Truncate(time.Millisecond)
	cfg.UpsertBot(RobotNode{
		ID:        "testbot1",
		LatencyMs: 42.5,
		LastSeen:  testTimestamp,
		Status:    "online",
	})

	// 5. Save() should save CLEAN config without LatencyMs and LastSeen, and must NOT overwrite known_devices.json
	if err := cfg.Save(); err != nil {
		t.Fatalf("Failed to save config: %v", err)
	}

	// Verify known_devices.json was untouched
	afterKnownData, err := os.ReadFile(knownDevicesPath)
	if err != nil {
		t.Fatalf("Failed to read known_devices.json: %v", err)
	}
	if string(afterKnownData) != string(knownData) {
		t.Fatalf("known_devices.json was modified by cfg.Save()! Expected it to remain untouched.")
	}

	// Verify fleethub_config.json does not contain the runtime LatencyMs (42.5)
	cfgContent, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("Failed to read fleethub_config.json: %v", err)
	}
	var parsedCfg Config
	if err := json.Unmarshal(cfgContent, &parsedCfg); err != nil {
		t.Fatalf("Failed to parse fleethub_config.json: %v", err)
	}
	for _, b := range parsedCfg.Bots {
		if b.ID == "testbot1" && b.LatencyMs != 0 {
			t.Fatalf("Expected LatencyMs to be stripped in saved config, got: %v", b.LatencyMs)
		}
		if b.ID == "testbot1" && !b.LastSeen.IsZero() {
			t.Fatalf("Expected LastSeen to be zeroed in saved config, got: %v", b.LastSeen)
		}
	}

	// 6. SaveRuntimeState() should write to fleethub_state.generated.json
	if err := cfg.SaveRuntimeState(); err != nil {
		t.Fatalf("Failed to save runtime state: %v", err)
	}

	if _, err := os.Stat(statePath); os.IsNotExist(err) {
		t.Fatalf("Expected fleethub_state.generated.json to exist at %s", statePath)
	}

	stateContent, err := os.ReadFile(statePath)
	if err != nil {
		t.Fatalf("Failed to read runtime state file: %v", err)
	}
	var state FleetStateGenerated
	if err := json.Unmarshal(stateContent, &state); err != nil {
		t.Fatalf("Failed to unmarshal runtime state: %v", err)
	}

	foundStateBot := false
	for _, b := range state.Bots {
		if b.ID == "testbot1" {
			foundStateBot = true
			if b.LatencyMs != 42.5 {
				t.Fatalf("Expected LatencyMs 42.5 in runtime state, got %v", b.LatencyMs)
			}
			break
		}
	}
	if !foundStateBot {
		t.Fatalf("Expected testbot1 in runtime state file")
	}

	// 7. Test loading from scratch with state file present overlays runtime fields
	reloadedCfg, err := LoadConfig(configPath)
	if err != nil {
		t.Fatalf("Failed to reload config: %v", err)
	}

	for _, b := range reloadedCfg.Bots {
		if b.ID == "testbot1" {
			if b.LatencyMs != 42.5 {
				t.Fatalf("Expected reloaded bot to have LatencyMs 42.5 from state file, got %v", b.LatencyMs)
			}
		}
	}
}

func TestSetBotHomeKit(t *testing.T) {
	cfg := &Config{
		Bots: []RobotNode{
			{
				ID:             "bot-hk-test",
				Name:           "Living Room Bot",
				Hostname:       "living-room.local",
				Platform:       PlatformSimpleBot,
				HomeKitEnabled: true,
			},
		},
	}

	// Disable HomeKit
	ok := cfg.SetBotHomeKit("bot-hk-test", false)
	if !ok {
		t.Fatalf("Expected SetBotHomeKit to return true for existing bot")
	}
	if cfg.Bots[0].HomeKitEnabled {
		t.Fatalf("Expected HomeKitEnabled to be false after disabling")
	}
	if cfg.Bots[0].Name != "Living Room Bot" || cfg.Bots[0].Hostname != "living-room.local" {
		t.Fatalf("Expected bot metadata to remain intact, got: %+v", cfg.Bots[0])
	}

	// Re-enable HomeKit
	ok = cfg.SetBotHomeKit("bot-hk-test", true)
	if !ok || !cfg.Bots[0].HomeKitEnabled {
		t.Fatalf("Expected HomeKitEnabled to be re-enabled")
	}

	// Non-existent bot
	if cfg.SetBotHomeKit("non-existent", true) {
		t.Fatalf("Expected SetBotHomeKit to return false for non-existent bot")
	}
}

func TestDeleteBot(t *testing.T) {
	cfg := &Config{
		Bots: []RobotNode{
			{ID: "bot-1", Name: "Bot One"},
			{ID: "bot-2", Name: "Bot Two"},
		},
	}

	deleted := cfg.DeleteBot("bot-1")
	if !deleted {
		t.Fatalf("Expected DeleteBot to return true for bot-1")
	}
	if len(cfg.Bots) != 1 || cfg.Bots[0].ID != "bot-2" {
		t.Fatalf("Expected only bot-2 to remain, got %+v", cfg.Bots)
	}

	// Delete non-existent
	if cfg.DeleteBot("bot-999") {
		t.Fatalf("Expected DeleteBot to return false for bot-999")
	}
}

func TestUpsertMultipleRules(t *testing.T) {
	cfg := &Config{
		Rules: []AutomationRule{
			{ID: "rule-1", Name: "Initial Rule", TriggerCode: 123456},
		},
	}

	// Add second rule with same trigger code (e.g. sensor 123456 triggers another action)
	cfg.UpsertRule(AutomationRule{
		ID:          "rule-2",
		Name:        "Second Rule",
		TriggerCode: 123456,
	})

	if len(cfg.Rules) != 2 {
		t.Fatalf("Expected 2 rules, got %d. Rules should not be clobbered by same TriggerCode!", len(cfg.Rules))
	}

	// Add third rule with empty ID (should auto-generate ID)
	cfg.UpsertRule(AutomationRule{
		Name:        "Third Auto ID Rule",
		TriggerCode: 123456,
	})

	if len(cfg.Rules) != 3 {
		t.Fatalf("Expected 3 rules, got %d", len(cfg.Rules))
	}
	if cfg.Rules[2].ID == "" {
		t.Fatalf("Expected generated ID on third rule")
	}

	// Updating rule-1 by ID should update it without creating a new rule
	cfg.UpsertRule(AutomationRule{
		ID:          "rule-1",
		Name:        "Updated Initial Rule",
		TriggerCode: 999999,
	})
	if len(cfg.Rules) != 3 {
		t.Fatalf("Expected still 3 rules after updating rule-1, got %d", len(cfg.Rules))
	}
	if cfg.Rules[0].Name != "Updated Initial Rule" {
		t.Fatalf("Expected rule-1 to be updated")
	}
}


