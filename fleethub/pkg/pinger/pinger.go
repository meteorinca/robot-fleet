package pinger

import (
	"fmt"
	"log"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/db"
	"github.com/meteorinca/robot-fleet/fleethub/pkg/mdns"
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

// Start launches the background health check ticker and hourly IP resolver.
func (p *Pinger) Start() {
	go func() {
		// Run immediate initial mDNS resolution to heal stale IPs on startup
		p.ResolveAndUpdateIPs()

		// Run immediate initial ping sweep
		p.PingAll()

		// User requested: ping interval every 5 minutes (reduced from 15s to save CPU and SD writes)
		pingTicker := time.NewTicker(5 * time.Minute)
		defer pingTicker.Stop()

		// Hourly ticker re-resolves all mDNS hostnames to fresh IPs, healing stale DHCP entries
		resolveTicker := time.NewTicker(1 * time.Hour)
		defer resolveTicker.Stop()

		for {
			select {
			case <-p.stop:
				_ = p.cfg.SaveRuntimeState()
				return
			case <-pingTicker.C:
				p.PingAll()
			case <-resolveTicker.C:
				p.ResolveAndUpdateIPs()
			}
		}
	}()
}

// ResolveAndUpdateIPs re-resolves all bot hostnames via direct pure-Go mDNS and updates the stored IP
// if the resolved address differs. This heals stale IPs after DHCP lease changes without
// requiring a restart, working reliably on Raspberry Pi without Avahi.
func (p *Pinger) ResolveAndUpdateIPs() {
	bots := p.cfg.Bots
	var hostnames []string
	for _, bot := range bots {
		if bot.Hostname != "" {
			hostnames = append(hostnames, bot.Hostname)
		}
	}
	if len(hostnames) == 0 {
		return
	}

	resolvedMap := mdns.ResolveAll(hostnames, 800*time.Millisecond)
	updated := false

	for _, bot := range bots {
		if bot.Hostname == "" {
			continue
		}
		cleanHost := mdns.CleanHostname(bot.Hostname)
		resolvedIP, found := resolvedMap[cleanHost]
		if !found {
			resolvedIP, found = resolvedMap[bot.Hostname]
		}
		if !found || resolvedIP == "" || !config.IsValidIP(resolvedIP) {
			continue
		}
		if resolvedIP != bot.IP {
			log.Printf("[Pinger] IP change detected for %s: %s -> %s (mDNS resolved)", bot.Hostname, bot.IP, resolvedIP)
			p.cfg.UpsertBot(config.RobotNode{
				ID:       bot.ID,
				Name:     bot.Name,
				Hostname: bot.Hostname,
				Platform: bot.Platform,
				IP:       resolvedIP,
				Status:   "online",
			})
			updated = true
		}
	}
	if updated {
		_ = p.cfg.SaveRuntimeState()
	}
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
	stateChanged := false

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
			// If the pinger reached the bot via mDNS and discovered a fresh IP, persist it
			ipChanged := false
			if discoveredIP != "" && isValidIP(discoveredIP) && discoveredIP != b.IP {
				log.Printf("[Pinger] Updating stored IP for %s: %s -> %s (live ping)", b.Hostname, b.IP, discoveredIP)
				updatedBot.IP = discoveredIP
				ipChanged = true
			} else if isValidIP(b.IP) {
				updatedBot.IP = b.IP
			}
			p.cfg.UpsertBot(updatedBot)

			mu.Lock()
			if b.Status != status || ipChanged {
				stateChanged = true
			}
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
	if stateChanged {
		_ = p.cfg.SaveRuntimeState()
	}
	return results
}

// PingAll iterates through configured bots and tests connectivity with fallback IP support.
func (p *Pinger) PingAll() {
	_ = p.PingAllSync()
}

// PingDevice attempts to connect to a bot using mDNS hostname first (when PreferMDNS is set),
// then falls back to stored IP and fallback_ip.
func (p *Pinger) PingDevice(bot config.RobotNode) (float64, bool, string) {
	port := bot.Port
	if port == 0 {
		port = 80
	}

	type targetItem struct {
		addr string
		ip   string
	}

	targets := []targetItem{}
	var resolvedIP string

	if p.cfg.PreferMDNS {
		// Try resolving hostname to IPv4 address via pure-Go mDNS lookup
		if bot.Hostname != "" {
			resolvedIP, _ = mdns.ResolveHostname(bot.Hostname, 500*time.Millisecond)
		}
		if resolvedIP != "" && isValidIP(resolvedIP) {
			targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", resolvedIP, port), ip: resolvedIP})
		}
		if isValidIP(bot.IP) && bot.IP != resolvedIP {
			targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.IP, port), ip: bot.IP})
		}
		if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP && bot.FallbackIP != resolvedIP {
			targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.FallbackIP, port), ip: bot.FallbackIP})
		}
		if bot.Hostname != "" {
			hostAddr := fmt.Sprintf("%s:%d", mdns.CleanHostname(bot.Hostname), port)
			targets = append(targets, targetItem{addr: hostAddr, ip: resolvedIP})
		}
	} else {
		// IP-first: test known IP directly without waiting for mDNS network multicast
		if isValidIP(bot.IP) {
			targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.IP, port), ip: bot.IP})
		}
		if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP {
			targets = append(targets, targetItem{addr: fmt.Sprintf("%s:%d", bot.FallbackIP, port), ip: bot.FallbackIP})
		}
		if bot.Hostname != "" {
			hostAddr := fmt.Sprintf("%s:%d", mdns.CleanHostname(bot.Hostname), port)
			targets = append(targets, targetItem{addr: hostAddr, ip: ""})
		}
	}

	// Try each target in sequence
	for _, target := range targets {
		start := time.Now()
		dialAddr := target.addr
		host, portStr, err := net.SplitHostPort(dialAddr)
		if err == nil && strings.HasSuffix(strings.ToLower(host), ".local") {
			if ip, err := mdns.ResolveHostname(host, 400*time.Millisecond); err == nil && ip != "" {
				dialAddr = net.JoinHostPort(ip, portStr)
				if target.ip == "" {
					target.ip = ip
				}
				resolvedIP = ip
			}
		}

		conn, err := net.DialTimeout("tcp", dialAddr, 800*time.Millisecond)
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
