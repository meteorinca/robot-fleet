package homekit

import (
	"fmt"
	"log"

	"github.com/brutella/hc"
	"github.com/brutella/hc/accessory"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
)

// BridgeManager manages the Apple HomeKit bridge server.
type BridgeManager struct {
	cfg      *config.Config
	tport    hc.Transport
	switches map[uint32]*accessory.Switch
}

// NewBridgeManager initializes a new HomeKit bridge instance.
func NewBridgeManager(cfg *config.Config, dbPath string) (*BridgeManager, error) {
	bridgeInfo := accessory.Info{
		Name:         "FleetHub Bridge",
		Manufacturer: "MeteorInca Robot Fleet",
		Model:        "FleetHub-Go",
		SerialNumber: "FH-433-001",
	}

	bridgeAcc := accessory.New(bridgeInfo, accessory.TypeBridge)

	// Create transport config
	hcConfig := hc.Config{
		Pin:         cfg.HomeKitPIN,
		StoragePath: dbPath,
		Port:        "0", // auto choose free port
	}

	var accessories []*accessory.Accessory
	switchMap := make(map[uint32]*accessory.Switch)

	for _, s := range cfg.Sensors {
		info := accessory.Info{
			Name:         s.Name,
			Manufacturer: "Robot Fleet Mesh",
			Model:        fmt.Sprintf("RF-Sensor-0x%X", s.Code),
			SerialNumber: fmt.Sprintf("RF-%d", s.Code),
		}

		sw := accessory.NewSwitch(info)
		accessories = append(accessories, sw.Accessory)
		switchMap[s.Code] = sw
	}

	// Add HomeKit accessories for devices with homekit_enabled == true
	for _, b := range cfg.Bots {
		if !b.HomeKitEnabled {
			continue
		}
		info := accessory.Info{
			Name:         b.Name,
			Manufacturer: "Robot Fleet",
			Model:        string(b.Platform),
			SerialNumber: fmt.Sprintf("BOT-%s", b.ID),
		}
		sw := accessory.NewSwitch(info)
		accessories = append(accessories, sw.Accessory)
	}

	tport, err := hc.NewIPTransport(hcConfig, bridgeAcc, accessories...)
	if err != nil {
		return nil, fmt.Errorf("failed to create HomeKit IP transport: %w", err)
	}

	bm := &BridgeManager{
		cfg:      cfg,
		tport:    tport,
		switches: switchMap,
	}

	return bm, nil
}

// Start launches the HomeKit transport server safely in the background.
func (bm *BridgeManager) Start() {
	go func() {
		defer func() {
			if r := recover(); r != nil {
				log.Printf("[HomeKit] Notice: HomeKit mDNS responder deferred on local interface: %v", r)
			}
		}()
		bm.tport.Start()
	}()
}

// Stop stops the HomeKit transport.
func (bm *BridgeManager) Stop() {
	if bm.tport != nil {
		bm.tport.Stop()
	}
}

// HandleRFEvent updates corresponding HomeKit accessory states when an RF code is decoded.
func (bm *BridgeManager) HandleRFEvent(event rules.EventPayload) {
	if sw, exists := bm.switches[event.Code]; exists {
		// Toggle state on incoming pulse
		current := sw.Switch.On.GetValue()
		sw.Switch.On.SetValue(!current)
	}
}
