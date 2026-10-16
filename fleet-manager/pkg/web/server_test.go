package web

import (
	"bufio"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"github.com/meteorinca/robot-fleet/fleet-manager/pkg/scanner"
)

func TestInventoryAndScanAPI(t *testing.T) {
	cfg := scanner.DefaultConfig()
	cfg.StatePath = filepath.Join(t.TempDir(), "state.json")
	s, err := scanner.New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	handler := Handler(s)
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/api/fleet", nil))
	var snapshot scanner.Snapshot
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &snapshot) != nil || len(snapshot.Bots) != 27 {
		t.Fatal("invalid inventory response")
	}
	for _, status := range []int{http.StatusAccepted, http.StatusConflict} {
		w = httptest.NewRecorder()
		handler.ServeHTTP(w, httptest.NewRequest("POST", "/api/scan", nil))
		if w.Code != status {
			t.Fatalf("scan status %d, want %d", w.Code, status)
		}
	}
	w = httptest.NewRecorder()
	req := httptest.NewRequest("POST", "/api/scan", nil)
	req.Header.Set("Origin", "http://different.example")
	handler.ServeHTTP(w, req)
	if w.Code != http.StatusForbidden {
		t.Fatal("must reject a different browser origin")
	}
	w = httptest.NewRecorder()
	handler.ServeHTTP(w, httptest.NewRequest("GET", "/", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), "new EventSource('/api/events')") {
		t.Fatal("embedded dashboard missing")
	}
}

func TestSSEInitialInventory(t *testing.T) {
	cfg := scanner.DefaultConfig()
	cfg.StatePath = filepath.Join(t.TempDir(), "state.json")
	s, err := scanner.New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(Handler(s))
	defer server.Close()
	resp, err := server.Client().Get(server.URL + "/api/events")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	reader := bufio.NewReader(resp.Body)
	event, err := reader.ReadString('\n')
	if err != nil || event != "event: fleet\n" {
		t.Fatalf("invalid SSE event: %q %v", event, err)
	}
	data, err := reader.ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	var snapshot scanner.Snapshot
	if json.Unmarshal([]byte(strings.TrimPrefix(data, "data: ")), &snapshot) != nil || len(snapshot.Bots) != 27 {
		t.Fatal("invalid streamed inventory")
	}
}
