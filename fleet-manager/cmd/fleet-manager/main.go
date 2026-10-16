package main

import (
	"context"
	"encoding/json"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleet-manager/pkg/scanner"
	"github.com/meteorinca/robot-fleet/fleet-manager/pkg/web"
)

func main() {
	cfg := scanner.DefaultConfig()
	command := "serve"
	if len(os.Args) > 1 && (os.Args[1] == "serve" || os.Args[1] == "scan") {
		command = os.Args[1]
		os.Args = append(os.Args[:1], os.Args[2:]...)
	}
	addr := flag.String("listen", "127.0.0.1:8080", "web listen address")
	platforms := flag.String("platforms", strings.Join(cfg.Platforms, ","), "comma-separated bot prefixes")
	flag.IntVar(&cfg.MaxDevice, "max-device", cfg.MaxDevice, "highest device number (1..9)")
	flag.IntVar(&cfg.Workers, "workers", cfg.Workers, "parallel probes (1..27)")
	flag.IntVar(&cfg.Port, "bot-port", cfg.Port, "bot HTTP port")
	flag.DurationVar(&cfg.Timeout, "timeout", cfg.Timeout, "total per-bot deadline")
	flag.DurationVar(&cfg.Interval, "interval", cfg.Interval, "scan interval")
	flag.StringVar(&cfg.StatePath, "state", cfg.StatePath, "persistent JSON inventory")
	flag.Parse()
	if flag.NArg() != 0 {
		log.Fatal("usage: fleet-manager [serve|scan] [flags]")
	}
	cfg.Platforms = strings.Split(*platforms, ",")
	for i := range cfg.Platforms {
		cfg.Platforms[i] = strings.TrimSpace(cfg.Platforms[i])
	}
	s, err := scanner.New(cfg)
	if err != nil {
		log.Fatal(err)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()
	if command == "scan" {
		s.RunOnce(ctx, func(err error) { log.Printf("inventory: %v", err) })
		if err := s.Save(); err != nil {
			log.Fatal(err)
		}
		if err := json.NewEncoder(os.Stdout).Encode(s.Snapshot()); err != nil {
			log.Fatal(err)
		}
		return
	}
	var wg sync.WaitGroup
	wg.Add(1)
	go func() { defer wg.Done(); s.Run(ctx, func(err error) { log.Printf("inventory: %v", err) }) }()
	server := &http.Server{Addr: *addr, Handler: web.Handler(s), ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 60 * time.Second}
	go func() {
		<-ctx.Done()
		shutdownCtx, done := context.WithTimeout(context.Background(), 5*time.Second)
		defer done()
		_ = server.Shutdown(shutdownCtx)
	}()
	log.Printf("Fleet Manager: http://%s", *addr)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		cancel()
		wg.Wait()
		log.Fatal(err)
	}
	cancel()
	wg.Wait()
}
