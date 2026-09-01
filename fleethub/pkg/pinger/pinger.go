package pinger

import (
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/db"
)

// Pinger periodically checks network accessibility of fleet devices using TCP/HTTP connection probes with fallback IP support.
type Pinger struct {
	cfg      *config.Config
	database *db.DB
	stop     chan struct{}
	mu       sync.Mutex
}

// NewPinger creates a new device ping service.
func NewPinger(cfg *config.Config) *Pinger {
	return &Pinger{
		cfg:  cfg,
		stop: make(chan struct{}),
	}
}

// SetDatabase connects the persistent SQLite database for ping latency history.
func (p *Pinger) SetDatabase(d *db.DB) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.database = d
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

func isValidIP(ip string) bool {
	return net.ParseIP(strings.TrimSpace(ip)) != nil
}

// PingResult represents an immediate probe result for a bot.
type PingResult struct {
	ID        string  `json:"id"`
	Name      string  `json:"name"`
	LatencyMs float64 `json:"latency_ms"`
	Status    string  `json:"status"`
	IP        string  `json:"ip"`
}

// PingAllSync executes a synchronous ping sweep across all fleet devices and returns the results.
func (p *Pinger) PingAllSync() []PingResult {
	bots := p.cfg.Bots
	var wg sync.WaitGroup
	var mu sync.Mutex
	var results []PingResult

	for _, bot := range bots {
		wg.Add(1)
		go func(b config.RobotNode) {
			defer wg.Done()
			latency, online, discoveredIP := p.PingDevice(b)

			status := "offline"
			if online {
				status = "online"
			}

			ipToRecord := discoveredIP
			if ipToRecord == "" {
				ipToRecord = b.IP
			}
			if ipToRecord == "" {
				ipToRecord = b.FallbackIP
			}

			if p.database != nil {
				_ = p.database.RecordPing(b.ID, latency, status, ipToRecord)
			}

			updatedBot := config.RobotNode{
				ID:        b.ID,
				Name:      b.Name,
				Hostname:  b.Hostname,
				Platform:  b.Platform,
				Status:    status,
				LatencyMs: latency,
			}
			if discoveredIP != "" && isValidIP(discoveredIP) {
				updatedBot.IP = discoveredIP
			}
			p.cfg.UpsertBot(updatedBot)

			mu.Lock()
			results = append(results, PingResult{
				ID:        b.ID,
				Name:      b.Name,
				LatencyMs: latency,
				Status:    status,
				IP:        ipToRecord,
			})
			mu.Unlock()
		}(bot)
	}

	wg.Wait()
	return results
}

// PingAll iterates through configured bots and tests connectivity with fallback IP support.
func (p *Pinger) PingAll() {
	_ = p.PingAllSync()
}

// PingDevice attempts to connect to a bot using its hostname first, and falls back to fallback_ip if hostname fails or times out.
func (p *Pinger) PingDevice(bot config.RobotNode) (float64, bool, string) {
	port := bot.Port
	if port == 0 {
		port = 80
	}

	// Try resolving hostname to IPv4 address via DNS/mDNS lookup
	var resolvedIP string
	if bot.Hostname != "" {
		cleanHost := strings.TrimSuffix(bot.Hostname, ".local")
		if ips, err := net.LookupHost(cleanHost + ".local"); err == nil && len(ips) > 0 {
			resolvedIP = ips[0]
		} else if ips, err := net.LookupHost(cleanHost); err == nil && len(ips) > 0 {
			resolvedIP = ips[0]
		}
	}

	type targetItem struct {
		addr string
		ip   string
	}

	targets := []targetItem{}

	if resolvedIP != "" && isValidIP(resolvedIP) {
		targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", resolvedIP, port), ip: resolvedIP})
	}
	if isValidIP(bot.IP) {
		targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.IP, port), ip: bot.IP})
	}
	if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP && bot.FallbackIP != resolvedIP {
		targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.FallbackIP, port), ip: bot.FallbackIP})
	}
	if bot.Hostname != "" {
		hostAddr := fmt.Sprintf("%s:%d", strings.TrimSuffix(bot.Hostname, ".local")+".local", port)
		targets = append(targets, targetItem{addr: hostAddr, ip: resolvedIP})
	}

	// Try each target in sequence
	for _, target := range targets {
		start := time.Now()
		conn, err := net.DialTimeout("tcp", target.addr, 1200*time.Millisecond)
		if err == nil {
			latency := float64(time.Since(start).Microseconds()) / 1000.0 // ms
			_ = conn.Close()
			return latency, true, target.ip
		}
	}

	// In local simulation mode, if host is localhost or 127.0.0.1, simulate responsive ping
	if strings.Contains(bot.IP, "127.0.0.1") || strings.Contains(bot.Hostname, "localhost") {
		return 0.8, true, "127.0.0.1"
	}

	return 0, false, resolvedIP
}

// Stop halts the pinger service.
func (p *Pinger) Stop() {
	close(p.stop)
}

