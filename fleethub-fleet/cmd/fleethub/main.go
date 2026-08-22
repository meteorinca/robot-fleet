package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"syscall"

	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/db"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/homekit"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/listener"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/mdns"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/pinger"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/rfpoller"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/rules"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/telemetry"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/webui"
	"github.com/meteorinca/robot-fleet/fleethub-fleet/web"
)

func main() {
	httpPort := flag.Int("port", 8126, "HTTP WebUI and API port")
	udpPort := flag.Int("udp", 4330, "UDP RF mesh listener port")
	configPath := flag.String("config", "fleethub_config.json", "Path to FleetHub JSON configuration file")
	dbPath := flag.String("db", "fleethub.db", "Path to FleetHub SQLite database file")
	homekitPin := flag.String("pin", "11122333", "HomeKit setup PIN (8 digits)")
	enableHomeKit := flag.Bool("homekit", runtime.GOOS != "windows", "Enable Apple HomeKit HAP Bridge (default true on Linux/macOS)")
	flag.Parse()

	log.Printf("[FleetHub] Starting Mothership Daemon v1.0.0 (%s/%s)...", runtime.GOOS, runtime.GOARCH)

	// 1. Load Configuration
	cfg, err := config.LoadConfig(*configPath)
	if err != nil {
		log.Fatalf("[FleetHub] Config error: %v", err)
	}

	portSet := false
	flag.CommandLine.Visit(func(f *flag.Flag) {
		if f.Name == "port" {
			portSet = true
		}
	})

	if portSet {
		cfg.HTTPPort = *httpPort
	} else if cfg.HTTPPort == 8080 {
		cfg.HTTPPort = 8126
	}

	if *udpPort != 4330 {
		cfg.UDPPort = *udpPort
	}
	if *homekitPin != "" {
		cfg.HomeKitPIN = *homekitPin
	}

	// 2. Initialize Embedded SQLite Database
	actualDBPath := *dbPath
	if !filepath.IsAbs(actualDBPath) {
		actualDBPath = filepath.Join(filepath.Dir(*configPath), actualDBPath)
	}
	database, err := db.Open(actualDBPath)
	if err != nil {
		log.Fatalf("[FleetHub] Database initialization failed: %v", err)
	}
	defer database.Close()

	// 3. Initialize Web Assets & Server
	assets, err := web.Assets()
	if err != nil {
		log.Printf("[FleetHub] Warning: Failed to load embedded assets: %v", err)
	}

	var webServer *webui.Server
	var hkBridge *homekit.BridgeManager

	// 4. Initialize Rule Engine & wire DB
	ruleEngine := rules.NewEngine(cfg, func(event rules.EventPayload, executed []string) {
		if webServer != nil {
			webServer.BroadcastEvent(event, executed)
		}
		if hkBridge != nil {
			hkBridge.HandleRFEvent(event)
		}
	})
	ruleEngine.SetDB(database)
	ruleEngine.SetCountCallback(func(code uint32, totalCount int64) {
		if webServer != nil {
			webServer.BroadcastCountUpdate(code, totalCount)
		}
	})

	// 5. Initialize & Start Active RF Poller
	rfPoller := rfpoller.NewRFPoller(cfg, ruleEngine)
	rfPoller.Start()
	log.Printf("[FleetHub] Active HTTP RF Poller active (Polling RFBot receiver nodes every 800ms)")

	webServer = webui.NewServer(cfg, ruleEngine, rfPoller, assets)
	webServer.SetDB(database)

	// 6. Initialize & Start Telemetry Service
	telemetryService := telemetry.NewService(database, func(rec db.SpeedtestRecord) {
		if webServer != nil {
			webServer.BroadcastSpeedtestDone(rec)
		}
	})
	telemetryService.Start()
	webServer.SetTelemetry(telemetryService)
	log.Printf("[FleetHub] System Telemetry & Metrics Service active")

	// 7. Start UDP Listener
	udpListener := listener.NewUDPListener(cfg, cfg.UDPPort, ruleEngine)
	if err := udpListener.Start(); err != nil {
		log.Printf("[FleetHub] Warning: UDP listener bind failed: %v", err)
	} else {
		log.Printf("[FleetHub] Sub-50ms UDP RF Ingest listener active on port %d", cfg.UDPPort)
	}

	// 8. Start mDNS Fleet Scanner & Health Pinger
	scanner := mdns.NewScanner(cfg)
	scanner.Start()
	webServer.SetScanner(scanner)
	log.Printf("[FleetHub] Multi-Bot mDNS Scanner active (rfbot, speakerbot, dogbot_v1, simplebot)")

	devicePinger := pinger.NewPinger(cfg)
	devicePinger.SetCallback(func(deviceID string, latencyMs float64, status, ip string) {
		telemetryService.RecordPing(deviceID, latencyMs, status, ip)
	})
	devicePinger.Start()
	webServer.SetPinger(devicePinger)
	log.Printf("[FleetHub] Device Health Pinger active (Probing with IP fallback)")

	// 9. Start HomeKit HAP Server if enabled
	if *enableHomeKit {
		dataDir := filepath.Join(filepath.Dir(*configPath), ".fleethub_hk")
		_ = os.MkdirAll(dataDir, 0755)

		hkBridge, err = homekit.NewBridgeManager(cfg, dataDir)
		if err != nil {
			log.Printf("[FleetHub] Notice: HomeKit server deferred: %v", err)
		} else {
			hkBridge.Start()
			log.Printf("[FleetHub] Apple HomeKit HAP Bridge active (Setup PIN: %s)", cfg.HomeKitPIN)
		}
	} else {
		log.Printf("[FleetHub] HomeKit Bridge disabled (run with -homekit to enable on Linux/macOS)")
	}

	// 10. Start HTTP Server
	serverAddr := fmt.Sprintf(":%d", cfg.HTTPPort)
	server := &http.Server{
		Addr:    serverAddr,
		Handler: webServer.Handler(),
	}

	go func() {
		log.Printf("[FleetHub] WebUI & REST API running at http://localhost:%d", cfg.HTTPPort)
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("[FleetHub] HTTP server error: %v", err)
		}
	}()

	// 11. Graceful Shutdown Listener
	stopSig := make(chan os.Signal, 1)
	signal.Notify(stopSig, syscall.SIGINT, syscall.SIGTERM)
	<-stopSig

	log.Printf("[FleetHub] Shutting down Mothership Daemon cleanly...")
	telemetryService.Stop()
	rfPoller.Stop()
	udpListener.Stop()
	scanner.Stop()
	devicePinger.Stop()
	if hkBridge != nil {
		hkBridge.Stop()
	}
	_ = cfg.Save()
	log.Printf("[FleetHub] Stopped.")
}
