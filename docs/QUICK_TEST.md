# Quick Test Reference

**Before every commit, run:**

```powershell
# One-liner validation
.\scripts\build.ps1; .\scripts\test.ps1; .\scripts\run.ps1
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

## If Anything Fails

1. **Build fails** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#build-fails)
2. **Tests fail** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#tests-fail)
3. **Runtime fails** → Check [PRE_COMMIT_TESTING.md](PRE_COMMIT_TESTING.md#runtime-issues)

---

## Manual Test Checklist

Before pushing to GitHub:

- [ ] Clean build passes
- [ ] All integration tests pass
- [ ] Phase 1 functionality works (backward compatible)
- [ ] No CLI warnings when running
- [ ] Database file exists and updated: `(Get-Item sentinel_data.sqlite3).LastWriteTime`

**Time: ~5 minutes**

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
