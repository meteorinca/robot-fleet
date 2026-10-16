package web

import (
	"embed"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/meteorinca/robot-fleet/fleet-manager/pkg/scanner"
)

//go:embed static/*
var assets embed.FS

func Handler(s *scanner.Scanner) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/fleet", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(s.Snapshot())
	})
	mux.HandleFunc("POST /api/scan", func(w http.ResponseWriter, r *http.Request) {
		if origin := r.Header.Get("Origin"); origin != "" && origin != "http://"+r.Host && origin != "https://"+r.Host {
			http.Error(w, "invalid origin", http.StatusForbidden)
			return
		}
		if !s.RequestScan() {
			http.Error(w, "scan already queued", http.StatusConflict)
			return
		}
		w.WriteHeader(http.StatusAccepted)
	})
	mux.HandleFunc("GET /api/events", func(w http.ResponseWriter, r *http.Request) {
		if _, ok := w.(http.Flusher); !ok {
			http.Error(w, "stream unavailable", 500)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Accel-Buffering", "no")
		ch, unsubscribe := s.Subscribe()
		defer unsubscribe()
		heartbeat := time.NewTicker(15 * time.Second)
		defer heartbeat.Stop()
		controller := http.NewResponseController(w)
		for {
			var payload string
			select {
			case <-r.Context().Done():
				return
			case snapshot := <-ch:
				data, _ := json.Marshal(snapshot)
				payload = fmt.Sprintf("event: fleet\ndata: %s\n\n", data)
			case <-heartbeat.C:
				payload = ": keepalive\n\n"
			}
			_ = controller.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if _, err := fmt.Fprint(w, payload); err != nil {
				return
			}
			if err := controller.Flush(); err != nil {
				return
			}
		}
	})
	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		data, _ := assets.ReadFile("static/index.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		_, _ = w.Write(data)
	})
	return mux
}
