# scripts\test.ps1
# Run integration tests
# Usage: .\test.ps1 [-Config Debug|Release]
# Default: Debug

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "        Sentinel Integration Tests" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Configuration: $Config" -ForegroundColor Gray
Write-Host "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray
Write-Host ""

# Ensure build exists
$testExePath = Join-Path $ProjectRoot "build\$Config\test_delivery_foundation.exe"
if (-not (Test-Path $testExePath)) {
    Write-Host "Test executable not found: $testExePath" -ForegroundColor Yellow
    Write-Host "Building $Config configuration first..." -ForegroundColor Yellow
    Write-Host ""
    
    Push-Location $ProjectRoot
    try {
        & "$ScriptDir\build.ps1" -Config $Config
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Build failed - cannot run tests."
            exit $LASTEXITCODE
        }
    } finally {
        Pop-Location
    }
    
    Write-Host ""
}

# Run tests
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray
Write-Host "Running Delivery Foundation Integration Tests" -ForegroundColor Yellow
Write-Host "---------------------------------------------------" -ForegroundColor DarkGray
Write-Host ""

Push-Location $ProjectRoot
try {
    & $testExePath
    $exitCode = $LASTEXITCODE
    
    Write-Host ""
    
    if ($exitCode -ne 0) {
        Write-Host "===================================================" -ForegroundColor Red
        Write-Host "        X Tests Failed (exit code: $exitCode)" -ForegroundColor Red
        Write-Host "===================================================" -ForegroundColor Red
        exit $exitCode
    } else {
        Write-Host "===================================================" -ForegroundColor Cyan
        Write-Host "        * All Tests Passed" -ForegroundColor Green
        Write-Host "===================================================" -ForegroundColor Cyan
    }
} finally {
    Pop-Location
}

exit 0
