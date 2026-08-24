package config

import (
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"time"
)

// BotPlatform represents supported robot types in the robot-fleet ecosystem.
type BotPlatform string

const (
	PlatformRFBot      BotPlatform = "rfbot"
	PlatformSpeakerBot BotPlatform = "speakerbot"
	PlatformDogBot     BotPlatform = "dogbot_v1"
	PlatformSimpleBot  BotPlatform = "simplebot"
	PlatformCarBot     BotPlatform = "carbot"
	PlatformMyBot      BotPlatform = "mybot"
)

// DeviceAction represents an executable action on a known robot device.
type DeviceAction struct {
	Name     string `json:"name"`
	Endpoint string `json:"endpoint"`
	Method   string `json:"method"` // "GET", "POST"
	Payload  string `json:"payload,omitempty"`
}

// RobotNode represents a discovered or configured robot device.
type RobotNode struct {
	ID              string         `json:"id"`
	Name            string         `json:"name"`
	Hostname        string         `json:"hostname,omitempty"`
	FallbackIP      string         `json:"fallback_ip,omitempty"`
	Platform        BotPlatform    `json:"platform"`
	IP              string         `json:"ip"`
	MAC             string         `json:"mac,omitempty"`
	Port            int            `json:"port"`
	LastSeen        time.Time      `json:"last_seen"`
	Status          string         `json:"status"` // "online", "offline", "busy"
	Firmware        string         `json:"firmware,omitempty"`
	Role            string         `json:"role,omitempty"`
	HomeKitEnabled  bool           `json:"homekit_enabled"`
	HomeKitCategory string         `json:"homekit_category,omitempty"` // "switch", "speaker", "sensor"
	PingEnabled     bool           `json:"ping_enabled"`
	PingIntervalSec int            `json:"ping_interval_sec,omitempty"`
	LatencyMs       float64        `json:"latency_ms,omitempty"`
	Actions         []DeviceAction `json:"actions,omitempty"`
}

// RFSensor represents a mapped 433 MHz RF sensor (e.g. photodetector, motion sensor, magnetic door switch).
type RFSensor struct {
	Code          uint32    `json:"code"`
	Name          string    `json:"name"`
	AccessoryType string    `json:"accessory_type"` // "motion", "light", "contact", "switch"
	Room          string    `json:"room"`
	LastTriggered time.Time `json:"last_triggered"`
	State         bool      `json:"state"`
}

// RuleAction represents an action triggered by an RF sensor or automation.
type RuleAction struct {
	Type      string `json:"type"`       // "http_get", "http_post", "speakerbot_play", "dogbot_action"
	Target    string `json:"target"`     // URL or target IP/hostname
	Path      string `json:"path"`       // Endpoint path (e.g., /bark or /api/v1/action)
	Payload   string `json:"payload"`    // Body or query param
}

// AutomationRule represents an active rule in the engine.
type AutomationRule struct {
	ID          string       `json:"id"`
	Name        string       `json:"name"`
	Enabled     bool         `json:"enabled"`
	TriggerCode uint32       `json:"trigger_code"`
	CooldownSec int          `json:"cooldown_sec,omitempty"` // Cooldown period in seconds to prevent spam
	Snoozeable  bool         `json:"snoozeable"`            // Allow rule to be snoozed
	Actions     []RuleAction `json:"actions"`
}

// InputButton represents a physical RF button remote code or virtual input button.
type InputButton struct {
	ID                 string `json:"id"`
	Name               string `json:"name"`
	TriggerCode        uint32 `json:"trigger_code"`
	ActionType         string `json:"action_type"`           // "snooze_rule", "snooze_all", "trigger_action", "toggle_device"
	TargetRuleID       string `json:"target_rule_id,omitempty"`
	MinutesPerClick    int    `json:"minutes_per_click,omitempty"`    // e.g. 15 mins per click for snooze
	MultiClickWindowMs int    `json:"multi_click_window_ms,omitempty"`// multi-click aggregation window in ms (default 2500ms)
	FeedbackTarget     string `json:"feedback_target,omitempty"`     // Target device for audio/visual confirmation
}

// Config holds all FleetHub settings and fleet state.
type Config struct {
	HTTPPort   int              `json:"http_port"`
	UDPPort    int              `json:"udp_port"`
	HomeKitPIN string           `json:"homekit_pin"`
	Mode       string           `json:"mode"` // "home_automation" or "stem_classroom"
	Bots       []RobotNode      `json:"bots"`
	Sensors    []RFSensor       `json:"sensors"`
	Rules      []AutomationRule `json:"rules"`
	Buttons    []InputButton    `json:"buttons"`
	filePath   string
	mu         sync.RWMutex
}

// DefaultConfig returns rich robot fleet defaults for PaulBots and MyBots.
func DefaultConfig(path string) *Config {
	bots := []RobotNode{}

	// 1. Generate default PaulBot DogBots (Quadrupeds) 1 to 12
	for i := 1; i <= 12; i++ {
		id := fmt.Sprintf("paulbot%d", i)
		hostname := fmt.Sprintf("paulbot%d.local", i)
		fallbackIP := fmt.Sprintf("192.168.4.%d", 100+i)
		name := fmt.Sprintf("PaulBot %d (DogBot Quad)", i)

		bots = append(bots, RobotNode{
			ID:          id,
			Name:        name,
			Hostname:    hostname,
			FallbackIP:  fallbackIP,
			Platform:    PlatformDogBot,
			IP:          hostname,
			Port:        80,
			Status:      "online",
			Role:        "quadruped_dogbot",
			PingEnabled: true,
			Actions: []DeviceAction{
				{Name: "Stand", Endpoint: "/stand", Method: "GET"},
				{Name: "Wave Hi", Endpoint: "/hi", Method: "GET"},
				{Name: "Wiggle Dance", Endpoint: "/wiggle", Method: "GET"},
				{Name: "Bark", Endpoint: "/bark", Method: "GET"},
				{Name: "Bow", Endpoint: "/bow", Method: "GET"},
				{Name: "Lie Down", Endpoint: "/lay", Method: "GET"},
				{Name: "Jump Forward", Endpoint: "/jump_fwd", Method: "GET"},
				{Name: "Walk Forward", Endpoint: "/walk_fwd", Method: "GET"},
				{Name: "Walk Backward", Endpoint: "/walk_bwd", Method: "GET"},
				{Name: "Shake", Endpoint: "/shake", Method: "GET"},
				{Name: "Poke", Endpoint: "/poke", Method: "GET"},
				{Name: "Kick", Endpoint: "/kick", Method: "GET"},
				{Name: "Rock", Endpoint: "/rock", Method: "GET"},
				{Name: "Sway", Endpoint: "/sway", Method: "GET"},
				{Name: "Lean Back", Endpoint: "/lean", Method: "GET"},
				{Name: "Happy Eyes", Endpoint: "/eye_mood?val=0", Method: "GET"},
				{Name: "Sad Eyes", Endpoint: "/eye_mood?val=1", Method: "GET"},
				{Name: "Neutral Eyes", Endpoint: "/eye_mood?val=2", Method: "GET"},
				{Name: "Angry Eyes", Endpoint: "/eye_mood?val=3", Method: "GET"},
				{Name: "Fireworks OLED", Endpoint: "/anim_fireworks", Method: "GET"},
				{Name: "Matrix Rain OLED", Endpoint: "/anim_matrix", Method: "GET"},
				{Name: "Heartbeat OLED", Endpoint: "/anim_heartbeat", Method: "GET"},
				{Name: "Eyes Mode OLED", Endpoint: "/anim_eyes", Method: "GET"},
				{Name: "Sound: Huh?", Endpoint: "/huh", Method: "GET"},
				{Name: "Sound: Yes", Endpoint: "/yes", Method: "GET"},
				{Name: "Sound: Jump", Endpoint: "/jump", Method: "GET"},
				{Name: "Sound: Ding", Endpoint: "/ding", Method: "GET"},
				{Name: "LED On", Endpoint: "/l1on", Method: "GET"},
				{Name: "LED Off", Endpoint: "/l1off", Method: "GET"},
				{Name: "LED Toggle", Endpoint: "/toggle", Method: "GET"},
			},
		})
	}

	// 2. Generate default MyBots (Breadboard Bots) 1 to 12
	for i := 1; i <= 12; i++ {
		id := fmt.Sprintf("mybot%d", i)
		hostname := fmt.Sprintf("mybot%d.local", i)
		fallbackIP := fmt.Sprintf("192.168.4.%d", 200+i)
		name := fmt.Sprintf("MyBot %d (Breadboard Bot)", i)

		bots = append(bots, RobotNode{
			ID:          id,
			Name:        name,
			Hostname:    hostname,
			FallbackIP:  fallbackIP,
			Platform:    PlatformMyBot,
			IP:          hostname,
			Port:        80,
			Status:      "online",
			Role:        "breadboard_bot",
			PingEnabled: true,
			Actions: []DeviceAction{
				{Name: "Servo 1 ON", Endpoint: "/s1on", Method: "GET"},
				{Name: "Servo 1 OFF", Endpoint: "/s1off", Method: "GET"},
				{Name: "Servo 2 ON", Endpoint: "/s2on", Method: "GET"},
				{Name: "Servo 2 OFF", Endpoint: "/s2off", Method: "GET"},
				{Name: "Random Look ON", Endpoint: "/random_look?on=1", Method: "GET"},
				{Name: "Random Look OFF", Endpoint: "/random_look?on=0", Method: "GET"},
				{Name: "Mario Melody", Endpoint: "/demo?type=mario", Method: "GET"},
				{Name: "Coin SFX", Endpoint: "/demo?type=coin", Method: "GET"},
				{Name: "1Up SFX", Endpoint: "/demo?type=1up", Method: "GET"},
				{Name: "Laser SFX", Endpoint: "/demo?type=laser", Method: "GET"},
				{Name: "Siren SFX", Endpoint: "/demo?type=siren", Method: "GET"},
				{Name: "Game Over SFX", Endpoint: "/demo?type=gameover", Method: "GET"},
				{Name: "Mario Dance OLED", Endpoint: "/anim_mario", Method: "GET"},
				{Name: "Space Invaders OLED", Endpoint: "/anim_invader", Method: "GET"},
				{Name: "Big Yawn OLED", Endpoint: "/big_yawn", Method: "GET"},
				{Name: "Fireworks OLED", Endpoint: "/anim_fireworks", Method: "GET"},
				{Name: "Matrix Rain OLED", Endpoint: "/anim_matrix", Method: "GET"},
				{Name: "Heartbeat OLED", Endpoint: "/anim_heartbeat", Method: "GET"},
				{Name: "Game Off / Normal", Endpoint: "/game_off", Method: "GET"},
				{Name: "Green LED ON", Endpoint: "/grnon", Method: "GET"},
				{Name: "Green LED OFF", Endpoint: "/grnoff", Method: "GET"},
				{Name: "Green LED Tog", Endpoint: "/grntog", Method: "GET"},
				{Name: "Red LED ON", Endpoint: "/redon", Method: "GET"},
				{Name: "Red LED OFF", Endpoint: "/redoff", Method: "GET"},
				{Name: "Red LED Tog", Endpoint: "/redtog", Method: "GET"},
				{Name: "Board LED ON", Endpoint: "/l1on", Method: "GET"},
				{Name: "Board LED OFF", Endpoint: "/l1off", Method: "GET"},
				{Name: "Board LED Tog", Endpoint: "/toggle", Method: "GET"},
			},
		})
	}

	return &Config{
		HTTPPort:   8126,
		UDPPort:    4330,
		HomeKitPIN: "11122333",
		Mode:       "robot_fleet",
		Bots:       bots,
		Sensors:    []RFSensor{},
		Rules:      []AutomationRule{},
		Buttons:    []InputButton{},
		filePath:   path,
	}
}

// LoadConfig reads config from file or creates default, merging known_devices.json if present.
func LoadConfig(path string) (*Config, error) {
	var cfg *Config
	if _, err := os.Stat(path); os.IsNotExist(err) {
		cfg = DefaultConfig(path)
		_ = cfg.Save()
	} else {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("failed to read config file: %w", err)
		}

		cfg = &Config{filePath: path}
		if err := json.Unmarshal(data, cfg); err != nil {
			return nil, fmt.Errorf("failed to parse config JSON: %w", err)
		}
	}

	// Try loading known_devices.json
	knownDevicesPath := "known_devices.json"
	if _, err := os.Stat(knownDevicesPath); err == nil {
		knownData, err := os.ReadFile(knownDevicesPath)
		if err == nil {
			var knownBots []RobotNode
			if err := json.Unmarshal(knownData, &knownBots); err == nil {
				for _, dev := range knownBots {
					cfg.UpsertBot(dev)
				}
			}
		}
	}

	return cfg, nil
}

// Save persists configuration to disk safely.
func (c *Config) Save() error {
	c.mu.RLock()
	defer c.mu.RUnlock()

	data, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}

	// Also sync back to known_devices.json
	knownData, err := json.MarshalIndent(c.Bots, "", "  ")
	if err == nil {
		_ = os.WriteFile("known_devices.json", knownData, 0644)
	}

	return os.WriteFile(c.filePath, data, 0644)
}

// UpsertBot adds or updates a discovered or configured robot node in the fleet registry.
func (c *Config) UpsertBot(node RobotNode) {
	c.mu.Lock()
	defer c.mu.Unlock()

	found := false
	for i, b := range c.Bots {
		if (node.ID != "" && b.ID == node.ID) ||
			(node.Hostname != "" && b.Hostname == node.Hostname) ||
			(node.IP != "" && b.IP == node.IP && b.Platform == node.Platform) ||
			(node.Name != "" && b.Name == node.Name) {

			if node.Name != "" {
				c.Bots[i].Name = node.Name
			}
			if node.Hostname != "" {
				c.Bots[i].Hostname = node.Hostname
			}
			if node.FallbackIP != "" {
				c.Bots[i].FallbackIP = node.FallbackIP
			}
			if node.IP != "" {
				c.Bots[i].IP = node.IP
			}
			if node.Port != 0 {
				c.Bots[i].Port = node.Port
			}
			if node.Platform != "" {
				c.Bots[i].Platform = node.Platform
			}
			if node.Role != "" {
				c.Bots[i].Role = node.Role
			}
			if node.HomeKitCategory != "" {
				c.Bots[i].HomeKitCategory = node.HomeKitCategory
			}
			if len(node.Actions) > 0 {
				c.Bots[i].Actions = node.Actions
			}
			c.Bots[i].HomeKitEnabled = node.HomeKitEnabled || c.Bots[i].HomeKitEnabled
			c.Bots[i].PingEnabled = node.PingEnabled || c.Bots[i].PingEnabled
			if node.LatencyMs > 0 {
				c.Bots[i].LatencyMs = node.LatencyMs
			}
			c.Bots[i].LastSeen = time.Now()
			if node.Status != "" {
				c.Bots[i].Status = node.Status
			} else {
				c.Bots[i].Status = "online"
			}
			found = true
			break
		}
	}

	if !found {
		if node.LastSeen.IsZero() {
			node.LastSeen = time.Now()
		}
		if node.Status == "" {
			node.Status = "online"
		}
		c.Bots = append(c.Bots, node)
	}
}

// UpsertSensor adds or updates a mapped RF sensor.
func (c *Config) UpsertSensor(sensor RFSensor) {
	c.mu.Lock()
	defer c.mu.Unlock()

	found := false
	for i, s := range c.Sensors {
		if s.Code == sensor.Code {
			c.Sensors[i].Name = sensor.Name
			c.Sensors[i].AccessoryType = sensor.AccessoryType
			c.Sensors[i].Room = sensor.Room
			found = true
			break
		}
	}
	if !found {
		c.Sensors = append(c.Sensors, sensor)
	}
}

// DeleteSensor removes a mapped RF sensor by code.
func (c *Config) DeleteSensor(code uint32) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	for i, s := range c.Sensors {
		if s.Code == code {
			c.Sensors = append(c.Sensors[:i], c.Sensors[i+1:]...)
			return true
		}
	}
	return false
}


// UpsertButton adds or updates an InputButton configuration.
func (c *Config) UpsertButton(btn InputButton) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if btn.MinutesPerClick <= 0 {
		btn.MinutesPerClick = 15
	}
	if btn.MultiClickWindowMs <= 0 {
		btn.MultiClickWindowMs = 2500
	}

	found := false
	for i, b := range c.Buttons {
		if (btn.ID != "" && b.ID == btn.ID) || (btn.TriggerCode != 0 && b.TriggerCode == btn.TriggerCode) {
			c.Buttons[i] = btn
			found = true
			break
		}
	}
	if !found {
		if btn.ID == "" {
			btn.ID = fmt.Sprintf("btn-%d", time.Now().UnixNano())
		}
		c.Buttons = append(c.Buttons, btn)
	}
}

// DeleteButton removes an InputButton configuration by ID.
func (c *Config) DeleteButton(id string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	for i, b := range c.Buttons {
		if b.ID == id {
			c.Buttons = append(c.Buttons[:i], c.Buttons[i+1:]...)
			return true
		}
	}
	return false
}

// UpsertRule adds or updates an AutomationRule.
func (c *Config) UpsertRule(rule AutomationRule) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if rule.CooldownSec <= 0 {
		rule.CooldownSec = 30
	}

	found := false
	for i, r := range c.Rules {
		if (rule.ID != "" && r.ID == rule.ID) || (rule.TriggerCode != 0 && r.TriggerCode == rule.TriggerCode) {
			c.Rules[i] = rule
			found = true
			break
		}
	}
	if !found {
		if rule.ID == "" {
			rule.ID = fmt.Sprintf("rule-%d", time.Now().UnixNano())
		}
		c.Rules = append(c.Rules, rule)
	}
}

// DeleteRule removes an AutomationRule by ID.
func (c *Config) DeleteRule(id string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	for i, r := range c.Rules {
		if r.ID == id {
			c.Rules = append(c.Rules[:i], c.Rules[i+1:]...)
			return true
		}
	}
	return false
}

