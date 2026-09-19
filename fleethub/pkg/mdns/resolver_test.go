package mdns

import (
	"testing"
	"time"
)

func TestResolveAll(t *testing.T) {
	hosts := []string{"simplebot1.local", "speakerbot1.local", "rfbot1.local", "rfbot6.local"}
	res := ResolveAll(hosts, 600*time.Millisecond)
	t.Logf("ResolveAll discovered %d bots: %+v", len(res), res)
	if len(res) == 0 {
		t.Log("No local bots replied (this is expected if running in CI without bots on the network)")
	}
}

func TestResolveHostname(t *testing.T) {
	// Test IP passthrough
	ip, err := ResolveHostname("192.168.1.55", 100*time.Millisecond)
	if err != nil || ip != "192.168.1.55" {
		t.Fatalf("Expected IP passthrough, got %q, %v", ip, err)
	}

	// Test nonexistent host timeout
	_, err = ResolveHostname("definitely-nonexistent-device-12345.local", 200*time.Millisecond)
	if err == nil {
		t.Fatal("Expected error for nonexistent device")
	}
}
