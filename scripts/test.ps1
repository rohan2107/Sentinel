# scripts\test.ps1
# Run all C++ quality checks — mirrors the windows-build CI job.
# Usage: .\scripts\test.ps1 [-Config Debug|Release|Both]
# Default: Both (Debug for fast feedback + Release to match CI)

param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Config = 'Both'
)

$ErrorActionPreference = 'Stop'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

function Fail {
    param([string]$Step)
    Write-Host ""
    Write-Host "===================================================" -ForegroundColor Red
    Write-Host "  FAILED: $Step" -ForegroundColor Red
    Write-Host "===================================================" -ForegroundColor Red
    exit 1
}

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "        Sentinel Integration Tests" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Config: $Config" -ForegroundColor Gray
Write-Host "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray
Write-Host ""

# Determine which configs to test
$configs = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

foreach ($cfg in $configs) {
    Write-Host "---------------------------------------------------" -ForegroundColor DarkGray
    Write-Host "Config: $cfg" -ForegroundColor Yellow
    Write-Host "---------------------------------------------------" -ForegroundColor DarkGray

    # Build if binary is missing (incremental builds are fast)
    $testExe = Join-Path $ProjectRoot "build\$cfg\test_delivery_foundation.exe"
    if (-not (Test-Path $testExe)) {
        Write-Host "Building $cfg..." -ForegroundColor Yellow
        & "$ScriptDir\build.ps1" -Config $cfg
        if ($LASTEXITCODE -ne 0) { Fail "build ($cfg)" }
        Write-Host ""
    }

    Push-Location $ProjectRoot
    try {
        & $testExe
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if ($exitCode -ne 0) {
        Write-Host "===================================================" -ForegroundColor Red
        Write-Host "  Tests FAILED ($cfg, exit $exitCode)" -ForegroundColor Red
        Write-Host "===================================================" -ForegroundColor Red
        exit $exitCode
    }

    Write-Host "[OK] tests ($cfg)" -ForegroundColor Green
    Write-Host ""
}

# --- Policy JSON validation (mirrors validate-backend CI job) ---------------
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray
Write-Host "Policy JSON validation" -ForegroundColor Yellow
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray

$policyDir = Join-Path $ProjectRoot "policies"
$policyErrors = 0
Get-ChildItem $policyDir -Filter "*.json" | ForEach-Object {
    try {
        $data = Get-Content $_.FullName -Raw | ConvertFrom-Json
        # Mirrors CI assertion: policy_name or rules key must be present
        if (-not ($data.policy_name -or $data.rules)) {
            Write-Host "  [FAIL] $($_.Name): missing 'policy_name' or 'rules' key" -ForegroundColor Red
            $policyErrors++
        } else {
            Write-Host "  [OK] $($_.Name)" -ForegroundColor Green
        }
    } catch {
        Write-Host "  [FAIL] $($_.Name): invalid JSON — $_" -ForegroundColor Red
        $policyErrors++
    }
}
if ($policyErrors -gt 0) { Fail "policy JSON validation ($policyErrors error(s))" }
Write-Host "[OK] all policy files valid" -ForegroundColor Green
Write-Host ""

# --- Python backend syntax check (mirrors validate-backend CI job) ----------
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray
Write-Host "Backend syntax check" -ForegroundColor Yellow
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray

$python = Get-Command python -ErrorAction SilentlyContinue
$backendFile = Join-Path $ProjectRoot "backend\server.py"
if ($python -and (Test-Path $backendFile)) {
    python -m py_compile $backendFile
    if ($LASTEXITCODE -ne 0) { Fail "backend/server.py syntax check" }
    Write-Host "[OK] backend/server.py" -ForegroundColor Green
} elseif (-not (Test-Path $backendFile)) {
    Write-Host "--- skipped (backend/server.py not found)" -ForegroundColor DarkGray
} else {
    Write-Host "--- skipped (python not on PATH)" -ForegroundColor DarkGray
    Write-Host "    Install Python to mirror the validate-backend CI job." -ForegroundColor DarkGray
}
Write-Host ""

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  ALL TESTS PASSED" -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Cyan
