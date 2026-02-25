# Expansion Plan: Go Aggregation Service + Load Simulation

> **Status:** Planning  
> **Date:** February 25, 2026  
> **Goal:** Make Sentinel's architecture demonstrably scalable — not just "it works" but "here's how it performs under load"

---

## Motivation

The current Sentinel agent (C++) proves the per-agent delivery model works: osquery data collection, Lua policy evaluation, SQLite retry queue, SHA-256 dedup, HTTP delivery to a FastAPI backend. But the backend is a demo-grade single-process Python server.

To demonstrate production systems thinking (and back up claims like "10k+ endpoints"), the project needs:
1. A backend that can handle concurrent ingestion from N agents
2. Load simulation proving it works at scale
3. Observable metrics (latency percentiles, throughput, dedup hit rate)

---

## Target Architecture

```
[N Sentinel agents (C++)]
    → MQTT / HTTP (at-least-once, SHA-256 dedup)
        → [Go aggregation service]
            → dedup by hash (seen-cache or DB)
            → fan-out / storage
            → metrics endpoint (Prometheus)
                → load simulation harness
```

---

## Phase 3: Go Aggregation Service

### Why Go?

- **Goroutine model** earns its place with concurrent ingestion from N agents
- **Low latency**: No GC pauses comparable to Java; suitable for p99 targets
- **Simple deployment**: Single static binary
- **Native HTTP/gRPC**: stdlib is production-quality
- **Prometheus client**: First-class Go library

### Core Requirements

| Requirement | Detail |
|-------------|--------|
| **Concurrent ingestion** | Handle N agents POSTing simultaneously via goroutines |
| **Idempotent processing** | Same hash from multiple agents → store once |
| **Backpressure** | What happens when storage is slow and agents are fast |
| **Observability** | p50/p95/p99 latency, throughput, dedup hit rate as real Prometheus metrics |

### Planned Components

```
go-aggregator/
├── cmd/
│   └── aggregator/
│       └── main.go           # Entry point, HTTP server
├── internal/
│   ├── handler/
│   │   └── reports.go        # POST /reports handler
│   ├── dedup/
│   │   └── seen_cache.go     # In-memory LRU + DB dedup
│   ├── storage/
│   │   └── sqlite.go         # SQLite storage (swap to Postgres later)
│   └── metrics/
│       └── prometheus.go     # Custom metrics registration
├── go.mod
├── go.sum
├── Dockerfile
└── README.md
```

### Key Design Decisions

1. **In-memory seen-cache + DB**: LRU cache for recent hashes (fast dedup), DB as source of truth
2. **Bounded worker pool**: Limit concurrent DB writes to prevent SQLite lock contention
3. **Graceful degradation**: When storage is slow, return 503 with Retry-After header; agents' exponential backoff handles the rest
4. **Structured logging**: JSON logs with request ID, agent hostname, hash

### Metrics to Expose (`/metrics` Prometheus endpoint)

| Metric | Type | Description |
|--------|------|-------------|
| `sentinel_reports_received_total` | Counter | Total reports received |
| `sentinel_reports_deduplicated_total` | Counter | Reports rejected as duplicates |
| `sentinel_reports_stored_total` | Counter | Reports successfully stored |
| `sentinel_report_latency_seconds` | Histogram | Request latency (p50/p95/p99) |
| `sentinel_dedup_cache_hit_ratio` | Gauge | In-memory cache hit rate |
| `sentinel_storage_queue_depth` | Gauge | Pending writes to storage |
| `sentinel_active_connections` | Gauge | Current concurrent agent connections |

---

## Phase 4: Load Simulation Harness

### Purpose

A Go tool that spins up N goroutines, each mimicking a Sentinel agent — generating reports, hashing them, delivering with retry semantics. Produces reproducible benchmarks.

### Planned Structure

```
load-sim/
├── cmd/
│   └── loadsim/
│       └── main.go           # CLI entry, orchestrator
├── internal/
│   ├── agent/
│   │   └── simulated.go      # Simulated Sentinel agent
│   ├── report/
│   │   └── generator.go      # Random report generation + SHA-256
│   └── stats/
│       └── collector.go      # Latency percentiles, throughput
├── go.mod
└── README.md
```

### Configurable Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--agents` | 100 | Number of simulated agents (goroutines) |
| `--rate` | 1/min | Report rate per agent |
| `--duration` | 5m | Test duration |
| `--backend-url` | localhost:8080 | Aggregator URL |
| `--jitter` | 100ms | Simulated network jitter |
| `--duplicate-rate` | 0.1 | Fraction of intentional duplicate reports |
| `--failure-rate` | 0.05 | Simulated delivery failures |

### Output

```
=== Load Test Results ===
Agents:           100
Duration:         5m0s
Reports sent:     500
Reports accepted: 450
Duplicates:       50 (10.0%)
Failures:         12 (2.4% - all retried)

Latency (ms):
  p50:   12
  p95:   45
  p99:   120
  max:   350

Throughput:       1.67 reports/sec
Backend CPU:      15% avg
Dedup cache hit:  92%
```

This output (or a Grafana screenshot) goes in the README and gets talked through in interviews.

---

## Phase 5: Showcase & Documentation

### README Update

Add "Performance at Scale" section with:
- Architecture diagram (agents → aggregator → storage)
- Load test results at 100 / 1000 agents
- Latency percentile charts (or table)
- Dedup efficiency metrics
- Backpressure behavior description

### What This Does for the CV

The current state has working at-least-once delivery from a single agent. After this expansion:
- "10k+ endpoints" becomes demonstrable with load sim results
- Architecture diagram shows production-grade thinking
- Prometheus metrics show observability skills
- Go service shows polyglot engineering (C++ agent + Go backend + Python prototype)
- Graceful degradation under load shows systems thinking

---

## Estimated Timeline

| Phase | Effort | Dependency |
|-------|--------|------------|
| Go aggregator (basic) | 2-3 days | None |
| Prometheus metrics | 1 day | Aggregator |
| Load sim tool | 1-2 days | Aggregator |
| Backpressure handling | 1 day | Load sim (to test) |
| Documentation + README | 0.5 day | Load sim results |
| **Total** | **~6-8 days** | |

---

## Open Questions

1. **Storage backend**: Start with SQLite (consistency with agent), migrate to Postgres when multi-agent concurrency demands it?
2. **MQTT vs HTTP**: Keep HTTP for simplicity, or add MQTT broker to demonstrate pub/sub pattern?
3. **Grafana dashboard**: Worth setting up, or Prometheus `/metrics` endpoint + screenshots sufficient?
4. **Docker Compose**: Bundle agent + aggregator + Prometheus + Grafana for one-command demo?
