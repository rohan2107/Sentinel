# scripts\smoketest.ps1
# Comprehensive build + run sanity check

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "        Sentinel Smoketest" -ForegroundColor Cyan
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray
Write-Host ""

# Step 1: Build Debug
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "Step 1: Building Debug configuration" -ForegroundColor Yellow
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray

Push-Location $ProjectRoot
try {
    & "$ScriptDir\build.ps1" -Config Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed - smoketest aborting."
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host ""

# Step 2: Run Sentinel with sample policy
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "Step 2: Running Sentinel with sample_policy.json" -ForegroundColor Yellow
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray

Push-Location $ProjectRoot
try {
    & "$ScriptDir\run.ps1" -Policy "policies\sample_policy.json"
    $exitCode = $LASTEXITCODE
    
    if ($exitCode -ne 0) {
        Write-Error "Sentinel exited with code $exitCode"
        exit $exitCode
    }
} finally {
    Pop-Location
}

Write-Host ""

# Step 3: Show latest report
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "Step 3: Latest report (reports\latest_report.json)" -ForegroundColor Yellow
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray

$reportPath = Join-Path $ProjectRoot "reports\latest_report.json"
if (Test-Path $reportPath) {
    $report = Get-Content $reportPath -Raw | ConvertFrom-Json
    Write-Host ($report | ConvertTo-Json -Depth 10) -ForegroundColor White
    
    # Validate report structure
    Write-Host ""
    Write-Host "Report validation:" -ForegroundColor Cyan
    if ($report.policy) { Write-Host "  ✓ Policy: $($report.policy)" -ForegroundColor Green }
    if ($report.score -ne $null) { Write-Host "  ✓ Score: $($report.score)" -ForegroundColor Green }
    if ($report.hostname) { Write-Host "  ✓ Hostname: $($report.hostname)" -ForegroundColor Green }
    if ($report.timestamp) { Write-Host "  ✓ Timestamp: $($report.timestamp)" -ForegroundColor Green }
} else {
    Write-Warning "Report file not found: $reportPath"
}

Write-Host ""

# Step 4: Show last DB entry if sqlite3 is available
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "Step 4: Database verification" -ForegroundColor Yellow
Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray

$sqlite3 = Get-Command sqlite3 -ErrorAction SilentlyContinue
if ($sqlite3) {
    $dbPath = Join-Path $ProjectRoot "sentinel_data.sqlite3"
    if (Test-Path $dbPath) {
        Write-Host "Last DB entry:" -ForegroundColor Cyan
        & sqlite3 $dbPath "SELECT id, ts, policy, score FROM runs ORDER BY id DESC LIMIT 1;"
    } else {
        Write-Warning "Database file not found: $dbPath"
    }
} else {
    Write-Host "sqlite3 not found on PATH - skipping DB verification" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan
Write-Host "        ✓ Smoketest Completed Successfully" -ForegroundColor Green
Write-Host "═══════════════════════════════════════════════════" -ForegroundColor Cyan

exit 0
