@echo off
REM Robust scripts\build.bat that works when invoked from project root or from scripts\

REM Resolve the directory of this script (with trailing backslash)
set "SCRIPT_DIR=%~dp0"
REM Trim trailing backslash for some operations if needed (not necessary here)

REM Project root is parent of scripts directory
set "PROJECT_ROOT=%SCRIPT_DIR%.."

REM Allow optional BUILD_TYPE as first argument (default Debug)
set "BUILD_TYPE=%1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

REM Ensure we are using the right vcvars if cl missing
where cl >nul 2>&1
if errorlevel 1 (
  if exist "C:\tools\vcvars64_wrapper.bat" (
    call "C:\tools\vcvars64_wrapper.bat"
  ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  )
)

REM Configure & build using the script-dir relative paths
cmake -S "%PROJECT_ROOT%" -B "%PROJECT_ROOT%\build" -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -A x64 -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 (
  echo Error: Could not configure project.
  exit /b 1
)

cmake --build "%PROJECT_ROOT%\build" --config %BUILD_TYPE%
if errorlevel 1 (
  echo Build failed
  exit /b 1
)

echo Build succeeded
exit /b 0
