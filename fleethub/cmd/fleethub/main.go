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
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/audio"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/db"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/environment"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/homekit"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/listener"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/mdns"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/pinger"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rfpoller"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/telemetry"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/webui"
	"github.com/meteorinca/robot-fleet/fleethub/web"
)

func main() {
	httpPort := flag.Int("port", 8126, "HTTP WebUI and API port")
	udpPort := flag.Int("udp", 4330, "UDP RF mesh listener port")
	configPath := flag.String("config", "fleethub_config.json", "Path to FleetHub JSON configuration file")
	homekitPin := flag.String("pin", "11122333", "HomeKit setup PIN (8 digits)")
	enableHomeKit := flag.Bool("homekit", runtime.GOOS != "windows", "Enable Apple HomeKit HAP Bridge (default true on Linux/macOS)")
	startupChime := flag.Bool("startup-chime", true, "Play audio confirmation chime on SpeakerBot at startup")
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

	// 2. Initialize Persistent SQLite WAL Database
	dbPath := filepath.Join(filepath.Dir(*configPath), "fleethub.db")
	database, err := db.Open(dbPath)
	if err != nil {
		log.Printf("[FleetHub] Warning: SQLite database initialization failed: %v", err)
	} else {
		log.Printf("[FleetHub] High-Performance SQLite WAL Database active (%s)", dbPath)
	}

	// 3. Initialize Background Telemetry & Speedtest Service
	var telemetrySvc *telemetry.Service
	if database != nil {
		telemetrySvc = telemetry.NewService(database, func(sr db.SpeedtestRecord) {
			log.Printf("[FleetHub Telemetry] Speedtest complete: %.1f Mbps down / %.1f Mbps up (Ping: %.1fms)", sr.DownloadMbps, sr.UploadMbps, sr.PingMs)
		})
		telemetrySvc.Start()
		log.Printf("[FleetHub] System Telemetry sampler active (20s interval)")
	}

	// 4. Initialize Web Assets & Server
	assets, err := web.Assets()
	if err != nil {
		log.Printf("[FleetHub] Warning: Failed to load embedded assets: %v", err)
	}

	var webServer *webui.Server
	var hkBridge *homekit.BridgeManager

	// Initialize Environmental & Solar Tracker
	envSvc := environment.NewService(0, 0)

	// 5. Initialize Rule Engine
	ruleEngine := rules.NewEngine(cfg, func(event rules.EventPayload, executed []string) {
		if webServer != nil {
			webServer.BroadcastEvent(event, executed)
		}
		if hkBridge != nil {
			hkBridge.HandleRFEvent(event)
		}
	})
	ruleEngine.SetEnvironment(envSvc)
	if database != nil {
		ruleEngine.SetDatabase(database)
	}

	// 6. Initialize & Start Active RF Poller
	rfPoller := rfpoller.NewRFPoller(cfg, ruleEngine)
	rfPoller.Start()
	log.Printf("[FleetHub] Active HTTP RF Poller active (Polling RFBot receiver nodes every 800ms)")

	webServer = webui.NewServer(cfg, ruleEngine, rfPoller, assets)
	webServer.SetEnvironment(envSvc)
	if database != nil {
		webServer.SetDatabase(database)
	}
	if telemetrySvc != nil {
		webServer.SetTelemetry(telemetrySvc)
	}

	// 7. Initialize Audio Subsystem & SpeakerBot Sound Studio
	soundsDir := filepath.Join(filepath.Dir(*configPath), "sounds")
	audioSvc, err := audio.NewService(soundsDir, func(st audio.StreamStatus) {
		if webServer != nil {
			webServer.BroadcastAudioStatus(st)
		}
	})
	if err != nil {
		log.Printf("[FleetHub] Warning: Audio subsystem initialization failed: %v", err)
	} else {
		ruleEngine.SetAudio(audioSvc)
		webServer.SetAudio(audioSvc)
	}

	// 8. Start UDP Listener
	udpListener := listener.NewUDPListener(cfg, cfg.UDPPort, ruleEngine)
	if err := udpListener.Start(); err != nil {
		log.Printf("[FleetHub] Warning: UDP listener bind failed: %v", err)
	} else {
		log.Printf("[FleetHub] Sub-50ms UDP RF Ingest listener active on port %d", cfg.UDPPort)
	}

	// 8. Start mDNS Fleet Scanner & Health Pinger
	scanner := mdns.NewScanner(cfg)
	webServer.SetScanner(scanner)
	scanner.Start()
	log.Printf("[FleetHub] Multi-Bot mDNS & LAN Scanner active (rfbot, speakerbot, wled, klipper, dogbot, simplebot)")

	devicePinger := pinger.NewPinger(cfg)
	if database != nil {
		devicePinger.SetDatabase(database)
	}
	webServer.SetPinger(devicePinger)
	devicePinger.Start()
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

	// 11. Startup Audio Confirmation
	if *startupChime {
		go func() {
			time.Sleep(800 * time.Millisecond)
			log.Printf("[FleetHub] Startup confirmation: Dispatching test /choola chime to SpeakerBot...")
			ruleEngine.ExecuteRuleAction(config.RuleAction{
				Type:   "speakerbot_play",
				Target: "speakerbot1",
				Path:   "/choola",
			})
		}()
	}

	// 12. Graceful Shutdown Listener
	stopSig := make(chan os.Signal, 1)
	signal.Notify(stopSig, syscall.SIGINT, syscall.SIGTERM)
	<-stopSig

	log.Printf("[FleetHub] Shutting down Mothership Daemon cleanly...")
	rfPoller.Stop()
	udpListener.Stop()
	scanner.Stop()
	devicePinger.Stop()
	if telemetrySvc != nil {
		telemetrySvc.Stop()
	}
	if database != nil {
		_ = database.Close()
	}
	if hkBridge != nil {
		hkBridge.Stop()
	}
	_ = cfg.SaveRuntimeState()
	log.Printf("[FleetHub] Stopped.")
}

