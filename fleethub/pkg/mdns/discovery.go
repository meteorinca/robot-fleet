package mdns

import (
	"context"
	"strings"
	"time"

	"github.com/grandcat/zeroconf"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

// Scanner continuously discovers robot-fleet devices via mDNS.
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
		s.scanOnce()

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
		{"_http._tcp", config.PlatformRFBot},
	}

	for _, svc := range services {
		entries := make(chan *zeroconf.ServiceEntry)
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)

		go func(plt config.BotPlatform) {
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
				if strings.Contains(lowerName, "cambot") || strings.Contains(lowerHost, "cambot") || strings.Contains(lowerName, "cam") {
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
				}

				s.cfg.UpsertBot(node)
			}
		}(svc.platform)

		_ = resolver.Browse(ctx, svc.service, "local.", entries)
		<-ctx.Done()
		cancel()
	}
}

// Stop stops the scanner.
func (s *Scanner) Stop() {
	close(s.stop)
}
