# GitHub Copilot Instructions — Sentinel

## What This Project Is
Security compliance agent (C++17) with a Go aggregation backend. Portfolio project demonstrating: osquery + Lua policy evaluation, SQLite retry queue with at-least-once delivery, SHA-256 content-addressable deduplication, concurrent ingestion via Go goroutines, Prometheus observability, and Docker Compose orchestration. See `docs/roadmap/IMPLEMENTATION_PLAN.md` for C++ delivery status; `docs/roadmap/EXPANSION_PLAN.md` for Phase 3+ roadmap.

---

## Code Conventions

**Naming**: Classes `PascalCase`, methods/variables/files `snake_case`, constants `UPPER_SNAKE_CASE`

**C++ patterns to follow**:
- `std::unique_ptr` + `std::make_unique` for ownership
- RAII for all resources (DB, file handles)
- `sqlite3_finalize(stmt)` before any throw
- NULL safety: `const char* p = sqlite3_column_text(...); std::string v = p ? p : "";`
- Cross-platform guards: `#ifdef _WIN32` for `gmtime_s` / `gethostname`

**Error messages**: specific, not generic. Include function name + `sqlite3_errmsg()`.

**Comments**: explain *why*, not *what*. Document state machine transitions, resource bounds, and defensive checks.

---

## Project Structure
```
src/             # Core C++ implementation (main.cpp orchestrates)
backend/         # FastAPI prototype backend (server.py) — kept for reference
go-aggregator/   # Go aggregation service (Phase 3) — goroutines, Prometheus, SQLite
load-sim/        # Go load simulation harness (Phase 4) — benchmark tool
docs/roadmap/    # Phase plans with ✅/⏳/❌ status
test_*.cpp       # Integration tests at root level
scripts/         # PowerShell build/run/test automation
policies/        # Sample policy JSON files
prometheus/      # Prometheus scrape config (docker-compose)
mosquitto/       # Mosquitto broker config (docker-compose, Phase 3.5)
docker-compose.yml  # One-command demo: aggregator + Prometheus + Grafana (+ Mosquitto in 3.5)
```

---

## Quality Gates (required before marking anything done)

**C++ changes:**
- All tests pass: `.\scripts\test.ps1`
- Smoke test: `.\scripts\smoketest.ps1`
- Zero MSVC `/W4` warnings
- `docs/roadmap/IMPLEMENTATION_PLAN.md` status updated

**Go changes:**
- `go build ./...` — zero errors
- `go test ./...` — all tests pass
- `go vet ./...` — zero warnings

**Any change:**
- `README.md` updated if capabilities or structure changed
- Relevant roadmap doc (`PHASE3_PLAN.md`, `MQTT_PLAN.md`, etc.) status updated

---

## Testing Rules
- Use temporary DBs, always `remove(test_db)` in setup AND cleanup
- Dynamic timestamps — no hardcoded future dates
- Test both happy path and error paths (UNIQUE violations, NULL columns)
- Format: `std::cout << "[PASS] Description\n";`

---

## Architecture: Follow Existing Patterns
Before implementing anything new, read the closest existing example:
- New delivery client → read `MockDeliveryClient` first
- New schema → read existing `init_schema()` in `db.cpp`
- New policy rule → read an existing entry in `policies/`

Persist intent before side effects. DB write → network call → DB confirm. Always.

---

## Go Conventions (go-aggregator/, load-sim/)

**Naming**: Packages `lowercase`, exported types/funcs `PascalCase`, unexported `camelCase`, files `snake_case.go`

**Go patterns to follow:**
- Return `(T, error)` — never panic on recoverable errors
- Use `context.Context` for cancellation on all blocking operations (DB writes, MQTT, HTTP)
- `defer rows.Close()` / `defer stmt.Close()` immediately after successful open
- Channel-based worker pools: buffer size = worker count; full channel = backpressure (return 503)
- `sync.RWMutex` for shared state (dedup cache); hold read lock for reads, write lock only for writes
- Structured logging with `log/slog` (Go 1.21+): `slog.Info("stored report", "hash", hash, "latency_ms", ms)`

**Error messages**: wrap with context — `fmt.Errorf("storage.Write: %w", err)`, not `"error"`

**Testing:**
- Use `t.TempDir()` for temporary SQLite databases — auto-cleaned after test
- Table-driven tests with `t.Run(name, func(t *testing.T) {...})`
- Test both happy path and error paths (dedup hit, backpressure, malformed JSON)
- Format: `t.Logf("[PASS] %s", description)` — consistent with C++ test style

**Comments**: explain *why*, not *what*. Document concurrency contracts, channel semantics, and backpressure behavior.

---

## Anti-Patterns

**C++:**
❌ `throw std::runtime_error("database error")` — be specific
❌ `sqlite3_finalize` after throw — finalize before
❌ Skipping NULL checks on `sqlite3_column_text`
❌ Adding dependencies when a standalone implementation suffices
❌ Claiming done without running test + smoke scripts
❌ Committing without updating the roadmap status

**Go:**
❌ `panic(err)` for recoverable errors — return `(T, error)`
❌ Unbounded goroutine spawning — always use a worker pool with a fixed channel buffer
❌ Ignoring `context.Context` cancellation — all blocking calls must respect ctx
❌ Global mutable state without a mutex — make concurrency ownership explicit
❌ `log.Fatalf` inside library code — only in `main.go`

---

## Git
- Branches: `feature/description` or `fix/description`
- Commit: imperative subject, bullet body listing what changed, which files, which phase updated
- Merge only: tests pass + docs updated + no warnings