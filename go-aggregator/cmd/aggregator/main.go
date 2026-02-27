package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/rohan2107/sentinel/go-aggregator/internal/handler"
)

func main() {
	port      := flag.Int("port", 8080, "HTTP listen port")
	dbPath    := flag.String("db-path", "aggregator.db", "Path to SQLite database")
	workers   := flag.Int("workers", 4, "Number of storage worker goroutines")
	cacheSize := flag.Int("cache-size", 1000, "LRU dedup cache size (number of hashes)")
	flag.Parse()

	// JSON structured logging so log lines are parseable by Prometheus/Grafana Loki.
	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(log)

	slog.Info("starting sentinel aggregator",
		"port", *port,
		"db_path", *dbPath,
		"workers", *workers,
		"cache_size", *cacheSize,
	)

	// Route registration uses Go 1.22 method+path patterns.
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", handleHealth)
	mux.Handle("POST /reports", handler.New())

	srv := &http.Server{
		Addr:         fmt.Sprintf(":%d", *port),
		Handler:      mux,
		ReadTimeout:  15 * time.Second,
		WriteTimeout: 15 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Start server in a background goroutine; main goroutine blocks on OS signal.
	// errCh is buffered so the goroutine never leaks if we exit via signal first.
	errCh := make(chan error, 1)
	go func() {
		slog.Info("listening", "addr", srv.Addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)

	select {
	case err := <-errCh:
		slog.Error("server error", "err", err)
		os.Exit(1)
	case sig := <-quit:
		slog.Info("shutdown signal received", "signal", sig.String())
	}

	// Give in-flight requests up to 5 seconds to complete before forceful close.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		slog.Error("graceful shutdown failed", "err", err)
		os.Exit(1)
	}
	slog.Info("shutdown complete")
}

func handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprint(w, `{"status":"ok"}`)
}
