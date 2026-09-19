package mdns

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/miekg/dns"
	"golang.org/x/net/ipv4"
)

type cacheEntry struct {
	ip        string
	expiresAt time.Time
}

var (
	cacheMu sync.RWMutex
	ipCache = make(map[string]cacheEntry)
)

// getCachedIP retrieves an unexpired IP from the in-memory mDNS cache.
func getCachedIP(host string) (string, bool) {
	cacheMu.RLock()
	defer cacheMu.RUnlock()
	entry, ok := ipCache[host]
	if ok && time.Now().Before(entry.expiresAt) {
		return entry.ip, true
	}
	return "", false
}

// setCachedIP stores an IP in the in-memory cache with the specified TTL.
func setCachedIP(host, ip string, ttl time.Duration) {
	cacheMu.Lock()
	defer cacheMu.Unlock()
	ipCache[host] = cacheEntry{
		ip:        ip,
		expiresAt: time.Now().Add(ttl),
	}
}

// CleanHostname normalises a host string into a lower-case hostname ending in .local.
func CleanHostname(host string) string {
	host = strings.TrimSpace(host)
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}
	clean := strings.TrimSuffix(strings.ToLower(host), ".")
	if !strings.HasSuffix(clean, ".local") {
		clean += ".local"
	}
	return clean
}

// ResolveHostname resolves a .local mDNS hostname to an IPv4 address.
// It prioritises direct pure-Go multicast DNS querying (RFC 6762) over local network interfaces,
// ensuring reliable resolution on Linux / Raspberry Pi even when Avahi or CGO are absent.
func ResolveHostname(host string, timeout time.Duration) (string, error) {
	host = strings.TrimSpace(host)
	if host == "" {
		return "", fmt.Errorf("empty hostname")
	}

	// Remove port if present
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}

	// If already a valid IP address, return immediately
	if net.ParseIP(host) != nil {
		return host, nil
	}

	cleanHost := CleanHostname(host)

	// Check cache
	if cached, ok := getCachedIP(cleanHost); ok {
		return cached, nil
	}

	if timeout <= 0 {
		timeout = 800 * time.Millisecond
	}

	// 1. Direct Pure-Go Multicast DNS query over local interfaces
	res := ResolveAll([]string{cleanHost}, timeout)
	if ip, ok := res[cleanHost]; ok && ip != "" {
		setCachedIP(cleanHost, ip, 30*time.Second)
		setCachedIP(host, ip, 30*time.Second)
		return ip, nil
	}

	// 2. Fallback to OS resolver (works on Windows/macOS or Linux with Avahi NSS)
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	var resolver net.Resolver
	ips, err := resolver.LookupHost(ctx, cleanHost)
	if err == nil && len(ips) > 0 {
		for _, ip := range ips {
			if parsed := net.ParseIP(ip); parsed != nil && parsed.To4() != nil {
				setCachedIP(cleanHost, ip, 30*time.Second)
				setCachedIP(host, ip, 30*time.Second)
				return ip, nil
			}
		}
		setCachedIP(cleanHost, ips[0], 30*time.Second)
		return ips[0], nil
	}

	return "", fmt.Errorf("mDNS host %q could not be resolved", host)
}

// ResolveAll resolves multiple hostnames concurrently via a single multicast query burst.
// Returns a map of normalized hostname -> resolved IPv4 address.
func ResolveAll(hostnames []string, timeout time.Duration) map[string]string {
	results := make(map[string]string)
	if len(hostnames) == 0 {
		return results
	}

	needed := make(map[string]string) // cleanHost -> original
	for _, h := range hostnames {
		h = strings.TrimSpace(h)
		if h == "" {
			continue
		}
		if ip := net.ParseIP(h); ip != nil {
			results[h] = ip.String()
			continue
		}
		clean := CleanHostname(h)
		if cached, ok := getCachedIP(clean); ok {
			results[h] = cached
			results[clean] = cached
			continue
		}
		needed[clean] = h
	}

	if len(needed) == 0 {
		return results
	}

	if timeout <= 0 {
		timeout = 800 * time.Millisecond
	}

	// 1. Ephemeral UDP socket for unicast replies (QU bit in queries)
	unicastConn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: 0})
	if err != nil {
		return results
	}
	defer unicastConn.Close()

	// 2. Multicast UDP socket for multicast replies (QM replies)
	var multiConn *net.UDPConn
	mConn, err := net.ListenMulticastUDP("udp4", nil, &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353})
	if err == nil {
		multiConn = mConn
		defer multiConn.Close()
	}

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	var mu sync.Mutex
	var wg sync.WaitGroup

	readResponses := func(conn *net.UDPConn) {
		defer wg.Done()
		buf := make([]byte, 2048)
		for {
			select {
			case <-ctx.Done():
				return
			default:
			}
			_ = conn.SetReadDeadline(time.Now().Add(60 * time.Millisecond))
			n, _, err := conn.ReadFrom(buf)
			if err != nil {
				continue
			}
			var resp dns.Msg
			if err := resp.Unpack(buf[:n]); err != nil {
				continue
			}

			checkRRs := func(rrs []dns.RR) {
				for _, rr := range rrs {
					if a, ok := rr.(*dns.A); ok {
						rrName := strings.TrimSuffix(strings.ToLower(a.Hdr.Name), ".")
						ipStr := a.A.String()
						mu.Lock()
						if orig, exists := needed[rrName]; exists {
							results[orig] = ipStr
							results[rrName] = ipStr
							setCachedIP(rrName, ipStr, 30*time.Second)
							setCachedIP(orig, ipStr, 30*time.Second)
							delete(needed, rrName)
							if len(needed) == 0 {
								mu.Unlock()
								cancel()
								return
							}
						}
						mu.Unlock()
					}
				}
			}

			checkRRs(resp.Answer)
			checkRRs(resp.Extra)
		}
	}

	wg.Add(1)
	go readResponses(unicastConn)
	if multiConn != nil {
		wg.Add(1)
		go readResponses(multiConn)
	}

	dst := &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}
	pconn := ipv4.NewPacketConn(unicastConn)
	ifaces, _ := net.Interfaces()

	// Dispatch queries for all needed hostnames
	for clean := range needed {
		fqdn := clean + "."

		// Unicast-response query (QU bit)
		msgQU := new(dns.Msg)
		msgQU.SetQuestion(fqdn, dns.TypeA)
		msgQU.RecursionDesired = false
		if len(msgQU.Question) > 0 {
			msgQU.Question[0].Qclass = dns.ClassINET | 0x8000
		}
		qBytesQU, err := msgQU.Pack()
		if err != nil {
			continue
		}

		// Multicast-response query (QM)
		msgQM := new(dns.Msg)
		msgQM.SetQuestion(fqdn, dns.TypeA)
		msgQM.RecursionDesired = false
		qBytesQM, _ := msgQM.Pack()

		// Broadcast across all active multicast IPv4 network interfaces
		for _, iface := range ifaces {
			if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 || iface.Flags&net.FlagLoopback != 0 {
				continue
			}
			addrs, err := iface.Addrs()
			if err != nil {
				continue
			}
			hasIPv4 := false
			for _, addr := range addrs {
				if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
					hasIPv4 = true
					break
				}
			}
			if !hasIPv4 {
				continue
			}

			_ = pconn.SetMulticastInterface(&iface)
			_, _ = pconn.WriteTo(qBytesQU, nil, dst)
			if len(qBytesQM) > 0 {
				_, _ = pconn.WriteTo(qBytesQM, nil, dst)
			}
		}

		// Fallback send to default route
		_, _ = unicastConn.WriteTo(qBytesQU, dst)
	}

	<-ctx.Done()
	wg.Wait()
	return results
}

// InstallHTTPResolver configures the global http.DefaultTransport with an mDNS-aware DialContext.
// Any outbound HTTP request to a .local hostname will transparently resolve via mDNS
// before connecting, solving hostname resolution failures on platforms without Avahi/systemd-resolved.
func InstallHTTPResolver() {
	defaultDialer := &net.Dialer{
		Timeout:   10 * time.Second,
		KeepAlive: 30 * time.Second,
	}

	if t, ok := http.DefaultTransport.(*http.Transport); ok {
		t.DialContext = func(ctx context.Context, network, addr string) (net.Conn, error) {
			host, port, err := net.SplitHostPort(addr)
			if err == nil && strings.HasSuffix(strings.ToLower(host), ".local") {
				if ip, err := ResolveHostname(host, 600*time.Millisecond); err == nil && ip != "" {
					addr = net.JoinHostPort(ip, port)
				}
			}
			return defaultDialer.DialContext(ctx, network, addr)
		}
	}
}
