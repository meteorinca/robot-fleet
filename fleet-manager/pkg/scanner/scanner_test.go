package scanner

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func testConfig(t *testing.T) Config {
	t.Helper()
	cfg := DefaultConfig()
	cfg.StatePath = filepath.Join(t.TempDir(), "state.json")
	return cfg
}

func newTestScanner(t *testing.T, cfg Config) *Scanner {
	t.Helper()
	s, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func validTelemetry() Telemetry {
	return Telemetry{Status: "running", Platform: "simplebot", Firmware: "0.1", Version: "0.1", Hostname: "simplebot1", Name: "simplebot1", DeviceNumber: 1, Device: "simplebot1", Uptime: 3600, FreeHeap: 182400, Epoch: 1728345678, TimeSynced: true}
}

func TestTargetsAndRange(t *testing.T) {
	cfg := testConfig(t)
	s := newTestScanner(t, cfg)
	if len(s.Snapshot().Bots) != 27 {
		t.Fatal("expected 27 targets")
	}
	for _, b := range s.Snapshot().Bots {
		if b.DeviceNumber < 1 || b.DeviceNumber > 9 || !strings.HasSuffix(b.Hostname, ".local") {
			t.Fatalf("unexpected target: %+v", b)
		}
	}
	cfg.MaxDevice = 10
	if _, err := New(cfg); err == nil {
		t.Fatal("must reject device 10")
	}
	cfg.MaxDevice = 9
	cfg.Platforms = append(cfg.Platforms, "newbot")
	if len(newTestScanner(t, cfg).Snapshot().Bots) != 36 {
		t.Fatal("platform extension failed")
	}
}

func TestOfflineRetentionAndRestart(t *testing.T) {
	cfg := testConfig(t)
	s := newTestScanner(t, cfg)
	telemetry := validTelemetry()
	first := time.Now().UTC()
	s.apply("simplebot1.local", Result{Online: true, IP: "192.0.2.1", Telemetry: &telemetry}, first)
	s.apply("simplebot1.local", Result{Error: "timeout", LatencyMS: -1}, first.Add(time.Second))
	if err := s.Save(); err != nil {
		t.Fatal(err)
	}
	reloaded := newTestScanner(t, cfg)
	b := reloaded.bots["simplebot1.local"]
	if !b.Exists || b.Status != "unknown" || b.Telemetry.Firmware != "0.1" || b.OfflineSince == nil || !b.LastSeen.Equal(first) {
		t.Fatalf("lost history: %+v", b)
	}
	reloaded.apply(b.Hostname, Result{Online: true, IP: "192.0.2.2"}, first.Add(2*time.Second))
	b = reloaded.bots[b.Hostname]
	if b.OfflineSince != nil || b.IP != "192.0.2.2" || !b.FirstSeen.Equal(first) {
		t.Fatalf("bad recovery: %+v", b)
	}
}

func TestUnifiedTelemetryAndIdentity(t *testing.T) {
	s := newTestScanner(t, testConfig(t))
	b := s.bots["simplebot1.local"]
	tel := validTelemetry()
	data, _ := json.Marshal(tel)
	if _, err := decodeTelemetry(data, b); err != nil {
		t.Fatal(err)
	}
	if _, err := decodeTelemetry([]byte(`{"status":"running"}`), b); err == nil {
		t.Fatal("accepted incomplete schema")
	}
	tel.DeviceNumber = 2
	data, _ = json.Marshal(tel)
	if _, err := decodeTelemetry(data, b); err == nil {
		t.Fatal("accepted another device's telemetry")
	}
	tel = validTelemetry()
	tel.TimeSynced = false
	tel.Epoch = 0
	data, _ = json.Marshal(tel)
	got, err := decodeTelemetry(data, b)
	if err != nil || got.TimeSynced || got.Epoch != 0 {
		t.Fatal("zero telemetry fields must be preserved")
	}
}

func TestHTTPFallbackAndFreshResolution(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.Host, "simplebot1.local:") {
			t.Errorf("Host must retain mDNS name: %s", r.Host)
		}
		if r.URL.Path == "/api/info" {
			http.NotFound(w, r)
			return
		}
		_ = json.NewEncoder(w).Encode(validTelemetry())
	}))
	defer server.Close()
	cfg := testConfig(t)
	_, port, _ := net.SplitHostPort(strings.TrimPrefix(server.URL, "http://"))
	cfg.Port, _ = strconv.Atoi(port)
	cfg.Resolve = func(ctx context.Context, host string) (string, error) { calls.Add(1); return "127.0.0.1", nil }
	s := newTestScanner(t, cfg)
	b := s.bots["simplebot1.local"]
	for i := 0; i < 2; i++ {
		result := s.Probe(context.Background(), b)
		if !result.Online || result.Telemetry == nil || result.Error != "" {
			t.Fatalf("probe failed: %+v", result)
		}
	}
	if calls.Load() != 2 {
		t.Fatal("must freshly resolve each probe")
	}
}

func TestLegacyVersionsAndUpgrade(t *testing.T) {
	data := []byte(`{"status":"running","version":"v1.0","epoch":1728345678,"time_synced":true}`)
	if legacyVersion(data) != "v1.0" {
		t.Fatal("lost deployed firmware version")
	}
	if legacyVersion([]byte(`{"status":"running","version":"v1.0","device_number":2}`)) != "" {
		t.Fatal("must not bypass conflicting unified identity")
	}
	s := newTestScanner(t, testConfig(t))
	now := time.Now().UTC()
	s.apply("simplebot1.local", Result{Online: true, ReportedVersion: "v1.0", LegacyTelemetry: data}, now)
	s.apply("simplebot1.local", Result{LatencyMS: -1}, now.Add(time.Second))
	b := s.bots["simplebot1.local"]
	if b.ReportedVersion != "v1.0" || b.Telemetry != nil || len(b.LegacyTelemetry) == 0 {
		t.Fatal("lost legacy telemetry while offline")
	}
	tel := validTelemetry()
	s.apply(b.Hostname, Result{Online: true, Telemetry: &tel}, now.Add(2*time.Second))
	b = s.bots[b.Hostname]
	if b.ReportedVersion != "0.1" || b.Telemetry == nil || b.LegacyTelemetry != nil {
		t.Fatal("unified telemetry did not replace legacy data")
	}
}

func TestProgressPersistsBeforeSlowTargetCompletes(t *testing.T) {
	cfg := testConfig(t)
	cfg.Platforms = []string{"simplebot"}
	cfg.MaxDevice = 2
	cfg.Workers = 2
	slowStarted := make(chan struct{})
	release := make(chan struct{})
	cfg.Resolve = func(ctx context.Context, host string) (string, error) {
		if host == "simplebot2.local" {
			close(slowStarted)
			select {
			case <-release:
			case <-ctx.Done():
			}
			return "", errors.New("slow offline")
		}
		return "", errors.New("fast offline")
	}
	s := newTestScanner(t, cfg)
	done := make(chan struct{})
	go func() { s.RunOnce(context.Background(), func(err error) { t.Error(err) }); close(done) }()
	<-slowStarted
	defer func() { close(release); <-done }()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(cfg.StatePath)
		if err == nil {
			var snapshot Snapshot
			if json.Unmarshal(data, &snapshot) == nil && snapshot.Bots[0].Status == "offline" && snapshot.Bots[1].Status == "unknown" {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("fast result was not persisted while the slow target was pending")
}

func TestCancellationAndBoundedWorkers(t *testing.T) {
	cfg := testConfig(t)
	cfg.Workers = 3
	var active, peak atomic.Int32
	ready := make(chan struct{}, 3)
	cfg.Resolve = func(ctx context.Context, host string) (string, error) {
		n := active.Add(1)
		defer active.Add(-1)
		for old := peak.Load(); n > old; old = peak.Load() {
			if peak.CompareAndSwap(old, n) {
				break
			}
		}
		ready <- struct{}{}
		<-ctx.Done()
		return "", ctx.Err()
	}
	s := newTestScanner(t, cfg)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { s.Scan(ctx); close(done) }()
	for n := 0; n < 3; n++ {
		<-ready
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("scan ignored cancellation")
	}
	if peak.Load() > 3 || active.Load() != 0 {
		t.Fatal("worker bound or cleanup failed")
	}
	for _, b := range s.Snapshot().Bots {
		if b.Status != "unknown" {
			t.Fatal("shutdown must not mark devices offline")
		}
	}
}

func TestSlowSubscriberDoesNotBlockAndCorruptStateFails(t *testing.T) {
	cfg := testConfig(t)
	s := newTestScanner(t, cfg)
	ch, unsubscribe := s.Subscribe()
	defer unsubscribe()
	for n := 0; n < 100; n++ {
		s.apply("simplebot1.local", Result{Online: true, LatencyMS: float64(n)}, time.Now().UTC())
	}
	if (<-ch).Bots[9].LatencyMS != 99 { // rfbot1..9 precede simplebot1 in sorted inventory.
		t.Fatal("subscriber did not receive latest state")
	}
	if err := os.WriteFile(cfg.StatePath, []byte("invalid json"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := New(cfg); err == nil {
		t.Fatal("must not silently overwrite corrupt inventory")
	}
}
