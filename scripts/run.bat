@echo off
REM scripts\run.bat  -- meant to be run from repo root: ...\Sentinel>

setlocal

REM Usage: scripts\run.bat [--no-simulate] [policy_path]

REM default policy (relative to repo root)
set "POLICY=policies\sample_policy.json"
set "FLAG=--simulate"

REM parse args
if "%~1"=="" goto :run
if "%~1"=="--no-simulate" (
    set "FLAG=--no-simulate"
    if not "%~2"=="" set "POLICY=%~2"
) else (
    set "POLICY=%~1"
)

:run
REM exe path (relative to repo root)
set "EXE=build\Debug\SentinelAgent.exe"

if not exist "%EXE%" (
    echo ERROR: executable not found: "%EXE%"
    exit /b 1
)

if not exist "%POLICY%" (
    echo ERROR: policy file not found: "%POLICY%"
    exit /b 1
)

"%EXE%" --policy "%POLICY%" %FLAG%

endlocal
