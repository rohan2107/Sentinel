@echo off
REM scripts\run.bat
REM Usage: run.bat [policy_path] [Debug|Release]
REM Default policy: policies\sample_policy.json
REM Default build config: Debug

setlocal

set "PROJECT_ROOT=%~dp0.."
set "POLICY=%PROJECT_ROOT%\policies\sample_policy.json"
set "BUILD_TYPE=Debug"

REM parse args:
if not "%~1"=="" (
    set "POLICY=%~1"
)
if not "%~2"=="" (
    set "BUILD_TYPE=%~2"
)

REM normalize backslashes
set "POLICY=%POLICY:/=\%"

echo Using policy: %POLICY%
echo Build type: %BUILD_TYPE%

REM Check for osqueryi on PATH (quick sanity check)
where osqueryi >nul 2>&1
if errorlevel 1 (
    echo WARNING: osqueryi was not found on PATH. Make sure osquery is installed and osqueryi.exe is reachable.
) else (
    for /f "tokens=*" %%i in ('where osqueryi') do set "OSQUERY_PATH=%%i"
    echo Found osqueryi: %OSQUERY_PATH%
)

REM compute exe path depending on build type
if /I "%BUILD_TYPE%"=="Release" (
    set "EXE=%PROJECT_ROOT%\build\Release\Sentinel.exe"
) else (
    set "EXE=%PROJECT_ROOT%\build\Debug\Sentinel.exe"
)

if not exist "%EXE%" (
    echo ERROR: executable not found: "%EXE%"
    echo Run scripts\build.bat %BUILD_TYPE% first.
    endlocal
    exit /b 1
)

if not exist "%POLICY%" (
    echo ERROR: policy file not found: "%POLICY%"
    endlocal
    exit /b 1
)

REM Run the executable (no simulate flag)
"%EXE%" --policy "%POLICY%"

endlocal
