package mdns

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"strings"
	"time"

	"github.com/grandcat/zeroconf"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

// Scanner continuously discovers robot-fleet devices via mDNS and local network probing.
type Scanner struct {
	cfg  *config.Config
	stop chan struct{}
}

// NewScanner creates a new mDNS scanner instance.
func NewScanner(cfg *config.Config) *Scanner {
	return &Scanner{
		cfg:  cfg,
		stop: make(chan struct{}),
	}
}

// Start begins periodic mDNS service discovery in the background.
func (s *Scanner) Start() {
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()

		// Run immediate initial scan
		s.ScanOnce()

		for {
			select {
			case <-s.stop:
				return
			case <-ticker.C:
				s.scanOnce()
			}
		}
	}()
}

// ScanOnce triggers an immediate synchronous discovery scan and LAN candidate probe.
func (s *Scanner) ScanOnce() {
	s.scanOnce()
	s.probeLANCandidates()
	_ = s.cfg.Save()
	_ = s.cfg.SaveRuntimeState()
}

func (s *Scanner) scanOnce() {
	resolver, err := zeroconf.NewResolver(nil)
	if err != nil {
		return
	}

	services := []struct {
		service  string
		platform config.BotPlatform
	}{
		{"_rfbot._tcp", config.PlatformRFBot},
		{"_speakerbot._tcp", config.PlatformSpeakerBot},
		{"_dogbot._tcp", config.PlatformDogBot},
		{"_simplebot._tcp", config.PlatformSimpleBot},
		{"_carbot._tcp", config.PlatformCarBot},
		{"_cambot._tcp", config.PlatformCamBot},
		{"_wled._tcp", config.PlatformWLED},
		{"_moonraker._tcp", config.PlatformKlipper},
		{"_octoprint._tcp", config.PlatformKlipper},
		{"_http._tcp", ""},
	}

	for _, svc := range services {
		entries := make(chan *zeroconf.ServiceEntry)
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)

		go func(plt config.BotPlatform, svcName string) {
			for entry := range entries {
				ip := ""
				if len(entry.AddrIPv4) > 0 {
					ip = entry.AddrIPv4[0].String()
				}
				if ip == "" {
					continue
				}

				name := entry.Instance
				if name == "" {
					name = entry.ServiceInstanceName()
				}

				// Check if the service name identifies a known bot
				platform := plt
				lowerName := strings.ToLower(name)
				lowerHost := strings.ToLower(entry.HostName)

				if strings.Contains(lowerName, "wled") || strings.Contains(lowerHost, "wled") || svcName == "_wled._tcp" {
					platform = config.PlatformWLED
				} else if strings.Contains(lowerName, "speeder") || strings.Contains(lowerHost, "speeder") ||
					strings.Contains(lowerName, "mainsail") || strings.Contains(lowerHost, "mainsail") ||
					strings.Contains(lowerName, "klipper") || strings.Contains(lowerHost, "klipper") ||
					strings.Contains(lowerName, "moonraker") || strings.Contains(lowerHost, "moonraker") ||
					strings.Contains(lowerName, "fluidd") || strings.Contains(lowerHost, "fluidd") ||
					svcName == "_moonraker._tcp" || svcName == "_octoprint._tcp" {
					platform = config.PlatformKlipper
				} else if strings.Contains(lowerName, "cambot") || strings.Contains(lowerHost, "cambot") || strings.Contains(lowerName, "cam") {
					platform = config.PlatformCamBot
				} else if strings.Contains(lowerName, "speakerbot") {
					platform = config.PlatformSpeakerBot
				} else if strings.Contains(lowerName, "dogbot") {
					platform = config.PlatformDogBot
				} else if strings.Contains(lowerName, "simplebot") {
					platform = config.PlatformSimpleBot
				} else if strings.Contains(lowerName, "carbot") {
					platform = config.PlatformCarBot
				} else if strings.Contains(lowerName, "rfbot") {
					platform = config.PlatformRFBot
				} else if platform == "" {
					// Don't register unknown generic HTTP services automatically
					continue
				}

				hostname := entry.HostName
				if hostname != "" {
					hostname = strings.TrimSuffix(hostname, ".")
				} else {
					hostname = strings.ToLower(name) + ".local"
				}

				node := config.RobotNode{
					ID:       entry.Instance,
					Name:     name,
					Hostname: hostname,
					Platform: platform,
					IP:       ip,
					Port:     entry.Port,
					Status:   "online",
				}

				if platform == config.PlatformCamBot {
					node.Role = "camera"
					node.Actions = []config.DeviceAction{
						{Name: "Start Stream", Endpoint: "/cam_on", Method: "GET"},
						{Name: "Stop Stream", Endpoint: "/cam_off", Method: "GET"},
						{Name: "Snapshot", Endpoint: "/snapshot", Method: "GET"},
						{Name: "Toggle Flash", Endpoint: "/toggle", Method: "GET"},
					}
				} else if platform == config.PlatformWLED {
					node.Role = "lighting"
					node.Actions = []config.DeviceAction{
						{Name: "Toggle On/Off", Endpoint: "/win&T=2", Method: "GET"},
						{Name: "Solid Pink", Endpoint: "/json/state", Method: "POST", Payload: `{"on":true,"bri":255,"seg":[{"col":[[255,105,180]]}]}`},
						{Name: "Solid Cyan", Endpoint: "/json/state", Method: "POST", Payload: `{"on":true,"bri":255,"seg":[{"col":[[0,212,255]]}]}`},
					}
				} else if platform == config.PlatformKlipper {
					node.Role = "3d_printer"
					node.Actions = []config.DeviceAction{
						{Name: "Printer Info", Endpoint: "/printer/info", Method: "GET"},
						{Name: "Pause Print", Endpoint: "/printer/print/pause", Method: "POST"},
						{Name: "Resume Print", Endpoint: "/printer/print/resume", Method: "POST"},
						{Name: "Emergency Stop", Endpoint: "/printer/emergency_stop", Method: "POST"},
					}
				}

				s.cfg.UpsertBot(node)
			}
		}(svc.platform, svc.service)

		_ = resolver.Browse(ctx, svc.service, "local.", entries)
		<-ctx.Done()
		cancel()
	}
}

// probeLANCandidates resolves and checks common LAN hostnames for WLED and Klipper/Speeder Pad devices.
func (s *Scanner) probeLANCandidates() {
	client := &http.Client{Timeout: 1 * time.Second}
	candidates := []struct {
		id       string
		host     string
		port     int
		platform config.BotPlatform
		name     string
		role     string
	}{
		{"wled", "wled.local", 80, config.PlatformWLED, "WLED Ambient Lighting", "lighting"},
		{"speeder-pad-moonraker", "speeder-pad.local", 7125, config.PlatformKlipper, "Speeder Pad (Klipper)", "3d_printer"},
		{"speeder-pad-mainsail", "speeder-pad.local", 80, config.PlatformKlipper, "Speeder Pad (Mainsail)", "3d_printer"},
		{"mainsail", "mainsail.local", 80, config.PlatformKlipper, "Mainsail 3D Printer", "3d_printer"},
		{"klipper", "klipper.local", 7125, config.PlatformKlipper, "Klipper Moonraker Host", "3d_printer"},
	}

	for _, c := range candidates {
		ip := ""
		ips, err := net.LookupIP(c.host)
		if err == nil && len(ips) > 0 {
			ip = ips[0].String()
		}

		// Try quick HTTP probe if DNS didn't resolve
		if ip == "" {
			url := fmt.Sprintf("http://%s:%d/", c.host, c.port)
			resp, err := client.Get(url)
			if err != nil {
				continue
			}
			resp.Body.Close()
		}

		node := config.RobotNode{
			ID:       c.id,
			Name:     c.name,
			Hostname: c.host,
			Platform: c.platform,
			IP:       ip,
			Port:     c.port,
			Status:   "online",
			Role:     c.role,
		}

		if c.platform == config.PlatformWLED {
			node.Actions = []config.DeviceAction{
				{Name: "Toggle On/Off", Endpoint: "/win&T=2", Method: "GET"},
				{Name: "Solid Pink", Endpoint: "/json/state", Method: "POST", Payload: `{"on":true,"bri":255,"seg":[{"col":[[255,105,180]]}]}`},
				{Name: "Solid Cyan", Endpoint: "/json/state", Method: "POST", Payload: `{"on":true,"bri":255,"seg":[{"col":[[0,212,255]]}]}`},
			}
		} else if c.platform == config.PlatformKlipper {
			node.Actions = []config.DeviceAction{
				{Name: "Printer Info", Endpoint: "/printer/info", Method: "GET"},
				{Name: "Pause Print", Endpoint: "/printer/print/pause", Method: "POST"},
				{Name: "Resume Print", Endpoint: "/printer/print/resume", Method: "POST"},
				{Name: "Emergency Stop", Endpoint: "/printer/emergency_stop", Method: "POST"},
			}
		}

		s.cfg.UpsertBot(node)
	}
}

// Stop stops the scanner.
func (s *Scanner) Stop() {
	close(s.stop)
}
