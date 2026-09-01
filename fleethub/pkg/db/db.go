package db

import (
	"database/sql"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// RFEventRecord represents a single recorded RF transmission or ingest.
type RFEventRecord struct {
	ID          int64     `json:"id"`
	Code        uint32    `json:"code"`
	Bits        uint      `json:"bits"`
	Protocol    uint      `json:"protocol"`
	Pulse       uint      `json:"pulse"`
	Gateway     string    `json:"gateway"`
	MatchedRule string    `json:"matched_rule"`
	CreatedAt   time.Time `json:"created_at"`
}

// RFAggregate represents reception statistics for a specific RF code.
type RFAggregate struct {
	Code       uint32    `json:"code"`
	TotalCount int64     `json:"total_count"`
	FirstSeen  time.Time `json:"first_seen"`
	LastSeen   time.Time `json:"last_seen"`
}

// EntityState represents persisted on/off state for a device, unit, or virtual switch.
type EntityState struct {
	EntityID   string    `json:"entity_id"`
	State      string    `json:"state"` // "on" or "off"
	UpdatedAt  time.Time `json:"updated_at"`
	Source     string    `json:"source"`
	Attributes string    `json:"attributes,omitempty"`
}

// PingRecord represents an entry in the historical device ping log.
type PingRecord struct {
	ID        int64     `json:"id"`
	DeviceID  string    `json:"device_id"`
	LatencyMs float64   `json:"latency_ms"`
	Status    string    `json:"status"`
	IP        string    `json:"ip"`
	CreatedAt time.Time `json:"created_at"`
}

// SystemMetricRecord represents host performance telemetry over time.
type SystemMetricRecord struct {
	ID         int64     `json:"id"`
	CPUPct     float64   `json:"cpu_pct"`
	MemUsedMB  float64   `json:"mem_used_mb"`
	MemTotalMB float64   `json:"mem_total_mb"`
	Goroutines int       `json:"goroutines"`
	CreatedAt  time.Time `json:"created_at"`
}

// SpeedtestRecord represents a network speedtest measurement.
type SpeedtestRecord struct {
	ID           int64     `json:"id"`
	DownloadMbps float64   `json:"download_mbps"`
	UploadMbps   float64   `json:"upload_mbps"`
	PingMs       float64   `json:"ping_ms"`
	Server       string    `json:"server"`
	CreatedAt    time.Time `json:"created_at"`
}

// DB wraps the SQLite database with thread-safe helpers and prepared statements.
type DB struct {
	db *sql.DB
	mu sync.RWMutex
}

// Open initializes or creates the SQLite database with WAL optimizations.
func Open(dbPath string) (*DB, error) {
	if err := os.MkdirAll(filepath.Dir(dbPath), 0755); err != nil && filepath.Dir(dbPath) != "." {
		return nil, fmt.Errorf("failed to create db directory: %w", err)
	}

	// Connect with modernc sqlite driver
	dsn := fmt.Sprintf("file:%s?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)&_pragma=cache_size(-2000)", dbPath)
	conn, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("failed to open sqlite database: %w", err)
	}

	conn.SetMaxOpenConns(1) // SQLite single-writer safety
	conn.SetMaxIdleConns(1)

	instance := &DB{db: conn}
	if err := instance.migrate(); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("database migration failed: %w", err)
	}

	// Start background ring-buffer pruning routine (runs every 6 hours)
	go instance.pruneLoop()

	log.Printf("[FleetHub DB] High-performance SQLite database ready at %s (WAL Mode)", dbPath)
	return instance, nil
}

// Close cleanly flushes and closes the database connection.
func (d *DB) Close() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.db.Close()
}

func (d *DB) migrate() error {
	schema := `
	CREATE TABLE IF NOT EXISTS rf_events (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		code INTEGER NOT NULL,
		bits INTEGER NOT NULL,
		protocol INTEGER NOT NULL,
		pulse INTEGER NOT NULL,
		gateway TEXT NOT NULL,
		matched_rule TEXT NOT NULL,
		created_at DATETIME NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_rf_events_code ON rf_events(code);
	CREATE INDEX IF NOT EXISTS idx_rf_events_time ON rf_events(created_at);

	CREATE TABLE IF NOT EXISTS rf_aggregates (
		code INTEGER PRIMARY KEY,
		total_count INTEGER NOT NULL DEFAULT 0,
		first_seen DATETIME NOT NULL,
		last_seen DATETIME NOT NULL
	);

	CREATE TABLE IF NOT EXISTS entity_states (
		entity_id TEXT PRIMARY KEY,
		state TEXT NOT NULL,
		updated_at DATETIME NOT NULL,
		source TEXT NOT NULL,
		attributes TEXT NOT NULL DEFAULT ''
	);

	CREATE TABLE IF NOT EXISTS ping_history (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		device_id TEXT NOT NULL,
		latency_ms REAL NOT NULL,
		status TEXT NOT NULL,
		ip TEXT NOT NULL,
		created_at DATETIME NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_ping_history_dev ON ping_history(device_id, created_at);

	CREATE TABLE IF NOT EXISTS system_metrics (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		cpu_pct REAL NOT NULL,
		mem_used_mb REAL NOT NULL,
		mem_total_mb REAL NOT NULL,
		goroutines INTEGER NOT NULL,
		created_at DATETIME NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_sys_metrics_time ON system_metrics(created_at);

	CREATE TABLE IF NOT EXISTS speedtest_results (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		download_mbps REAL NOT NULL,
		upload_mbps REAL NOT NULL,
		ping_ms REAL NOT NULL,
		server TEXT NOT NULL,
		created_at DATETIME NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_speedtest_time ON speedtest_results(created_at);
	`

	_, err := d.db.Exec(schema)
	return err
}

// ──────────────────────────────────────────
// RF Ingest & Signal Aggregate Operations
// ──────────────────────────────────────────

// RecordRFEvent logs an incoming RF event and atomically increments the reception counter.
func (d *DB) RecordRFEvent(code uint32, bits, proto, pulse uint, gateway, ruleName string, ts time.Time) (int64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	tx, err := d.db.Begin()
	if err != nil {
		return 0, err
	}
	defer func() { _ = tx.Rollback() }()

	// 1. Insert Event Log
	insertSQL := `INSERT INTO rf_events (code, bits, protocol, pulse, gateway, matched_rule, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)`
	if _, err := tx.Exec(insertSQL, code, bits, proto, pulse, gateway, ruleName, ts); err != nil {
		return 0, err
	}

	// 2. Upsert Aggregate Counter
	upsertSQL := `
	INSERT INTO rf_aggregates (code, total_count, first_seen, last_seen)
	VALUES (?, 1, ?, ?)
	ON CONFLICT(code) DO UPDATE SET
		total_count = total_count + 1,
		last_seen = excluded.last_seen;
	`
	if _, err := tx.Exec(upsertSQL, code, ts, ts); err != nil {
		return 0, err
	}

	// 3. Query updated total count
	var newCount int64
	if err := tx.QueryRow(`SELECT total_count FROM rf_aggregates WHERE code = ?`, code).Scan(&newCount); err != nil {
		return 0, err
	}

	if err := tx.Commit(); err != nil {
		return 0, err
	}
	return newCount, nil
}

// GetRFCounts returns recorded RF codes with their total counts and timestamps.
func (d *DB) GetRFCounts() ([]RFAggregate, error) {
	return d.GetFilteredRFCounts(0, 0)
}

// GetFilteredRFCounts returns RF aggregates optionally filtered by minimum hits and limited.
func (d *DB) GetFilteredRFCounts(minHits int, limit int) ([]RFAggregate, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	query := `SELECT code, total_count, first_seen, last_seen FROM rf_aggregates`
	var args []interface{}

	if minHits > 0 {
		query += ` WHERE total_count >= ?`
		args = append(args, minHits)
	}

	query += ` ORDER BY total_count DESC`

	if limit > 0 {
		query += ` LIMIT ?`
		args = append(args, limit)
	}

	rows, err := d.db.Query(query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []RFAggregate
	for rows.Next() {
		var a RFAggregate
		if err := rows.Scan(&a.Code, &a.TotalCount, &a.FirstSeen, &a.LastSeen); err == nil {
			list = append(list, a)
		}
	}
	return list, nil
}

// PruneRFEphemeralNoise deletes noise codes from rf_aggregates with total_count < minHits that are not in preserveCodes.
func (d *DB) PruneRFEphemeralNoise(minHits int, preserveCodes []uint32) (int64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	if minHits <= 1 {
		minHits = 2
	}

	query := `DELETE FROM rf_aggregates WHERE total_count < ?`
	var args []interface{}
	args = append(args, minHits)

	if len(preserveCodes) > 0 {
		placeholders := make([]string, len(preserveCodes))
		for i, c := range preserveCodes {
			placeholders[i] = "?"
			args = append(args, c)
		}
		query += fmt.Sprintf(` AND code NOT IN (%s)`, strings.Join(placeholders, ","))
	}

	res, err := d.db.Exec(query, args...)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

// EnforceRFEventRetention deletes older records from rf_events, keeping at most maxRows.
func (d *DB) EnforceRFEventRetention(maxRows int) (int64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	if maxRows <= 0 {
		maxRows = 1000
	}

	res, err := d.db.Exec(`
		DELETE FROM rf_events 
		WHERE id NOT IN (
			SELECT id FROM rf_events ORDER BY created_at DESC LIMIT ?
		)
	`, maxRows)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

// ClearAllRFEvents truncates the rf_events table.
func (d *DB) ClearAllRFEvents() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	_, err := d.db.Exec(`DELETE FROM rf_events`)
	return err
}

// GetRFCodeCount returns the total count and metadata for a specific RF code.
func (d *DB) GetRFCodeCount(code uint32) (RFAggregate, bool) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	var a RFAggregate
	err := d.db.QueryRow(`SELECT code, total_count, first_seen, last_seen FROM rf_aggregates WHERE code = ?`, code).
		Scan(&a.Code, &a.TotalCount, &a.FirstSeen, &a.LastSeen)
	if err != nil {
		return a, false
	}
	return a, true
}

// GetRFHistory returns recent signal receptions, optionally filtered by code.
func (d *DB) GetRFHistory(code uint32, limit int) ([]RFEventRecord, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	if limit <= 0 || limit > 500 {
		limit = 100
	}

	var rows *sql.Rows
	var err error

	if code > 0 {
		rows, err = d.db.Query(`
			SELECT id, code, bits, protocol, pulse, gateway, matched_rule, created_at
			FROM rf_events WHERE code = ? ORDER BY created_at DESC LIMIT ?`, code, limit)
	} else {
		rows, err = d.db.Query(`
			SELECT id, code, bits, protocol, pulse, gateway, matched_rule, created_at
			FROM rf_events ORDER BY created_at DESC LIMIT ?`, limit)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []RFEventRecord
	for rows.Next() {
		var r RFEventRecord
		if err := rows.Scan(&r.ID, &r.Code, &r.Bits, &r.Protocol, &r.Pulse, &r.Gateway, &r.MatchedRule, &r.CreatedAt); err == nil {
			list = append(list, r)
		}
	}
	return list, nil
}

// ──────────────────────────────────────────
// Multi-Instance State Synchronization
// ──────────────────────────────────────────

// SetEntityState persists the state of an entity and records timestamp and source.
func (d *DB) SetEntityState(entityID, state, source, attributes string) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	now := time.Now()
	query := `
	INSERT INTO entity_states (entity_id, state, updated_at, source, attributes)
	VALUES (?, ?, ?, ?, ?)
	ON CONFLICT(entity_id) DO UPDATE SET
		state = excluded.state,
		updated_at = excluded.updated_at,
		source = excluded.source,
		attributes = excluded.attributes;
	`
	_, err := d.db.Exec(query, entityID, state, now, source, attributes)
	return err
}

// GetEntityStates returns all currently persisted entity states.
func (d *DB) GetEntityStates() (map[string]EntityState, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	rows, err := d.db.Query(`SELECT entity_id, state, updated_at, source, attributes FROM entity_states`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	res := make(map[string]EntityState)
	for rows.Next() {
		var s EntityState
		if err := rows.Scan(&s.EntityID, &s.State, &s.UpdatedAt, &s.Source, &s.Attributes); err == nil {
			res[s.EntityID] = s
		}
	}
	return res, nil
}

// ──────────────────────────────────────────
// Device Ping Telemetry Operations
// ──────────────────────────────────────────

// RecordPing logs a device health ping measurement.
func (d *DB) RecordPing(deviceID string, latencyMs float64, status, ip string) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	now := time.Now()
	_, err := d.db.Exec(`INSERT INTO ping_history (device_id, latency_ms, status, ip, created_at) VALUES (?, ?, ?, ?, ?)`,
		deviceID, latencyMs, status, ip, now)
	return err
}

// GetPingHistory returns ping measurements over the specified recent hour span.
func (d *DB) GetPingHistory(hours int) ([]PingRecord, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	if hours <= 0 {
		hours = 24
	}
	since := time.Now().Add(-time.Duration(hours) * time.Hour)

	rows, err := d.db.Query(`SELECT id, device_id, latency_ms, status, ip, created_at FROM ping_history WHERE created_at >= ? ORDER BY created_at ASC`, since)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []PingRecord
	for rows.Next() {
		var r PingRecord
		if err := rows.Scan(&r.ID, &r.DeviceID, &r.LatencyMs, &r.Status, &r.IP, &r.CreatedAt); err == nil {
			list = append(list, r)
		}
	}
	return list, nil
}

// ──────────────────────────────────────────
// System Metrics & Performance Logs
// ──────────────────────────────────────────

// RecordSystemMetrics logs host CPU and RAM telemetry.
func (d *DB) RecordSystemMetrics(cpuPct, memUsedMB, memTotalMB float64, goroutines int) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	now := time.Now()
	_, err := d.db.Exec(`INSERT INTO system_metrics (cpu_pct, mem_used_mb, mem_total_mb, goroutines, created_at) VALUES (?, ?, ?, ?, ?)`,
		cpuPct, memUsedMB, memTotalMB, goroutines, now)
	return err
}

// GetSystemMetrics returns recent system performance telemetry.
func (d *DB) GetSystemMetrics(hours int) ([]SystemMetricRecord, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	if hours <= 0 {
		hours = 24
	}
	since := time.Now().Add(-time.Duration(hours) * time.Hour)

	rows, err := d.db.Query(`SELECT id, cpu_pct, mem_used_mb, mem_total_mb, goroutines, created_at FROM system_metrics WHERE created_at >= ? ORDER BY created_at ASC`, since)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []SystemMetricRecord
	for rows.Next() {
		var r SystemMetricRecord
		if err := rows.Scan(&r.ID, &r.CPUPct, &r.MemUsedMB, &r.MemTotalMB, &r.Goroutines, &r.CreatedAt); err == nil {
			list = append(list, r)
		}
	}
	return list, nil
}

// ──────────────────────────────────────────
// Speedtest Logs
// ──────────────────────────────────────────

// RecordSpeedtest saves a completed network speedtest result.
func (d *DB) RecordSpeedtest(downloadMbps, uploadMbps, pingMs float64, server string) error {
	d.mu.Lock()
	defer d.mu.Unlock()

	now := time.Now()
	_, err := d.db.Exec(`INSERT INTO speedtest_results (download_mbps, upload_mbps, ping_ms, server, created_at) VALUES (?, ?, ?, ?, ?)`,
		downloadMbps, uploadMbps, pingMs, server, now)
	return err
}

// GetSpeedtestHistory returns recent speedtest benchmark runs.
func (d *DB) GetSpeedtestHistory(limit int) ([]SpeedtestRecord, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()

	if limit <= 0 || limit > 100 {
		limit = 30
	}

	rows, err := d.db.Query(`SELECT id, download_mbps, upload_mbps, ping_ms, server, created_at FROM speedtest_results ORDER BY created_at DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var list []SpeedtestRecord
	for rows.Next() {
		var r SpeedtestRecord
		if err := rows.Scan(&r.ID, &r.DownloadMbps, &r.UploadMbps, &r.PingMs, &r.Server, &r.CreatedAt); err == nil {
			list = append(list, r)
		}
	}
	return list, nil
}

// ──────────────────────────────────────────
// Automatic Storage Pruning
// ──────────────────────────────────────────

func (d *DB) pruneLoop() {
	ticker := time.NewTicker(6 * time.Hour)
	defer ticker.Stop()

	for range ticker.C {
		d.PruneOldRecords(30) // Keep last 30 days of telemetry
	}
}

// PruneOldRecords deletes historical records older than daysToKeep to save space on Raspberry Pi SD cards.
func (d *DB) PruneOldRecords(daysToKeep int) {
	d.mu.Lock()
	defer d.mu.Unlock()

	if daysToKeep <= 0 {
		daysToKeep = 30
	}
	cutoff := time.Now().Add(-time.Duration(daysToKeep) * 24 * time.Hour)

	_, _ = d.db.Exec(`DELETE FROM rf_events WHERE created_at < ?`, cutoff)
	_, _ = d.db.Exec(`DELETE FROM ping_history WHERE created_at < ?`, cutoff)
	_, _ = d.db.Exec(`DELETE FROM system_metrics WHERE created_at < ?`, cutoff)
	_, _ = d.db.Exec(`DELETE FROM speedtest_results WHERE created_at < ?`, cutoff)
}
