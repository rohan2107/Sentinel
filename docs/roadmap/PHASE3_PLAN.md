# Phase 3 Implementation Plan: Go Aggregation Service + Docker Compose

> **Status:** ⏳ PLANNED — starting after Phase 2 docs merge
> **Date:** February 27, 2026
> **Depends on:** Phase 1 + Phase 2 complete ✅

---

## Goal

Replace the demo-grade FastAPI backend with a production-thinking Go service that:
- Handles N concurrent agents via goroutines
- Deduplicates reports via in-memory LRU cache + SQLite
- Exposes a real Prometheus `/metrics` endpoint with latency histograms
- Ships as a Docker image, bundled with Prometheus via Docker Compose

The C++ agent needs **zero changes** — the Go aggregator exposes the same `POST /reports` contract.

---

## Decisions Made

| Question | Decision | Rationale |
|----------|----------|-----------|
| Storage backend | SQLite first | Consistent with agent; single binary; "swap to Postgres when horizontal scale demanded" is a concrete interview talking point |
| Docker Compose | Yes | One-command demo: `docker-compose up`; bundles aggregator + Prometheus + optional Grafana |
| MQTT | Phase 3.5 (after this) | Not abandoned — see `MQTT_PLAN.md` |

---

## Directory Structure

```
go-aggregator/
├── cmd/
│   └── aggregator/
│       └── main.go           # HTTP server entrypoint, flag parsing, graceful shutdown
├── internal/
│   ├── handler/
│   │   └── reports.go        # POST /reports — dedup check → store → metrics
│   ├── dedup/
│   │   └── seen_cache.go     # In-memory LRU cache (recent hashes, fast path)
│   ├── storage/
│   │   └── sqlite.go         # SQLite writer via bounded worker pool
│   └── metrics/
│       └── prometheus.go     # Register + update custom Prometheus metrics
├── go.mod
├── go.sum
├── Dockerfile
└── README.md
```

---

## Implementation Steps

### Step 1 — Go module scaffold

```bash
mkdir go-aggregator && cd go-aggregator
go mod init github.com/rohan2107/sentinel/go-aggregator
```

**Dependencies:**
```
github.com/prometheus/client_golang   # Prometheus metrics
github.com/hashicorp/golang-lru/v2    # LRU cache
github.com/mattn/go-sqlite3           # SQLite driver (CGO)
```

**`cmd/aggregator/main.go`** responsibilities:
- Parse `--port` (default 8080), `--db-path` (default `aggregator.db`), `--cache-size` (default 1000), `--workers` (default 4)
- Initialize storage → dedup cache → metrics → HTTP mux
- Register `/reports` (POST) and `/metrics` (GET) and `/health` (GET)
- Block on `http.ListenAndServe`, handle SIGINT/SIGTERM with `context` + graceful shutdown

---

### Step 2 — POST /reports handler

**File:** `internal/handler/reports.go`

Request body (same contract as FastAPI backend — no agent changes needed):
```json
{ "report": { ... }, "hash": "abc123..." }
```

Handler logic:
1. Decode JSON body; reject malformed requests with 400
2. Verify hash: compute SHA-256 of `json.Marshal(sorted report keys)`, compare to `body.Hash`
3. Check in-memory LRU cache — if hit → return 409 (duplicate)
4. Check DB for hash — if found → add to LRU cache → return 409
5. Submit write job to worker pool channel; if channel full → return 503 + `Retry-After: 1`
6. On success → add hash to LRU cache → record metrics → return 200

**Hash verification in Go:**
```go
// Sort report keys by marshaling via json.Marshal with a sorted map
canonical, _ := json.Marshal(report)         // Go maps are iterated in sorted key order since Go 1.12
hash := fmt.Sprintf("%x", sha256.Sum256(canonical))
```

> Note: nlohmann/json (C++) and Go's `encoding/json` both produce sorted-key output for objects,
> so canonical hashes will match. Verify this in integration test.

---

### Step 3 — In-memory LRU dedup cache

**File:** `internal/dedup/seen_cache.go`

```go
type SeenCache struct {
    cache *lru.Cache[string, struct{}]
    mu    sync.RWMutex
}

func (c *SeenCache) Contains(hash string) bool { ... }
func (c *SeenCache) Add(hash string)            { ... }
```

- Cache size: configurable (default 1000 hashes ≈ ~64KB)
- LRU eviction means hot duplicates hit fast path; cold duplicates fall through to DB
- Thread-safe: `sync.RWMutex` wrapping lru.Cache operations

---

### Step 4 — SQLite storage with bounded worker pool

**File:** `internal/storage/sqlite.go`

```go
type Store struct {
    db      *sql.DB
    jobChan chan writeJob
}

type writeJob struct {
    hash   string
    report string
    result chan error
}

func (s *Store) Start(workers int)           // spawns N goroutines reading from jobChan
func (s *Store) Write(hash, report string) error  // sends to jobChan, blocks on result
func (s *Store) Contains(hash string) bool   // read-only, can run outside pool
```

**Schema:**
```sql
CREATE TABLE IF NOT EXISTS received_reports (
    report_hash TEXT PRIMARY KEY,
    report_json TEXT NOT NULL,
    received_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_received_hash ON received_reports(report_hash);
```

**Why a bounded worker pool:**
SQLite supports one writer at a time. Funneling writes through N workers (not N goroutines) prevents lock contention and gives clean backpressure: when the channel is full, the handler returns 503 and the agent's exponential backoff handles the retry.

---

### Step 5 — Prometheus metrics

**File:** `internal/metrics/prometheus.go`

| Metric | Type | Labels |
|--------|------|--------|
| `sentinel_reports_received_total` | Counter | — |
| `sentinel_reports_deduplicated_total` | Counter | `source` (cache/db) |
| `sentinel_reports_stored_total` | Counter | — |
| `sentinel_reports_failed_total` | Counter | `reason` (hash_mismatch/backpressure) |
| `sentinel_report_latency_seconds` | Histogram | — |
| `sentinel_dedup_cache_hit_ratio` | Gauge | — |
| `sentinel_storage_queue_depth` | Gauge | — |
| `sentinel_active_connections` | Gauge | — |

Expose at `GET /metrics` using `promhttp.Handler()`.

---

### Step 6 — Dockerfile (multi-stage)

**File:** `go-aggregator/Dockerfile`

```dockerfile
# Stage 1: Build
FROM golang:1.23-alpine AS builder
RUN apk add --no-cache gcc musl-dev sqlite-dev
WORKDIR /build
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=1 GOOS=linux go build -o aggregator ./cmd/aggregator

# Stage 2: Run
FROM alpine:3.20
RUN apk add --no-cache sqlite-libs ca-certificates
WORKDIR /app
COPY --from=builder /build/aggregator .
EXPOSE 8080
ENTRYPOINT ["./aggregator"]
```

> CGO is required for `go-sqlite3`. Alpine provides `sqlite-libs` for the runtime.

---

### Step 7 — Docker Compose

**File:** `docker-compose.yml` (project root)

```yaml
services:
  aggregator:
    build: ./go-aggregator
    ports:
      - "8080:8080"
    volumes:
      - aggregator_data:/app/data
    environment:
      - DB_PATH=/app/data/aggregator.db
    restart: unless-stopped

  prometheus:
    image: prom/prometheus:latest
    ports:
      - "9090:9090"
    volumes:
      - ./prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro
    depends_on:
      - aggregator

  grafana:
    image: grafana/grafana:latest
    ports:
      - "3000:3000"
    environment:
      - GF_AUTH_ANONYMOUS_ENABLED=true
      - GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer
    volumes:
      - grafana_data:/var/lib/grafana
    depends_on:
      - prometheus

volumes:
  aggregator_data:
  grafana_data:
```

**File:** `prometheus/prometheus.yml`

```yaml
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: sentinel-aggregator
    static_configs:
      - targets: ["aggregator:8080"]
```

**Usage:**
```bash
# Start full backend stack
docker-compose up

# Run agent against it (on Windows host)
.\build\Release\Sentinel.exe --enable-delivery --backend-url http://localhost:8080
```

---

## Verification Checklist

- [ ] `go build ./...` in `go-aggregator/` — zero errors, zero warnings
- [ ] `go test ./...` — unit tests for handler, dedup cache, storage
- [ ] Run agent with `--enable-delivery --backend-url http://localhost:8080` — delivery succeeds
- [ ] `curl http://localhost:8080/health` → 200
- [ ] `curl http://localhost:8080/metrics` → Prometheus text format
- [ ] `docker-compose up` — all 3 services start cleanly
- [ ] Prometheus scrapes aggregator: `http://localhost:9090/targets` shows UP
- [ ] Grafana datasource auto-configured: `http://localhost:3000`
- [ ] Duplicate report returns 409 from aggregator (same behavior as FastAPI backend)
- [ ] 503 under backpressure: fill worker pool, verify Retry-After header

---

## Quality Gates (from copilot-instructions.md)

- All tests pass: `go test ./...`
- `docs/roadmap/IMPLEMENTATION_PLAN.md` updated with Phase 3 ✅
- `README.md` updated: Go aggregator in project structure + Docker Compose quick-start
- `.github/copilot-instructions.md` updated with Go conventions

---

## Interview Talking Points Unlocked

> "I replaced a demo Python backend with a Go service. It uses goroutines for concurrent ingestion from N agents, a bounded worker pool to prevent SQLite lock contention — when the pool fills, the aggregator returns 503 with a Retry-After header and the agents' exponential backoff handles the rest. Deduplication happens in two layers: an in-memory LRU cache for the hot path, and the database as source of truth. The whole stack spins up with `docker-compose up` — Prometheus scrapes the `/metrics` endpoint every 15 seconds. The C++ agent needed zero changes because the Go aggregator is contract-compatible."
