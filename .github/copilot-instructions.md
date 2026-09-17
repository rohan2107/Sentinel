# GitHub Copilot Instructions — Sentinel

## What This Project Is
Security compliance agent (C++17) with two independent receivers. Portfolio project: osquery + Lua policy evaluation, SQLite retry queue with at-least-once delivery, state-change-triggered reporting via a posture hash, and a cross-language report-hash contract.

Read [`architecture/README.md`](../architecture/README.md) before changing anything — in particular its **Known gaps** section, which lists what the code does *not* do. Do not restore claims it removed. [`docs/ROADMAP.md`](../docs/ROADMAP.md) has the plan.

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
src/             # C++ agent (main.cpp orchestrates)
backend/         # FastAPI receiver; canonical.py holds the hash contract
go-aggregator/   # Go receiver — verifies + dedups, but does NOT store yet
tests/           # All tests: two C++ binaries + canonicalization/
scripts/         # build/run/test/smoketest, .ps1 and .sh
policies/        # Per-platform policies (not portable across OSes)
architecture/    # How it works and what it does not do
docs/            # ROADMAP.md, trade-offs.md
```
There is no `load-sim/`, `prometheus/`, `mosquitto/` or `docker-compose.yml`.
Those are roadmap items, not current structure.

---

## Quality Gates (required before marking anything done)

**C++ changes:**
- All tests pass: `.\scripts\test.ps1` (Windows) or `./scripts/test.sh --config Both`
- Smoke test: `.\scripts\smoketest.ps1` or `./scripts/smoketest.sh`
- Zero MSVC `/W4` warnings

**Go changes:**
- `go build ./...` — zero errors
- `go test ./...` — all tests pass
- `go vet ./...` — zero warnings

**Anything touching the report hash or its JSON encoding:**
- `python3 tests/canonicalization/compare.py` passes. The C++, Python and Go
  encoders must agree byte-for-byte or every report is rejected as a hash
  mismatch. Review any `golden.json` change deliberately.

**Any change:**
- `README.md` and `architecture/README.md` updated if behaviour or structure
  changed. Claims must match code — the docs previously asserted a Lua timeout,
  enforced foreign keys and a scoring formula that none of the code implemented.

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

## Go Conventions (go-aggregator/)

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