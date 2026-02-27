# go-aggregator/scripts/test.ps1
# Run all Go quality checks for the aggregator service.
# Usage: .\go-aggregator\scripts\test.ps1
# Must be run from the repository root OR from inside go-aggregator/.

$ErrorActionPreference = 'Stop'

# Resolve go-aggregator root regardless of where the script is called from.
$ScriptDir      = Split-Path -Parent $MyInvocation.MyCommand.Path
$AggregatorRoot = Split-Path -Parent $ScriptDir

Set-Location $AggregatorRoot

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "        Go Aggregator - Quality Checks" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Working directory: $AggregatorRoot" -ForegroundColor Gray
Write-Host "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray
Write-Host ""

function Fail {
    param([string]$Step)
    Write-Host ""
    Write-Host "===================================================" -ForegroundColor Red
    Write-Host "  FAILED: $Step (exit $LASTEXITCODE)" -ForegroundColor Red
    Write-Host "===================================================" -ForegroundColor Red
    exit 1
}

# 1. Tidy check — ensures go.mod/go.sum are in sync with imports.
#    Fails fast if someone added a dep without running 'go mod tidy'.
Write-Host "--- go mod tidy (verify in-sync)" -ForegroundColor Yellow
go mod tidy
if ($LASTEXITCODE -ne 0) { Fail "go mod tidy" }

$diff = git diff --name-only go.mod go.sum 2>$null
if ($diff) {
    Write-Host "go.mod or go.sum changed after tidy -- commit the result:" -ForegroundColor Red
    Write-Host $diff
    exit 1
}
Write-Host "[OK] go.mod and go.sum are tidy" -ForegroundColor Green
Write-Host ""

# 2. Build — catches compile errors before running tests.
Write-Host "--- go build ./..." -ForegroundColor Yellow
go build ./...
if ($LASTEXITCODE -ne 0) { Fail "go build ./..." }
Write-Host "[OK] build" -ForegroundColor Green
Write-Host ""

# 3. Vet — static analysis; catches common mistakes the compiler won't.
Write-Host "--- go vet ./..." -ForegroundColor Yellow
go vet ./...
if ($LASTEXITCODE -ne 0) { Fail "go vet ./..." }
Write-Host "[OK] vet" -ForegroundColor Green
Write-Host ""

# 4. Test. Race detector requires CGO (a C compiler).
#    On Windows with MSVC only, CGO is not available, so we skip -race locally.
#    The CI workflow (ubuntu-latest) runs with -race — that is where race detection lives.
$cgoEnabled = (& go env CGO_ENABLED 2>$null).Trim()
if ($cgoEnabled -eq "1") {
    Write-Host "--- go test -race -count=1 -v ./..." -ForegroundColor Yellow
    go test -race -count=1 -v ./...
} else {
    Write-Host "--- go test -count=1 -v ./... (race detector skipped: CGO not available)" -ForegroundColor Yellow
    Write-Host "    NOTE: -race runs in CI on ubuntu-latest" -ForegroundColor DarkGray
    go test -count=1 -v ./...
}
if ($LASTEXITCODE -ne 0) { Fail "go test" }
Write-Host "[OK] tests" -ForegroundColor Green
Write-Host ""

# 5. Lint — mirrors the golangci-lint step in CI.
#    Version constraint: golangci-lint must be built with a Go version >= the 'go'
#    directive in go.mod. If lint fails with "language version mismatch", run:
#      go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest
#    Install once: go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest
#    Requires GOPATH/bin on PATH (e.g. C:\Users\<you>\go\bin).
$lintBin = (Get-Command golangci-lint -ErrorAction SilentlyContinue)
if ($lintBin) {
    Write-Host "--- golangci-lint run ./..." -ForegroundColor Yellow
    golangci-lint run ./...
    if ($LASTEXITCODE -ne 0) { Fail "golangci-lint" }
    Write-Host "[OK] lint" -ForegroundColor Green
} else {
    Write-Host "--- golangci-lint skipped (not installed)" -ForegroundColor DarkGray
    Write-Host "    Install: go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest" -ForegroundColor DarkGray
    Write-Host "    Lint always runs in CI." -ForegroundColor DarkGray
}
Write-Host ""

# 6. Vulnerability scan — mirrors the govulncheck step in CI.
#    Install once: go install golang.org/x/vuln/cmd/govulncheck@latest
$vulnBin = (Get-Command govulncheck -ErrorAction SilentlyContinue)
if ($vulnBin) {
    Write-Host "--- govulncheck ./..." -ForegroundColor Yellow
    govulncheck ./...
    if ($LASTEXITCODE -ne 0) { Fail "govulncheck" }
    Write-Host "[OK] vuln scan" -ForegroundColor Green
} else {
    Write-Host "--- govulncheck skipped (not installed)" -ForegroundColor DarkGray
    Write-Host "    Install: go install golang.org/x/vuln/cmd/govulncheck@latest" -ForegroundColor DarkGray
    Write-Host "    Vuln scan always runs in CI." -ForegroundColor DarkGray
}
Write-Host ""

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "  ALL CHECKS PASSED" -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Cyan
