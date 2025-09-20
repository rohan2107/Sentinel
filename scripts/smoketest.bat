@echo off
REM scripts\smoketest.bat - quick build + run sanity check
setlocal

echo === Sentinel Smoketest ===
echo Timestamp: %DATE% %TIME%
echo.

REM 1) Build Debug
call scripts\build.bat Debug
if errorlevel 1 (
  echo Build failed - smoketest aborting.
  exit /b 1
)

REM 2) Run Sentinel with sample policy
echo.
echo --- Running Sentinel with sample_policy.json ---
scripts\run.bat policies\sample_policy.json
set EXIT_CODE=%ERRORLEVEL%
if %EXIT_CODE% NEQ 0 (
  echo Sentinel exited with code %EXIT_CODE%
  exit /b %EXIT_CODE%
)

REM 3) Show latest report
echo.
echo --- Latest report (reports\latest_report.json) ---
if exist reports\latest_report.json (
  type reports\latest_report.json
) else (
  echo Report file not found!
)

REM 4) Optionally show last DB row if sqlite3 present
where sqlite3 >nul 2>&1
if not errorlevel 1 (
  echo.
  echo --- Last DB entry in sentinel_data.sqlite3 ---
  sqlite3 sentinel_data.sqlite3 "SELECT id, timestamp, policy, score FROM runs ORDER BY id DESC LIMIT 1;"
)

echo.
echo === Smoketest finished ===
endlocal
