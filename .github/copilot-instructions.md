# GitHub Copilot Instructions — Sentinel

## What This Project Is
Security compliance agent (C++17). Portfolio project demonstrating: osquery + Lua policy evaluation, SQLite retry queue with at-least-once delivery, SHA-256 content-addressable deduplication. See `docs/roadmap/IMPLEMENTATION_PLAN.md` for current status.

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
src/           # Core C++ implementation (main.cpp orchestrates)
backend/       # FastAPI backend (server.py, hash dedup)
docs/roadmap/  # Phase plans with ✅/⏳/❌ status
test_*.cpp     # Integration tests at root level
scripts/       # PowerShell build/run/test automation
policies/      # Sample policy JSON files
```

---

## Quality Gates (required before marking anything done)
- All tests pass: `.\scripts\test.ps1`
- Smoke test: `.\scripts\smoketest.ps1`
- Zero MSVC `/W4` warnings
- `docs/roadmap/IMPLEMENTATION_PLAN.md` status updated
- `README.md` updated if capabilities changed

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

## Anti-Patterns
❌ `throw std::runtime_error("database error")` — be specific  
❌ `sqlite3_finalize` after throw — finalize before  
❌ Skipping NULL checks on `sqlite3_column_text`  
❌ Adding dependencies when a standalone implementation suffices  
❌ Claiming done without running test + smoke scripts  
❌ Committing without updating the roadmap status

---

## Git
- Branches: `feature/description` or `fix/description`
- Commit: imperative subject, bullet body listing what changed, which files, which phase updated
- Merge only: tests pass + docs updated + no warnings