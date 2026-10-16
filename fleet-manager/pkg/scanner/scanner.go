package scanner

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

type Telemetry struct {
	Status       string `json:"status"`
	Platform     string `json:"platform"`
	Firmware     string `json:"firmware"`
	Version      string `json:"version"`
	Hostname     string `json:"hostname"`
	Name         string `json:"name"`
	DeviceNumber int    `json:"device_number"`
	Device       string `json:"device"`
	Uptime       uint64 `json:"uptime"`
	FreeHeap     uint64 `json:"free_heap"`
	Epoch        int64  `json:"epoch"`
	TimeSynced   bool   `json:"time_synced"`
}

type Bot struct {
	ReportedVersion string          `json:"reported_version,omitempty"`
	LegacyTelemetry json.RawMessage `json:"legacy_telemetry,omitempty"`
	Hostname        string          `json:"hostname"`
	Platform        string          `json:"platform"`
	DeviceNumber    int             `json:"device_number"`
	URL             string          `json:"url"`
	Status          string          `json:"status"`
	Exists          bool            `json:"exists"`
	IP              string          `json:"ip,omitempty"`
	LatencyMS       float64         `json:"latency_ms"`
	FirstSeen       *time.Time      `json:"first_seen,omitempty"`
	LastSeen        *time.Time      `json:"last_seen,omitempty"`
	LastChecked     *time.Time      `json:"last_checked,omitempty"`
	StatusChangedAt *time.Time      `json:"status_changed_at,omitempty"`
	OfflineSince    *time.Time      `json:"offline_since,omitempty"`
	LastTelemetryAt *time.Time      `json:"last_telemetry_at,omitempty"`
	Telemetry       *Telemetry      `json:"telemetry,omitempty"`
	Error           string          `json:"error,omitempty"`
}

type Snapshot struct {
	SchemaVersion int       `json:"schema_version"`
	UpdatedAt     time.Time `json:"updated_at"`
	Bots          []Bot     `json:"bots"`
}

type Config struct {
	Port      int
	Platforms []string
	MaxDevice int
	Workers   int
	Timeout   time.Duration
	Interval  time.Duration
	StatePath string
	Resolve   func(context.Context, string) (string, error)
}

func DefaultConfig() Config {
	return Config{Port: 80, Platforms: []string{"simplebot", "rfbot", "speakerbot"}, MaxDevice: 9,
		Workers: 12, Timeout: 3 * time.Second, Interval: 15 * time.Second,
		StatePath: "fleet_state.generated.json", Resolve: Resolve}
}

type Scanner struct {
	cfg     Config
	mu      sync.RWMutex
	bots    map[string]Bot
	updated time.Time
	scanMu  sync.Mutex
	trigger chan struct{}
	dirty   chan struct{}
	subs    map[chan Snapshot]struct{}
}

func New(cfg Config) (*Scanner, error) {
	if cfg.Port == 0 {
		cfg.Port = 80
	}
	if cfg.Port < 1 || cfg.Port > 65535 {
		return nil, errors.New("invalid HTTP port")
	}
	if cfg.MaxDevice < 1 || cfg.MaxDevice > 9 || cfg.Workers < 1 || cfg.Workers > 27 || cfg.Timeout <= 0 || cfg.Interval <= 0 || len(cfg.Platforms) == 0 || cfg.StatePath == "" {
		return nil, errors.New("require platforms, devices 1..9, workers 1..27, positive durations and a state path")
	}
	if cfg.Resolve == nil {
		cfg.Resolve = Resolve
	}
	s := &Scanner{cfg: cfg, bots: make(map[string]Bot), trigger: make(chan struct{}, 1), dirty: make(chan struct{}, 1), subs: make(map[chan Snapshot]struct{})}
	for _, platform := range cfg.Platforms {
		if platform == "" || strings.Trim(platform, "abcdefghijklmnopqrstuvwxyz0123456789-") != "" {
			return nil, fmt.Errorf("invalid platform %q", platform)
		}
		for n := 1; n <= cfg.MaxDevice; n++ {
			host := fmt.Sprintf("%s%d.local", platform, n)
			url := "http://" + host
			if cfg.Port != 80 {
				url = fmt.Sprintf("%s:%d", url, cfg.Port)
			}
			s.bots[host] = Bot{Hostname: host, Platform: platform, DeviceNumber: n, URL: url, Status: "unknown", LatencyMS: -1}
		}
	}
	data, err := os.ReadFile(cfg.StatePath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	if err == nil {
		var saved Snapshot
		if err := json.Unmarshal(data, &saved); err != nil {
			return nil, fmt.Errorf("read inventory: %w", err)
		}
		if saved.SchemaVersion != 1 {
			return nil, errors.New("unsupported inventory schema")
		}
		for _, bot := range saved.Bots {
			if target, ok := s.bots[bot.Hostname]; ok {
				bot.Platform, bot.DeviceNumber, bot.URL = target.Platform, target.DeviceNumber, target.URL
				bot.Status = "unknown" // Recheck persisted reachability before allowing actions.
				s.bots[bot.Hostname] = bot
			}
		}
	}
	return s, nil
}

func (s *Scanner) Snapshot() Snapshot {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.snapshotLocked()
}

func (s *Scanner) snapshotLocked() Snapshot {
	result := Snapshot{SchemaVersion: 1, UpdatedAt: s.updated, Bots: make([]Bot, 0, len(s.bots))}
	for _, b := range s.bots {
		result.Bots = append(result.Bots, b)
	}
	sort.Slice(result.Bots, func(i, j int) bool { return result.Bots[i].Hostname < result.Bots[j].Hostname })
	return result
}

// Subscribe keeps only the latest snapshot for each client. Slow browsers never block probes.
func (s *Scanner) Subscribe() (<-chan Snapshot, func()) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ch := make(chan Snapshot, 1)
	s.subs[ch] = struct{}{}
	ch <- s.snapshotLocked()
	return ch, func() { s.mu.Lock(); delete(s.subs, ch); s.mu.Unlock() }
}

func (s *Scanner) RequestScan() bool {
	select {
	case s.trigger <- struct{}{}:
		return true
	default:
		return false
	}
}

type Result struct {
	ReportedVersion string
	LegacyTelemetry json.RawMessage
	Online          bool
	IP              string
	LatencyMS       float64
	Telemetry       *Telemetry
	Error           string
}

// Probe first pings TCP port 80, then reads the unified telemetry using the same
// freshly resolved address. The HTTP Host and inventory URL remain the mDNS name.
func (s *Scanner) Probe(ctx context.Context, bot Bot) Result {
	ctx, cancel := context.WithTimeout(ctx, s.cfg.Timeout)
	defer cancel()
	ip, err := s.cfg.Resolve(ctx, bot.Hostname)
	if err != nil {
		return Result{LatencyMS: -1, Error: err.Error()}
	}
	dialer := &net.Dialer{}
	start := time.Now()
	port := fmt.Sprint(s.cfg.Port)
	conn, err := dialer.DialContext(ctx, "tcp", net.JoinHostPort(ip, port))
	if err != nil {
		return Result{IP: ip, LatencyMS: -1, Error: err.Error()}
	}
	_ = conn.Close()
	result := Result{Online: true, IP: ip, LatencyMS: float64(time.Since(start).Microseconds()) / 1000}
	transport := &http.Transport{Proxy: nil, DisableKeepAlives: true,
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			return dialer.DialContext(ctx, network, net.JoinHostPort(ip, port))
		}}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	var probeErrors []string
	for _, path := range []string{"/api/info", "/status", "/info"} {
		if ctx.Err() != nil {
			break
		}
		req, _ := http.NewRequestWithContext(ctx, http.MethodGet, bot.URL+path, nil)
		resp, err := client.Do(req)
		if err != nil {
			probeErrors = append(probeErrors, path+": "+err.Error())
			continue
		}
		data, readErr := io.ReadAll(io.LimitReader(resp.Body, 65537))
		_ = resp.Body.Close()
		if resp.StatusCode != http.StatusOK || readErr != nil || len(data) > 65536 {
			probeErrors = append(probeErrors, fmt.Sprintf("%s: invalid HTTP response (%d)", path, resp.StatusCode))
			continue
		}
		telemetry, err := decodeTelemetry(data, bot)
		if err != nil {
			probeErrors = append(probeErrors, path+": "+err.Error())
			if version := legacyVersion(data); version != "" {
				result.ReportedVersion, result.LegacyTelemetry = version, json.RawMessage(data)
			}
			continue
		}
		result.Telemetry, result.Error = telemetry, ""
		return result
	}
	result.Error = strings.Join(probeErrors, "; ")
	if result.Error == "" {
		result.Error = "telemetry timed out"
	}
	return result
}

// Older deployed firmware has only status/version/time fields. Preserve what
// it actually reports without manufacturing missing unified telemetry fields.
func legacyVersion(data []byte) string {
	var fields map[string]json.RawMessage
	if json.Unmarshal(data, &fields) != nil {
		return ""
	}
	for _, key := range []string{"platform", "hostname", "device_number"} {
		if _, ok := fields[key]; ok {
			return ""
		}
	}
	var status, version string
	if json.Unmarshal(fields["status"], &status) != nil || json.Unmarshal(fields["version"], &version) != nil || status != "running" {
		return ""
	}
	return version
}

func decodeTelemetry(data []byte, bot Bot) (*Telemetry, error) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(data, &fields); err != nil {
		return nil, err
	}
	for _, key := range []string{"status", "platform", "firmware", "version", "hostname", "name", "device_number", "device", "uptime", "free_heap", "epoch", "time_synced"} {
		value, ok := fields[key]
		if !ok || string(value) == "null" {
			return nil, fmt.Errorf("missing %s", key)
		}
	}
	var t Telemetry
	if err := json.Unmarshal(data, &t); err != nil {
		return nil, err
	}
	if t.Platform != bot.Platform || t.DeviceNumber != bot.DeviceNumber || strings.TrimSuffix(strings.ToLower(t.Hostname), ".local") != strings.TrimSuffix(bot.Hostname, ".local") {
		return nil, errors.New("telemetry identity does not match target")
	}
	return &t, nil
}

func (s *Scanner) apply(host string, result Result, now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	b := s.bots[host]
	status := "offline"
	if result.Online {
		status = "online"
	}
	if b.Status != status {
		b.StatusChangedAt = &now
	}
	if result.Online {
		if b.FirstSeen == nil {
			b.FirstSeen = &now
		}
		b.Exists, b.LastSeen, b.OfflineSince = true, &now, nil
	} else if b.Exists && b.OfflineSince == nil {
		b.OfflineSince = &now
	}
	b.Status, b.LastChecked, b.LatencyMS, b.Error = status, &now, result.LatencyMS, result.Error
	if result.IP != "" {
		b.IP = result.IP
	}
	if result.Telemetry != nil {
		b.Telemetry, b.LastTelemetryAt = result.Telemetry, &now
		b.ReportedVersion, b.LegacyTelemetry = result.Telemetry.Firmware, nil
	} else if result.LegacyTelemetry != nil {
		b.ReportedVersion, b.LegacyTelemetry, b.LastTelemetryAt = result.ReportedVersion, result.LegacyTelemetry, &now
		b.Telemetry = nil
	}
	s.bots[host], s.updated = b, now
	snapshot := s.snapshotLocked()
	for ch := range s.subs {
		select {
		case <-ch:
		default:
		}
		ch <- snapshot
	}
	select {
	case s.dirty <- struct{}{}:
	default:
	}
}

// Scan publishes each completed probe immediately rather than waiting for the sweep.
func (s *Scanner) Scan(ctx context.Context) {
	if !s.scanMu.TryLock() {
		return
	}
	defer s.scanMu.Unlock()
	jobs := make(chan Bot)
	var wg sync.WaitGroup
	for n := 0; n < s.cfg.Workers; n++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for b := range jobs {
				if ctx.Err() != nil {
					return
				}
				r := s.Probe(ctx, b)
				if ctx.Err() == nil {
					s.apply(b.Hostname, r, time.Now().UTC())
				}
			}
		}()
	}
	for _, b := range s.Snapshot().Bots {
		select {
		case jobs <- b:
		case <-ctx.Done():
			close(jobs)
			wg.Wait()
			return
		}
	}
	close(jobs)
	wg.Wait()
}

// Save atomically replaces the JSON so readers never see a partial document.
func (s *Scanner) Save() error {
	data, err := json.MarshalIndent(s.Snapshot(), "", "  ")
	if err != nil {
		return err
	}
	dir := filepath.Dir(s.cfg.StatePath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".fleet-state-*.json")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(append(data, '\n')); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(f.Name(), s.cfg.StatePath)
}

// Run owns a single asynchronous writer and serial sweeps with bounded workers.
func (s *Scanner) Run(ctx context.Context, report func(error)) {
	s.run(ctx, false, report)
}

func (s *Scanner) RunOnce(ctx context.Context, report func(error)) {
	s.run(ctx, true, report)
}

func (s *Scanner) run(ctx context.Context, once bool, report func(error)) {
	writerCtx, stopWriter := context.WithCancel(ctx)
	defer stopWriter()
	var writer sync.WaitGroup
	writer.Add(1)
	go func() {
		defer writer.Done()
		for {
			select {
			case <-s.dirty:
				if err := s.Save(); err != nil {
					report(err)
				}
			case <-writerCtx.Done():
				if err := s.Save(); err != nil {
					report(err)
				}
				return
			}
		}
	}()
	s.Scan(ctx)
	if once {
		stopWriter()
		writer.Wait()
		return
	}
	ticker := time.NewTicker(s.cfg.Interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			writer.Wait()
			return
		case <-ticker.C:
			s.Scan(ctx)
		case <-s.trigger:
			s.Scan(ctx)
		}
	}
}
