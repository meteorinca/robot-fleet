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
