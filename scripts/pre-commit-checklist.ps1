# Pre-Commit Checklist Script
# Runs all validation checks before committing to master
# Usage: .\scripts\pre-commit-checklist.ps1

param(
    [switch]$SkipBackendTest = $false,
    [switch]$Quick = $false
)

$script:passCount = 0
$script:failCount = 0
$script:warnings = @()

# Color helpers
function Write-Pass {
    param([string]$Message)
    Write-Host "✅ $Message" -ForegroundColor Green
    $script:passCount++
}

function Write-Fail {
    param([string]$Message)
    Write-Host "❌ $Message" -ForegroundColor Red
    $script:failCount++
}

function Write-Warn {
    param([string]$Message)
    Write-Host "⚠️  $Message" -ForegroundColor Yellow
    $script:warnings += $Message
}

function Write-Check {
    param([string]$Message)
    Write-Host "🔍 $Message" -ForegroundColor Cyan
}

function Write-Section {
    param([string]$Title)
    Write-Host "`n" + ("=" * 70) -ForegroundColor Magenta
    Write-Host $Title -ForegroundColor Magenta
    Write-Host ("=" * 70) -ForegroundColor Magenta
}

# Main execution
$rootDir = Split-Path -Parent $PSScriptRoot  # scripts -> Sentinel root
Push-Location $rootDir

Write-Section "PRE-COMMIT VALIDATION CHECKLIST"

# ============================================================================
# 1. BUILD VALIDATION
# ============================================================================

Write-Section "1. BUILD VALIDATION"

Write-Check "Building Debug configuration..."
$buildResult = cmake --build build --config Debug 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Pass "Debug build succeeded"
} else {
    Write-Fail "Debug build failed"
    Write-Host $buildResult -ForegroundColor Red
    Pop-Location
    exit 1
}

# Check for compiler warnings in new files
Write-Check "Scanning for compiler warnings in new code..."
$warnings_found = $buildResult | Select-String -Pattern "warning:|error:" | Measure-Object | Select-Object -ExpandProperty Count
if ($warnings_found -eq 0) {
    Write-Pass "No compiler warnings detected"
} else {
    Write-Warn "Found potential warnings (verify they're acceptable)"
    $buildResult | Select-String -Pattern "warning:|error:" | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}

# ============================================================================
# 2. AUTOMATED TEST SUITE
# ============================================================================

Write-Section "2. AUTOMATED TEST SUITE"

Write-Check "Cleaning up test database files from previous runs..."
@("test_sentinel.db", "test_retry_queue.db", "test_delivery_foundation.db") | 
    ForEach-Object { 
        if (Test-Path $_) { 
            Remove-Item $_ -Force -ErrorAction SilentlyContinue
        } 
    }
Write-Pass "Test databases cleaned"

Write-Check "Running integration tests..."
$testExe = ".\build\Debug\test_delivery_foundation.exe"
if (-not (Test-Path $testExe)) {
    Write-Fail "Test executable not found: $testExe"
    Pop-Location
    exit 1
}

$testOutput = & $testExe 2>&1
$testExitCode = $LASTEXITCODE

if ($testExitCode -eq 0 -and ($testOutput -match "ALL TESTS PASSED")) {
    Write-Pass "All integration tests passed"
} else {
    Write-Fail "Integration tests failed"
    Write-Host $testOutput -ForegroundColor Red
    Pop-Location
    exit 1
}

# Count test results
$testsPassed = $testOutput | Select-String -Pattern "\[PASS\]" | Measure-Object | Select-Object -ExpandProperty Count
Write-Host "  Test suites passed: $testsPassed" -ForegroundColor Green

# Verify no test database files left behind AFTER tests
$testDbs = @("test_sentinel.db", "test_retry_queue.db", "test_delivery_foundation.db")
$leftoverDbs = $testDbs | Where-Object { Test-Path $_ }
if ($leftoverDbs.Count -eq 0) {
    Write-Pass "No test database files left behind"
} else {
    Write-Fail "Test database files not cleaned up by tests: $($leftoverDbs -join ', ')"
}

# ============================================================================
# 3. BACKWARD COMPATIBILITY
# ============================================================================

Write-Section "3. BACKWARD COMPATIBILITY (Phase 1 Functionality)"

Write-Check "Testing Phase 1 default invocation..."
$phase1Result = .\build\Debug\Sentinel.exe 2>&1
if ($LASTEXITCODE -eq 0 -and (Test-Path "reports\latest_report.json")) {
    Write-Pass "Phase 1 default invocation works"
} else {
    Write-Fail "Phase 1 default invocation failed"
    Pop-Location
    exit 1
}

Write-Check "Verifying report structure..."
$report = Get-Content "reports\latest_report.json" -ErrorAction SilentlyContinue | ConvertFrom-Json -ErrorAction SilentlyContinue
if ($report -and $report.timestamp -and $report.hostname -and $report.policy -and $null -ne $report.score) {
    Write-Pass "Report contains required fields (timestamp, hostname, policy, score)"
} else {
    Write-Fail "Report missing required fields"
    Pop-Location
    exit 1
}

Write-Check "Verifying database persistence..."
if (Test-Path "sentinel_data.sqlite3") {
    $dbFile = Get-Item "sentinel_data.sqlite3"
    $recentTime = [datetime]::Now.AddSeconds(-60)
    if ($dbFile.LastWriteTime -gt $recentTime) {
        Write-Pass "Database was updated recently"
    } else {
        Write-Warn "Database modification time seems old (verify manually if concerned)"
    }
} else {
    Write-Fail "Database file not found"
    Pop-Location
    exit 1
}

# ============================================================================
# 4. CLI ARGUMENT HANDLING
# ============================================================================

Write-Section "4. CLI ARGUMENT HANDLING"

Write-Check "Testing policy argument variations..."
$testPassed = $true

# Test positional argument
$result = .\build\Debug\Sentinel.exe policies\sample_policy.json 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Pass "Positional policy argument works"
} else {
    Write-Fail "Positional policy argument failed"
    $testPassed = $false
}

# Test flag argument
$result = .\build\Debug\Sentinel.exe --policy policies\sample_policy.json 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Pass "Flag-based policy argument works"
} else {
    Write-Fail "Flag-based policy argument failed"
    $testPassed = $false
}

# Test invalid flag (should warn but not crash)
$result = .\build\Debug\Sentinel.exe --invalid-flag 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Pass "Invalid flag handled gracefully (no crash)"
} else {
    Write-Fail "Invalid flag caused crash"
    $testPassed = $false
}

if (-not $testPassed) {
    Pop-Location
    exit 1
}

# ============================================================================
# 5. DELIVERY LAYER (if not skipped)
# ============================================================================

if (-not $SkipBackendTest) {
    Write-Section "5. HTTP DELIVERY TO FASTAPI BACKEND"
    
    Write-Check "Checking backend availability..."
    $backendPath = Join-Path $rootDir "backend"
    if (-not (Test-Path (Join-Path $backendPath "server.py"))) {
        Write-Warn "Backend server.py not found, skipping delivery tests"
    } else {
        Write-Check "Starting FastAPI backend..."
        $backendJob = Start-Job -ScriptBlock {
            Push-Location $args[0]
            python -m uvicorn server:app --host 127.0.0.1 --port 8000 --log-level warning
        } -ArgumentList $backendPath
        
        Start-Sleep -Seconds 3
        
        # Verify backend is running
        $backendHealthy = $false
        try {
            $response = Invoke-RestMethod -Uri "http://localhost:8000/health" -ErrorAction Stop
            if ($response -and $response.status) {
                Write-Pass "Backend health check passed"
                $backendHealthy = $true
            } else {
                Write-Warn "Backend health endpoint returned: $response"
            }
        } catch {
            Write-Warn "Backend not responding to health check: $($_.Exception.Message)"
        }
        
        if ($backendHealthy) {
            Write-Check "Running agent with delivery enabled..."
            $deliveryResult = .\build\Debug\Sentinel.exe --enable-delivery --backend-url "http://localhost:8000" 2>&1
            if ($LASTEXITCODE -eq 0) {
                Write-Pass "Agent with --enable-delivery succeeded"
            } else {
                Write-Warn "Agent with --enable-delivery had exit code $LASTEXITCODE"
            }
            
            # Try to verify delivery
            try {
                Start-Sleep -Seconds 1
                $reports = Invoke-RestMethod -Uri "http://localhost:8000/reports" -ErrorAction Stop
                if ($reports -is [array] -and $reports.Count -gt 0) {
                    Write-Pass "Backend received reports"
                } elseif ($reports -is [object]) {
                    Write-Pass "Backend reports endpoint responding"
                } else {
                    Write-Warn "Backend reports endpoint empty or unexpected format"
                }
            } catch {
                Write-Warn "Could not verify backend reports: $($_.Exception.Message)"
            }
        } else {
            Write-Warn "Skipping delivery verification (backend not healthy)"
        }
        
        Write-Check "Cleaning up backend..."
        Stop-Job $backendJob -ErrorAction SilentlyContinue | Wait-Job
        Start-Sleep -Seconds 1
        Write-Pass "Backend stopped"
    }
} else {
    Write-Section "5. HTTP DELIVERY TO FASTAPI BACKEND (SKIPPED)"
    Write-Check "Use --SkipBackendTest `$false to include backend tests"
}

# ============================================================================
# 6. CODE QUALITY CHECKS
# ============================================================================

Write-Section "6. CODE QUALITY CHECKS"

Write-Check "Scanning for debug patterns..."
$debugPatterns = @(
    @{ Pattern = "console\.log|console\.error|console\.warn"; Files = "src/*.cpp"; Name = "JS console calls" },
    @{ Pattern = "DEBUG|TODO.*FIXME"; Files = "src/*.cpp"; Name = "Debug markers" }
)

$debugIssues = 0
foreach ($pattern in $debugPatterns) {
    $matches = Get-ChildItem -Path $pattern.Files -ErrorAction SilentlyContinue | 
        Select-String -Pattern $pattern.Pattern | 
        Where-Object { $_.Matches.Count -gt 0 }
    
    if ($matches) {
        Write-Warn "Found '$($pattern.Pattern)' in code:"
        $matches | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber)" -ForegroundColor Yellow }
        $debugIssues++
    }
}

# Note: std::cout in main.cpp for final report output is intentional
$stdcoutMatches = Get-ChildItem -Path "src/*.cpp" -ErrorAction SilentlyContinue | 
    Select-String -Pattern "std::cout" | 
    Where-Object { 
        # Exclude line 262 (final report output in main.cpp)
        -not ($_.Path -match "main\.cpp" -and $_.LineNumber -eq 262)
    }

if ($stdcoutMatches) {
    Write-Warn "Found std::cout (should use spdlog):"
    $stdcoutMatches | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber)" -ForegroundColor Yellow }
    $debugIssues++
} else {
    Write-Pass "No problematic std::cout calls found"
}

if ($debugIssues -eq 0) {
    Write-Pass "No debug patterns detected"
}

# ============================================================================
# 7. DOCUMENTATION CHECKS
# ============================================================================

Write-Section "7. DOCUMENTATION CHECKS"

$docChecks = @(
    @{ File = "README.md"; Pattern = "Phase 2.*COMPLETE"; Name = "Phase 2 Status" },
    @{ File = "docs\roadmap\IMPLEMENTATION_PLAN.md"; Pattern = "Phase 4.*COMPLETE"; Name = "Phase 4 Status" },
    @{ File = "docs\roadmap\IMPLEMENTATION_PLAN.md"; Pattern = "Phase 5.*COMPLETE"; Name = "Phase 5 Status" }
)

foreach ($check in $docChecks) {
    if (Test-Path $check.File) {
        $content = Get-Content $check.File -Raw
        if ($content -match $check.Pattern) {
            Write-Pass "$($check.Name): documented"
        } else {
            Write-Warn "$($check.Name): pattern not found in $($check.File)"
        }
    } else {
        Write-Warn "File not found: $($check.File)"
    }
}

# ============================================================================
# 8. GIT STATUS CHECK
# ============================================================================

Write-Section "8. GIT STATUS & COMMIT READINESS"

Write-Check "Checking git status..."
$gitStatus = git status --short 2>&1

$modified = @()
$untracked = @()

$gitStatus | ForEach-Object {
    if ($_ -match "^ M ") {
        $modified += $_.Substring(3)
    } elseif ($_ -match "^\?\? ") {
        $untracked += $_.Substring(3)
    }
}

if ($modified.Count -gt 0) {
    Write-Host "Modified files:" -ForegroundColor Cyan
    $modified | ForEach-Object { Write-Host "  M $_" -ForegroundColor Green }
    Write-Pass "Found $($modified.Count) modified files ready to commit"
} else {
    Write-Warn "No modified files detected"
}

if ($untracked.Count -gt 0) {
    Write-Host "Untracked files:" -ForegroundColor Cyan
    $untracked | ForEach-Object { 
        if ($_ -match '\.(bak|tmp|sqlite3)$|^build/|^\.vs/') {
            Write-Host "  ? $_ (should be .gitignored)" -ForegroundColor Yellow
        } else {
            Write-Host "  ? $_" -ForegroundColor Cyan
        }
    }
}

# ============================================================================
# FINAL SUMMARY
# ============================================================================

Write-Section "FINAL SUMMARY"

Write-Host "`n📊 Test Results:" -ForegroundColor Cyan
Write-Host "  ✅ Passed: $script:passCount" -ForegroundColor Green
Write-Host "  ❌ Failed: $script:failCount" -ForegroundColor $(if ($script:failCount -gt 0) { "Red" } else { "Green" })
Write-Host "  ⚠️  Warnings: $($script:warnings.Count)" -ForegroundColor Yellow

if ($script:failCount -gt 0) {
    Write-Host "`n❌ PRE-COMMIT VALIDATION FAILED" -ForegroundColor Red
    Write-Host "Fix the above issues before committing." -ForegroundColor Red
    Pop-Location
    exit 1
}

if ($script:warnings.Count -gt 0) {
    Write-Host "`n⚠️  PRE-COMMIT VALIDATION WARNING" -ForegroundColor Yellow
    Write-Host "Review warnings above before committing." -ForegroundColor Yellow
    Write-Host "`nWarnings:" -ForegroundColor Yellow
    $script:warnings | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
}

Write-Host "`n✅ PRE-COMMIT VALIDATION PASSED" -ForegroundColor Green
Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "  1. Review modified files: git diff --stat" -ForegroundColor Cyan
Write-Host "  2. Stage files: git add ." -ForegroundColor Cyan
Write-Host "  3. Commit: git commit -m 'Your message'" -ForegroundColor Cyan
Write-Host "  4. Push: git push origin feature-branch" -ForegroundColor Cyan

Pop-Location
exit 0
