# scripts\build.ps1
# Usage: .\build.ps1 [-Config Debug|Release]
# Default: Debug

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

Write-Host "Building Sentinel (config: $Config)..." -ForegroundColor Cyan

# Check for cl.exe (MSVC compiler)
$clPath = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $clPath) {
    Write-Host "cl.exe not found on PATH. Attempting to load Visual Studio environment..." -ForegroundColor Yellow
    
    $vcvarsLocations = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\tools\vcvars64_wrapper.bat"
    )
    
    $vcvarsPath = $vcvarsLocations | Where-Object { Test-Path $_ } | Select-Object -First 1
    
    if ($vcvarsPath) {
        Write-Host "Found vcvars at: $vcvarsPath" -ForegroundColor Green
        Write-Host "Note: PowerShell cannot directly source .bat files. Please run this in a Developer Command Prompt," -ForegroundColor Yellow
        Write-Host "or use 'cmake --preset' with a Visual Studio generator." -ForegroundColor Yellow
    } else {
        Write-Error "Could not find vcvars64.bat. Open a Developer Command Prompt or install Visual Studio Build Tools."
        exit 1
    }
}

# Configure with CMake
$cmakeConfigArgs = @(
    '-S', $ProjectRoot,
    '-B', "$ProjectRoot\build",
    "-DCMAKE_BUILD_TYPE=$Config",
    '-A', 'x64'
)

if (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake") {
    $cmakeConfigArgs += '-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake'
    $cmakeConfigArgs += '-DVCPKG_TARGET_TRIPLET=x64-windows'
}

Write-Host "`nRunning: cmake $($cmakeConfigArgs -join ' ')" -ForegroundColor Gray
& cmake $cmakeConfigArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

# Build
Write-Host "`nRunning: cmake --build $ProjectRoot\build --config $Config" -ForegroundColor Gray
& cmake --build "$ProjectRoot\build" --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "`n✓ Build succeeded (config: $Config)" -ForegroundColor Green
exit 0
