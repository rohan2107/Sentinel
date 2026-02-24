@echo off
REM Pre-Commit Checklist Batch Wrapper
REM Runs PowerShell pre-commit validation script
REM Usage: pre-commit-checklist.bat [options]

setlocal enabledelayedexpansion

cd /d "%~dp0\.."

echo Running Pre-Commit Checklist...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\pre-commit-checklist.ps1" %*

exit /b %ERRORLEVEL%
