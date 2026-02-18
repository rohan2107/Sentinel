@echo off
REM scripts\test.bat
REM Run integration tests (batch wrapper for PowerShell script)

setlocal

set CONFIG=Debug
if not "%1"=="" set CONFIG=%1

echo Running integration tests (config: %CONFIG%)...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0test.ps1" -Config %CONFIG%

exit /b %ERRORLEVEL%
