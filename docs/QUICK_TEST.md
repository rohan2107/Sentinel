# Quick Test Reference

## C++ Agent (before every commit touching `src/`)

```powershell
# One-liner validation
.\scripts\build.ps1; .\scripts\test.ps1; .\scripts\run.ps1
```

## Go Aggregator (before every commit touching `go-aggregator/`)

```powershell
# From repo root — build + vet + test (race detector on)
.\go-aggregator\scripts\test.ps1
```

Or manually:
```powershell
cd go-aggregator
go build ./...
go vet ./...
go test -race -count=1 -v ./...
```

---

## Expected Output

### Build
```
✓ Build succeeded (config: Debug)
```

### Tests  
```
===================================================
  ALL TESTS PASSED
===================================================
```

### Runtime
```
✓ Sentinel completed successfully
```

---

---

## Go Aggregator Expected Output

```
=== RUN   TestHandleReports
    [PASS] valid report accepted → HTTP 200
    [PASS] missing hash rejected → HTTP 400
    ...
--- PASS: TestHandleReports (0.00s)
PASS
===================================================
  ALL CHECKS PASSED
===================================================
```

---

## If Anything Fails

1. **Build fails** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#build-fails)
2. **Tests fail** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#tests-fail)
3. **Runtime fails** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#runtime-issues)

---

## Manual Test Checklist

### C++ Agent
Before pushing to GitHub:

- [ ] Clean build passes
- [ ] All integration tests pass
- [ ] Phase 1 functionality works (backward compatible)
- [ ] No CLI warnings when running
- [ ] Database file exists and updated: `(Get-Item sentinel_data.sqlite3).LastWriteTime`

### Go Aggregator
- [ ] `go build ./...` — zero errors
- [ ] `go vet ./...` — zero warnings
- [ ] `go test -race -count=1 ./...` — all pass
- [ ] `go mod tidy` — no diff in go.mod/go.sum

**C++ time: ~5 minutes | Go time: ~1 minute**

---

## CLI Variations to Test

```powershell
# All should work without warnings:
.\build\Debug\Sentinel.exe
.\build\Debug\Sentinel.exe policies\sample_policy.json
.\build\Debug\Sentinel.exe --policy policies\sample_policy.json
```

---

## Full Testing Guide

See [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md) for comprehensive 10-15 minute validation.
