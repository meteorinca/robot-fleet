package pinger

import (
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

// Pinger periodically checks network accessibility of fleet devices using TCP/HTTP connection probes with fallback IP support.
type Pinger struct {
	cfg  *config.Config
	stop chan struct{}
	mu   sync.Mutex
}

// NewPinger creates a new device ping service.
func NewPinger(cfg *config.Config) *Pinger {
	return &Pinger{
		cfg:  cfg,
		stop: make(chan struct{}),
	}
}

// Start launches the background health check ticker.
func (p *Pinger) Start() {
	go func() {
		// Run immediate initial ping sweep
		p.PingAll()

		ticker := time.NewTicker(15 * time.Second)
		defer ticker.Stop()

		for {
			select {
			case <-p.stop:
				return
			case <-ticker.C:
				p.PingAll()
			}
		}
	}()
}

// PingAll iterates through configured bots and tests connectivity with fallback IP support.
func (p *Pinger) PingAll() {
	bots := p.cfg.Bots
	var wg sync.WaitGroup

	for _, bot := range bots {
		wg.Add(1)
		go func(b config.RobotNode) {
			defer wg.Done()
			latency, online := p.PingDevice(b)
			
			status := "offline"
			if online {
				status = "online"
			}

			p.cfg.UpsertBot(config.RobotNode{
				ID:        b.ID,
				Name:      b.Name,
				Hostname:  b.Hostname,
				Platform:  b.Platform,
				Status:    status,
				LatencyMs: latency,
			})
		}(bot)
	}

	wg.Wait()
}

// PingDevice attempts to connect to a bot using its hostname first, and falls back to fallback_ip if hostname fails or times out.
func (p *Pinger) PingDevice(bot config.RobotNode) (float64, bool) {
	port := bot.Port
	if port == 0 {
		port = 80
	}

	targets := []string{}
	if bot.Hostname != "" {
		targets = append(targets, fmt.Sprintf("%s:%d", strings.TrimSuffix(bot.Hostname, ".local") + ".local", port))
	} else if bot.IP != "" {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.IP, port))
	}

	if bot.FallbackIP != "" && bot.FallbackIP != bot.Hostname && bot.FallbackIP != bot.IP {
		targets = append(targets, fmt.Sprintf("%s:%d", bot.FallbackIP, port))
	}

	// Try each target in sequence (hostname primary, fallback_ip secondary)
	for _, target := range targets {
		start := time.Now()
		conn, err := net.DialTimeout("tcp", target, 1500*time.Millisecond)
		if err == nil {
			latency := float64(time.Since(start).Microseconds()) / 1000.0 // ms
			_ = conn.Close()
			return latency, true
		}
	}

	// In local simulation mode, if host is localhost or 127.0.0.1, simulate responsive ping
	if strings.Contains(bot.IP, "127.0.0.1") || strings.Contains(bot.Hostname, "localhost") {
		return 0.8, true
	}

	return 0, false
}

// Stop halts the pinger service.
func (p *Pinger) Stop() {
	close(p.stop)
}
