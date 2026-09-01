package db

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDatabaseOperations(t *testing.T) {
	tempDir, err := os.MkdirTemp("", "fleethub_db_test_*")
	if err != nil {
		t.Fatalf("Failed to create temp dir: %v", err)
	}
	defer os.RemoveAll(tempDir)

	dbPath := filepath.Join(tempDir, "test.db")
	database, err := Open(dbPath)
	if err != nil {
		t.Fatalf("Open failed: %v", err)
	}
	defer database.Close()

	// 1. Test RF Ingest and Aggregation
	code := uint32(123456)
	now := time.Now()
	count1, err := database.RecordRFEvent(code, 24, 1, 185, "rfbot6", "Light Sensor Audio Alert", now)
	if err != nil {
		t.Fatalf("RecordRFEvent failed: %v", err)
	}
	if count1 != 1 {
		t.Errorf("Expected count 1, got %d", count1)
	}

	count2, err := database.RecordRFEvent(code, 24, 1, 185, "rfbot6", "Light Sensor Audio Alert", now.Add(time.Second))
	if err != nil {
		t.Fatalf("RecordRFEvent second time failed: %v", err)
	}
	if count2 != 2 {
		t.Errorf("Expected count 2, got %d", count2)
	}

	counts, err := database.GetRFCounts()
	if err != nil {
		t.Fatalf("GetRFCounts failed: %v", err)
	}
	if len(counts) != 1 || counts[0].TotalCount != 2 {
		t.Errorf("Expected 1 aggregate with 2 counts, got %v", counts)
	}

	history, err := database.GetRFHistory(code, 10)
	if err != nil {
		t.Fatalf("GetRFHistory failed: %v", err)
	}
	if len(history) != 2 {
		t.Errorf("Expected 2 history items, got %d", len(history))
	}

	// 2. Test Multi-Instance Entity State Persistence
	err = database.SetEntityState("unit_alpha", "on", "webui", "")
	if err != nil {
		t.Fatalf("SetEntityState failed: %v", err)
	}

	states, err := database.GetEntityStates()
	if err != nil {
		t.Fatalf("GetEntityStates failed: %v", err)
	}
	if state, exists := states["unit_alpha"]; !exists || state.State != "on" {
		t.Errorf("Expected unit_alpha 'on', got %v", state)
	}

	// 3. Test Ping Telemetry
	err = database.RecordPing("speakerbot1", 14.5, "online", "10.0.0.40")
	if err != nil {
		t.Fatalf("RecordPing failed: %v", err)
	}

	pings, err := database.GetPingHistory(1)
	if err != nil {
		t.Fatalf("GetPingHistory failed: %v", err)
	}
	if len(pings) != 1 || pings[0].LatencyMs != 14.5 {
		t.Errorf("Expected ping record with 14.5ms, got %v", pings)
	}

	// 4. Test System Metrics
	err = database.RecordSystemMetrics(12.4, 32.5, 64.0, 18)
	if err != nil {
		t.Fatalf("RecordSystemMetrics failed: %v", err)
	}

	metrics, err := database.GetSystemMetrics(1)
	if err != nil {
		t.Fatalf("GetSystemMetrics failed: %v", err)
	}
	if len(metrics) != 1 || metrics[0].MemUsedMB != 32.5 {
		t.Errorf("Expected metric with 32.5MB, got %v", metrics)
	}

	// 5. Test Speedtest Logs
	err = database.RecordSpeedtest(185.4, 45.2, 18.0, "Cloudflare")
	if err != nil {
		t.Fatalf("RecordSpeedtest failed: %v", err)
	}

	speedHistory, err := database.GetSpeedtestHistory(10)
	if err != nil {
		t.Fatalf("GetSpeedtestHistory failed: %v", err)
	}
	if len(speedHistory) != 1 || speedHistory[0].DownloadMbps != 185.4 {
		t.Errorf("Expected speed record 185.4 Mbps, got %v", speedHistory)
	}

	// 6. Test Noise Pruning and Retention
	_, _ = database.RecordRFEvent(999991, 24, 1, 185, "noise1", "", now)
	_, _ = database.RecordRFEvent(999992, 24, 1, 185, "noise2", "", now)

	// Filtered counts: minHits = 2 should only return code 123456
	filtered, err := database.GetFilteredRFCounts(2, 10)
	if err != nil {
		t.Fatalf("GetFilteredRFCounts failed: %v", err)
	}
	if len(filtered) != 1 || filtered[0].Code != code {
		t.Errorf("Expected only code %d in filtered results, got %v", code, filtered)
	}

	// Prune noise: minHits = 2, preserve empty
	deletedNoise, err := database.PruneRFEphemeralNoise(2, nil)
	if err != nil {
		t.Fatalf("PruneRFEphemeralNoise failed: %v", err)
	}
	if deletedNoise != 2 {
		t.Errorf("Expected 2 noise codes pruned, got %d", deletedNoise)
	}

	// Retention test: enforce retention to 1 record
	deletedRows, err := database.EnforceRFEventRetention(1)
	if err != nil {
		t.Fatalf("EnforceRFEventRetention failed: %v", err)
	}
	if deletedRows == 0 {
		t.Errorf("Expected older events deleted by retention limit, got %d", deletedRows)
	}
}

