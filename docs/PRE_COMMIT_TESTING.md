# Pre-Commit Testing Guide

**Branch:** `feature/delivery-foundation`  
**Target:** Merge to `master`

## Quick Validation (5 minutes)

Run these commands in order. All must pass before commit.

```powershell
# 1. Clean build
.\scripts\build.ps1

# 2. Run automated tests
.\scripts\test.ps1

# 3. Run Phase 1 functionality (backward compatibility)
.\scripts\run.ps1
```

**Expected Results:**
- Build completes with no errors
- All 13 test assertions pass
- Sentinel runs successfully, generates report, persists to SQLite

---

## Detailed Manual Tests

### Test 1: Build Integrity

```powershell
# Clean build from scratch
Remove-Item -Recurse -Force .\build\*
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug
cmake --build build --config Release
```

**Pass Criteria:**
- [ ] Debug build succeeds
- [ ] Release build succeeds
- [ ] No compiler warnings in new code (db.cpp, report_hasher.cpp, delivery_client.cpp, test_delivery_foundation.cpp)

---

### Test 2: Automated Test Suite

```powershell
.\scripts\test.ps1
```

**Pass Criteria:**
- [ ] Hash determinism test passes (2 assertions)
- [ ] Mock client test passes (4 assertions)
- [ ] Retry queue test passes (9 assertions)
- [ ] End-to-end test passes (2 assertions)
- [ ] **Total: 13 assertions pass**
- [ ] No test database files left behind (test_sentinel.db, integration_test.db deleted)

**Spot Check:**
- Open output and verify: "ALL TESTS PASSED"
- Check summary lists all test categories

---

### Test 3: Phase 1 Backward Compatibility

Run existing functionality to ensure no regressions.

```powershell
# Test 3a: Default invocation
.\build\Debug\Sentinel.exe

# Test 3b: Positional argument
.\build\Debug\Sentinel.exe policies\sample_policy.json

# Test 3c: Flag-based argument
.\build\Debug\Sentinel.exe --policy policies\sample_policy.json

# Test 3d: Run script wrapper
.\scripts\run.ps1
```

**Pass Criteria:**
- [ ] All 4 invocations succeed (exit code 0)
- [ ] No "[warning] Unknown argument" appears
- [ ] Report written to `reports/latest_report.json`
- [ ] Database file exists and is updated: `(Get-Item sentinel_data.sqlite3).LastWriteTime`

**Verify Core Logic:**
```powershell
# Check report structure
Get-Content .\reports\latest_report.json | ConvertFrom-Json | Format-List

# Check database persistence (file was recently modified)
(Get-Item sentinel_data.sqlite3).LastWriteTime
```

- [ ] Report contains: timestamp, hostname, policy, score, details
- [ ] Database file timestamp matches recent run time

---

### Test 4: Delivery Foundation Schema (Optional)

Verify retry_queue table exists and is functional.

**Requires sqlite3 CLI:**
```powershell
# Install: winget install SQLite.SQLite
# Or download from: https://www.sqlite.org/download.html

sqlite3 sentinel_data.sqlite3 ".schema retry_queue"
```

**Alternative verification (without sqlite3):**
```powershell
# Database file exists and is non-empty
(Get-Item sentinel_data.sqlite3).Length -gt 0

# Test suite already validates schema (test_delivery_foundation.cpp)
.\build\Debug\test_delivery_foundation.exe
```

**Pass Criteria:**
- [ ] Database file exists and is non-empty
- [ ] Integration tests pass (validates schema implicitly)

---

### Test 5: CLI Argument Handling

Test all argument variations.

```powershell
# Test 5a: No arguments (default)
.\build\Debug\Sentinel.exe
# Expected: Uses policies/sample_policy.json

# Test 5b: Positional policy path
.\build\Debug\Sentinel.exe policies\sample_policy.json
# Expected: No warning, uses specified policy

# Test 5c: Absolute path positional
.\build\Debug\Sentinel.exe C:\Users\rohta\source\repos\Sentinel\policies\sample_policy.json
# Expected: No warning, uses absolute path

# Test 5d: Flag-based policy
.\build\Debug\Sentinel.exe --policy policies\sample_policy.json
# Expected: No warning, uses flag value

# Test 5e: Both positional and flag (flag wins)
.\build\Debug\Sentinel.exe policies\sample_policy.json --policy policies\sample_policy.json
# Expected: No warning, flag takes precedence

# Test 5f: Invalid flag
.\build\Debug\Sentinel.exe --invalid-flag
# Expected: Warning logged, continues with default

# Test 5g: Multiple positional args
.\build\Debug\Sentinel.exe arg1 arg2
# Expected: Warning for second arg, uses first
```

**Pass Criteria:**
- [ ] Tests 5a-5e: No warnings, correct policy loaded
- [ ] Test 5f: Warning logged but doesn't crash
- [ ] Test 5g: Warning about multiple args

---

### Test 6: Hash Determinism

Verify report hashing is consistent.

```powershell
# Run test twice
.\build\Debug\test_delivery_foundation.exe | Select-String "Hash:"
.\build\Debug\test_delivery_foundation.exe | Select-String "Hash:"
```

**Pass Criteria:**
- [ ] Both runs show identical hash values
- [ ] Hash format: 64 hex characters

---

### Test 7: Edge Cases

```powershell
# Test 7a: Missing policy file
.\build\Debug\Sentinel.exe nonexistent.json
# Expected: Error logged, exit code 1

# Test 7b: Invalid JSON policy
echo "{invalid json" > temp_policy.json
.\build\Debug\Sentinel.exe temp_policy.json
# Expected: Parse error logged, exit code 1
Remove-Item temp_policy.json

# Test 7c: Database file permissions (Windows)
# Create read-only database
Copy-Item sentinel_data.sqlite3 test_readonly.db
Set-ItemProperty test_readonly.db -Name IsReadOnly -Value $true
# Try to write (will fail gracefully)
Remove-Item test_readonly.db
```

**Pass Criteria:**
- [ ] All error cases handled gracefully (no crashes)
- [ ] Error messages are clear and actionable

---

### Test 8: CI/CD Pipeline Simulation

Simulate GitHub Actions workflow locally.

```powershell
# Clean environment
Remove-Item -Recurse -Force .\build\*

# Configure (matches CI)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Build Debug (matches CI)
cmake --build build --config Debug

# Run tests (matches CI)
.\build\Debug\test_delivery_foundation.exe
if ($LASTEXITCODE -ne 0) { 
    Write-Error "Tests failed!" 
    exit 1 
}
```

**Pass Criteria:**
- [ ] Mirrors CI workflow exactly
- [ ] Tests pass with exit code 0

---

## Pre-Commit Checklist

Before running `git commit`, verify:

### Code Quality
- [ ] No commented-out debug code
- [ ] No hardcoded timestamps (except ISO format strings)
- [ ] No TODO comments for critical functionality
- [ ] Consistent code style (matches existing codebase)
- [ ] All includes necessary (no unused headers)

### Documentation
- [ ] README.md updated with Phase 2 status
- [ ] Schema documented in README (retry_queue table)
- [ ] scripts/README.md includes test commands
- [ ] Code comments explain non-obvious logic

### Testing
- [ ] All automated tests pass (13 assertions)
- [ ] Phase 1 functionality verified (backward compatible)
- [ ] CLI argument handling tested (all modes)
- [ ] Edge cases handled gracefully

### Files to Commit
Core implementation:
- [ ] `src/db.h` (modified - queue operations)
- [ ] `src/db.cpp` (modified - retry_queue schema)
- [ ] `src/report_hasher.h` (new - SHA-256 interface)
- [ ] `src/report_hasher.cpp` (new - SHA-256 implementation)
- [ ] `src/delivery_client.h` (new - interface + mock)
- [ ] `src/delivery_client.cpp` (new - mock implementation)
- [ ] `src/main.cpp` (modified - CLI parsing fix)
- [ ] `test_delivery_foundation.cpp` (new - integration tests)
- [ ] `CMakeLists.txt` (modified - new sources/targets)

CI/CD and tooling:
- [ ] `.github/workflows/windows-build.yml` (modified - test step)
- [ ] `scripts/test.ps1` (new - local test runner)
- [ ] `scripts/test.bat` (new - batch wrapper)
- [ ] `scripts/README.md` (modified - test docs)

Documentation:
- [ ] `README.md` (modified - Phase 2 status, testing section)
- [ ] `docs/PRE_COMMIT_TESTING.md` (new - this file)

### Files NOT to Commit
- [ ] `sentinel_data.sqlite3` (runtime database)
- [ ] `reports/*.json` (generated reports)
- [ ] `*.db` (test databases)
- [ ] `build/` directory
- [ ] `.vs/` (Visual Studio)
- [ ] `*.suo`, `*.user` (IDE files)

---

## Commit Message Template

```
feat(delivery): Add delivery foundation layer

Implements Phase 2 delivery infrastructure:
- retry_queue schema (3-state: PENDING/DELIVERED/FAILED)
- SHA-256 report hashing for deduplication
- DeliveryClient interface + MockDeliveryClient
- Database queue operations (enqueue, load, mark_delivered, mark_failed, update_retry)
- Integration test suite (13 assertions)
- CI/CD pipeline with automated tests

Fixes:
- CLI argument parsing now supports positional and flag-based policy paths
- Defensive NULL checks in DB row parsing
- Dynamic timestamp handling prevents test decay

Breaking changes: None (backward compatible with Phase 1)

Tested:
- All automated tests pass
- Backward compatibility verified
- CLI argument handling validated
- Edge cases handled gracefully
```

---

## Troubleshooting

### Build Fails
```powershell
# Clean and rebuild
Remove-Item -Recurse -Force .\build\*
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug
```

### Tests Fail
```powershell
# Run with verbose output
.\build\Debug\test_delivery_foundation.exe

# Check for leftover test databases
Get-ChildItem *.db
```

### Runtime Issues
```powershell
# Check osquery availability
& "C:\Program Files\osquery\osqueryi.exe" --version

# Verify policy file exists
Test-Path policies\sample_policy.json

# Check database integrity (file size and modification time)
(Get-Item sentinel_data.sqlite3).Length -gt 0
(Get-Item sentinel_data.sqlite3).LastWriteTime
```

**Optional (requires sqlite3 CLI):**
```powershell
# Install: winget install SQLite.SQLite
sqlite3 sentinel_data.sqlite3 "PRAGMA integrity_check;"
```

---

## Success Criteria Summary

All green = ready to commit:

- ✅ Clean build (Debug + Release)
- ✅ All 13 tests pass
- ✅ Phase 1 functionality works
- ✅ No CLI warnings
- ✅ Database schema correct
- ✅ Hash determinism verified
- ✅ Edge cases handled
- ✅ Documentation updated
- ✅ CI simulation passes

Time estimate: **10-15 minutes** for full validation
