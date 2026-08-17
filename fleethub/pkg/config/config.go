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
	Actions     []RuleAction `json:"actions"`
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
	filePath   string
	mu         sync.RWMutex
}

// DefaultConfig returns reasonable defaults.
func DefaultConfig(path string) *Config {
	return &Config{
		HTTPPort:   8080,
		UDPPort:    4330,
		HomeKitPIN: "11122333",
		Mode:       "home_automation",
		Bots: []RobotNode{
			{
				ID:              "rfbot1",
				Name:            "RFBot 1 (RX Gateway)",
				Hostname:        "rfbot1.local",
				FallbackIP:      "192.168.1.101",
				Platform:        PlatformRFBot,
				IP:              "rfbot1.local",
				Port:            4330,
				Status:          "online",
				Role:            "receiver",
				HomeKitEnabled:  true,
				HomeKitCategory: "sensor",
				PingEnabled:     true,
			},
			{
				ID:              "speakerbot1",
				Name:            "SpeakerBot 1 (Audio Chime)",
				Hostname:        "speakerbot1.local",
				FallbackIP:      "192.168.1.102",
				Platform:        PlatformSpeakerBot,
				IP:              "speakerbot1.local",
				Port:            80,
				Status:          "online",
				HomeKitEnabled:  true,
				HomeKitCategory: "speaker",
				PingEnabled:     true,
				Actions: []DeviceAction{
					{Name: "Play Chime Sound", Endpoint: "/choola", Method: "GET"},
				},
			},
			{
				ID:              "rfbot6",
				Name:            "RFBot 6 (TX Gateway)",
				Hostname:        "rfbot6.local",
				FallbackIP:      "192.168.1.106",
				Platform:        PlatformRFBot,
				IP:              "rfbot6.local",
				Port:            4330,
				Status:          "online",
				Role:            "transceiver",
				HomeKitEnabled:  false,
				HomeKitCategory: "switch",
				PingEnabled:     true,
				Actions: []DeviceAction{
					{Name: "Send RF Code (123456)", Endpoint: "/rf/send?code=123456&bits=24&proto=1&pulse=185", Method: "GET"},
				},
			},
			{
				ID:              "simplebot1",
				Name:            "SimpleBot 1 (Kitchen Light Controller)",
				Hostname:        "simplebot1.local",
				FallbackIP:      "192.168.1.105",
				Platform:        PlatformSimpleBot,
				IP:              "simplebot1.local",
				Port:            80,
				Status:          "online",
				Role:            "kitchen_bot",
				HomeKitEnabled:  true,
				HomeKitCategory: "switch",
				PingEnabled:     true,
				Actions: []DeviceAction{
					{Name: "Servo 1 ON", Endpoint: "/s1on", Method: "GET"},
					{Name: "Servo 1 OFF", Endpoint: "/s1off", Method: "GET"},
					{Name: "Servo 2 ON", Endpoint: "/s2on", Method: "GET"},
					{Name: "Servo 2 OFF", Endpoint: "/s2off", Method: "GET"},
				},
			},
		},
		Sensors: []RFSensor{
			{
				Code:          123456,
				Name:          "Hallway Photodetector",
				AccessoryType: "light",
				Room:          "Living Room",
				LastTriggered: time.Now(),
			},
			{
				Code:          122222,
				Name:          "Front Door Sensor",
				AccessoryType: "contact",
				Room:          "Entrance",
				LastTriggered: time.Now(),
			},
		},
		Rules: []AutomationRule{
			{
				ID:          "rule-1",
				Name:        "Photodetector Chime Trigger",
				Enabled:     true,
				TriggerCode: 123456,
				Actions: []RuleAction{
					{
						Type:   "speakerbot_play",
						Target: "speakerbot1.local",
						Path:   "/choola",
					},
				},
			},
		},
		filePath: path,
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
