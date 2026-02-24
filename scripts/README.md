# Build Scripts

Sentinel provides both Command Prompt (.bat) and PowerShell 7 (.ps1) build scripts.

## PowerShell 7 Scripts (Recommended for Shell Integration)

### Prerequisites
- PowerShell 7+ installed
- Visual Studio 2022 Build Tools or Community Edition
- vcpkg with required packages installed

### Usage

**Build:**
```powershell
.\scripts\build.ps1 -Config Debug    # or Release
```

**Run:**
```powershell
.\scripts\run.ps1 -Policy policies\sample_policy.json -Config Debug
```

**Test (Integration Tests):**
```powershell
.\scripts\test.ps1 -Config Debug    # or Release
```
*Runs delivery foundation integration tests*

**Smoketest (Build + Run + Verify):**
```powershell
.\scripts\smoketest.ps1
```

**Pre-Commit Checklist (Comprehensive Validation):**
```powershell
.\scripts\pre-commit-checklist.ps1
```
*Runs all validation checks before committing:*
- ✅ Build validation (Debug config)
- ✅ Automated test suite (all integration tests)
- ✅ Backward compatibility (Phase 1 functionality)
- ✅ CLI argument handling (all variations)
- ✅ HTTP delivery to FastAPI backend (if available)
- ✅ Code quality checks (debug patterns)
- ✅ Documentation verification
- ✅ Git status review

**Options:**
```powershell
# Skip backend testing (if Python not available)
.\scripts\pre-commit-checklist.ps1 -SkipBackendTest

# Quick validation only (no backend test)
.\scripts\pre-commit-checklist.ps1 -Quick
```

### Execution Policy

If you get an execution policy error, run:
```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

### Shell Integration

PowerShell 7 provides better shell integration with VS Code including:
- Command feedback
- PWD tracking
- Git status
- Command decorations

To use PowerShell 7 as default terminal in VS Code:
1. Open Settings (Ctrl+,)
2. Search for "terminal.integrated.defaultProfile.windows"
3. Set to "PowerShell" (pwsh.exe)

## Command Prompt Scripts (Legacy)

**Build:**
```cmd
scripts\build.bat Debug
```

**Run:**
```cmd
scripts\run.bat policies\sample_policy.json
```

**Test (Integration Tests):**
```cmd
scripts\test.bat Debug
```
*Runs delivery foundation integration tests*

**Smoketest:**
```cmd
scripts\smoketest.bat
```

**Pre-Commit Checklist:**
```cmd
scripts\pre-commit-checklist.bat
```
*Runs all validation checks before committing (see PowerShell section for details)*

## Troubleshooting

### "vcvars64_wrapper.bat" error in PowerShell

If you see `/k C:\tools\vcvars64_wrapper.bat` errors:

1. **Option A**: Run from a "Developer Command Prompt for VS 2022" instead of regular PowerShell
2. **Option B**: Use the Command Prompt (.bat) scripts instead
3. **Option C**: Remove the vcvars64_wrapper.bat from your profile/startup scripts

The PowerShell scripts (.ps1) work best when run from a Developer PowerShell or when Visual Studio environment is already loaded.

### "cmake not found"

Ensure CMake is installed and on PATH:
```powershell
winget install cmake
```

### "osqueryi not found"

Download and install osquery from https://osquery.io/downloads/official
