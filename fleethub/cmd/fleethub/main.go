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

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/homekit"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/listener"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/mdns"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/pinger"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rfpoller"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/rules"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/webui"
	"github.com/meteorinca/robot-fleet/fleethub/web"
)

func main() {
	httpPort := flag.Int("port", 8080, "HTTP WebUI and API port")
	udpPort := flag.Int("udp", 4330, "UDP RF mesh listener port")
	configPath := flag.String("config", "fleethub_config.json", "Path to FleetHub JSON configuration file")
	homekitPin := flag.String("pin", "11122333", "HomeKit setup PIN (8 digits)")
	enableHomeKit := flag.Bool("homekit", runtime.GOOS != "windows", "Enable Apple HomeKit HAP Bridge (default true on Linux/macOS)")
	flag.Parse()

	log.Printf("[FleetHub] Starting Mothership Daemon v1.0.0 (%s/%s)...", runtime.GOOS, runtime.GOARCH)

	// 1. Load Configuration
	cfg, err := config.LoadConfig(*configPath)
	if err != nil {
		log.Fatalf("[FleetHub] Config error: %v", err)
	}
	if *httpPort != 8080 {
		cfg.HTTPPort = *httpPort
	}
	if *udpPort != 4330 {
		cfg.UDPPort = *udpPort
	}
	if *homekitPin != "" {
		cfg.HomeKitPIN = *homekitPin
	}

	// 2. Initialize Web Assets & Server
	assets, err := web.Assets()
	if err != nil {
		log.Printf("[FleetHub] Warning: Failed to load embedded assets: %v", err)
	}

	var webServer *webui.Server
	var hkBridge *homekit.BridgeManager

	// 3. Initialize Rule Engine
	ruleEngine := rules.NewEngine(cfg, func(event rules.EventPayload, executed []string) {
		if webServer != nil {
			webServer.BroadcastEvent(event, executed)
		}
		if hkBridge != nil {
			hkBridge.HandleRFEvent(event)
		}
	})

	// 4. Initialize & Start Active RF Poller
	rfPoller := rfpoller.NewRFPoller(cfg, ruleEngine)
	rfPoller.Start()
	log.Printf("[FleetHub] Active HTTP RF Poller active (Polling RFBot receiver nodes every 800ms)")

	webServer = webui.NewServer(cfg, ruleEngine, rfPoller, assets)

	// 5. Start UDP Listener
	udpListener := listener.NewUDPListener(cfg, cfg.UDPPort, ruleEngine)
	if err := udpListener.Start(); err != nil {
		log.Printf("[FleetHub] Warning: UDP listener bind failed: %v", err)
	} else {
		log.Printf("[FleetHub] Sub-50ms UDP RF Ingest listener active on port %d", cfg.UDPPort)
	}

	// 6. Start mDNS Fleet Scanner & Health Pinger
	scanner := mdns.NewScanner(cfg)
	scanner.Start()
	log.Printf("[FleetHub] Multi-Bot mDNS Scanner active (rfbot, speakerbot, dogbot_v1, simplebot)")

	devicePinger := pinger.NewPinger(cfg)
	devicePinger.Start()
	log.Printf("[FleetHub] Device Health Pinger active (Probing with IP fallback)")

	// 7. Start HomeKit HAP Server if enabled
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

	// 8. Start HTTP Server
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

	// 9. Graceful Shutdown Listener
	stopSig := make(chan os.Signal, 1)
	signal.Notify(stopSig, syscall.SIGINT, syscall.SIGTERM)
	<-stopSig

	log.Printf("[FleetHub] Shutting down Mothership Daemon cleanly...")
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
