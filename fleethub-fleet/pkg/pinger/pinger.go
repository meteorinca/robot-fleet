package pinger

import (
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub-fleet/pkg/config"
)

// Pinger periodically checks network accessibility of fleet devices using TCP/HTTP connection probes with fallback IP support.
type Pinger struct {
	cfg        *config.Config
	stop       chan struct{}
	onPing     func(deviceID string, latencyMs float64, status, ip string)
	mu         sync.Mutex
}

// NewPinger creates a new device ping service.
func NewPinger(cfg *config.Config) *Pinger {
	return &Pinger{
		cfg:  cfg,
		stop: make(chan struct{}),
	}
}

// SetCallback registers a callback to receive every ping result.
func (p *Pinger) SetCallback(cb func(deviceID string, latencyMs float64, status, ip string)) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.onPing = cb
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

// PingAll iterates through configured bots and tests connectivity with fallback IP support.
func (p *Pinger) PingAll() {
	bots := p.cfg.Bots
	var wg sync.WaitGroup

	for _, bot := range bots {
		wg.Add(1)
		go func(b config.RobotNode) {
			defer wg.Done()
			latency, online, discoveredIP := p.PingDevice(b)
			
			status := "offline"
			if online {
				status = "online"
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

			p.mu.Lock()
			cb := p.onPing
			p.mu.Unlock()
			if cb != nil {
				ipToLog := b.IP
				if discoveredIP != "" {
					ipToLog = discoveredIP
				}
				cb(b.ID, latency, status, ipToLog)
			}
		}(bot)
	}

	wg.Wait()
}

// PingDevice attempts to connect to a bot using its hostname, fallback IP, or known IP on HTTP port 80 / custom port.
func (p *Pinger) PingDevice(bot config.RobotNode) (float64, bool, string) {
	ports := []int{}
	if bot.Port != 0 && bot.Port != 4330 {
		ports = append(ports, bot.Port)
	}
	if bot.Port != 80 {
		ports = append(ports, 80)
	}
	if len(ports) == 0 {
		ports = []int{80}
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
		host string
		ip   string
	}

	var hostCandidates []targetItem

	// 1. Direct resolved IP
	if resolvedIP != "" && isValidIP(resolvedIP) {
		hostCandidates = append(hostCandidates, targetItem{host: resolvedIP, ip: resolvedIP})
	}
	// 2. Stored IP
	if isValidIP(bot.IP) {
		hostCandidates = append(hostCandidates, targetItem{host: bot.IP, ip: bot.IP})
	}
	// 3. Fallback IP
	if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP && bot.FallbackIP != resolvedIP {
		hostCandidates = append(hostCandidates, targetItem{host: bot.FallbackIP, ip: bot.FallbackIP})
	}
	// 4. Hostname
	if bot.Hostname != "" {
		hostCandidates = append(hostCandidates, targetItem{host: bot.Hostname, ip: resolvedIP})
		cleanHost := strings.TrimSuffix(bot.Hostname, ".local")
		if !strings.HasSuffix(bot.Hostname, ".local") {
			hostCandidates = append(hostCandidates, targetItem{host: cleanHost + ".local", ip: resolvedIP})
		}
	}
	// 5. Bot ID hostname fallback
	if bot.ID != "" && !strings.Contains(bot.ID, ":") {
		cleanID := strings.TrimSuffix(bot.ID, ".local")
		hostCandidates = append(hostCandidates, targetItem{host: cleanID + ".local", ip: resolvedIP})
	}

	// Deduplicate candidates
	seenHosts := make(map[string]bool)
	var uniqueHosts []targetItem
	for _, hc := range hostCandidates {
		if !seenHosts[hc.host] {
			seenHosts[hc.host] = true
			uniqueHosts = append(uniqueHosts, hc)
		}
	}

	// Try each host candidate with each port
	for _, port := range ports {
		for _, target := range uniqueHosts {
			addr := net.JoinHostPort(target.host, strconv.Itoa(port))
			start := time.Now()
			conn, err := net.DialTimeout("tcp", addr, 1200*time.Millisecond)
			if err == nil {
				latency := float64(time.Since(start).Microseconds()) / 1000.0 // ms
				discoveredIP := target.ip
				if discoveredIP == "" || !isValidIP(discoveredIP) {
					if tcpAddr, ok := conn.RemoteAddr().(*net.TCPAddr); ok && tcpAddr.IP != nil {
						discoveredIP = tcpAddr.IP.String()
					}
				}
				_ = conn.Close()
				return latency, true, discoveredIP
			}
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

