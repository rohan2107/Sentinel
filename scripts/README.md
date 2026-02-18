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

**Smoketest (Build + Run + Verify):**
```powershell
.\scripts\smoketest.ps1
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

**Smoketest:**
```cmd
scripts\smoketest.bat
```

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
