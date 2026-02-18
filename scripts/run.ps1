# scripts\run.ps1
# Usage: .\run.ps1 [-Policy path] [-Config Debug|Release]

param(
    [string]$Policy = 'policies\sample_policy.json',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Normalize policy path
if (-not [System.IO.Path]::IsPathRooted($Policy)) {
    $Policy = Join-Path $ProjectRoot $Policy
}

Write-Host "Using policy: $Policy" -ForegroundColor Cyan
Write-Host "Build type: $Config" -ForegroundColor Cyan

# Check osqueryi
$osqueryPath = Get-Command osqueryi -ErrorAction SilentlyContinue
if ($osqueryPath) {
    Write-Host "Found osqueryi: $($osqueryPath.Source)" -ForegroundColor Green
} else {
    Write-Warning "osqueryi not found on PATH. Make sure osquery is installed."
}

# Determine executable path
$ExePath = Join-Path $ProjectRoot "build\$Config\Sentinel.exe"

if (-not (Test-Path $ExePath)) {
    Write-Error "Executable not found: $ExePath`nRun .\scripts\build.ps1 -Config $Config first."
    exit 1
}

if (-not (Test-Path $Policy)) {
    Write-Error "Policy file not found: $Policy"
    exit 1
}

# Run Sentinel
Write-Host "`nRunning: $ExePath $Policy" -ForegroundColor Gray
Write-Host "─" * 80 -ForegroundColor DarkGray

& $ExePath $Policy

$exitCode = $LASTEXITCODE
Write-Host "─" * 80 -ForegroundColor DarkGray

if ($exitCode -eq 0) {
    Write-Host "`n✓ Sentinel completed successfully" -ForegroundColor Green
} else {
    Write-Host "`n✗ Sentinel exited with code $exitCode" -ForegroundColor Red
}

exit $exitCode
