@echo off
REM scripts\build.bat
REM Usage: build.bat [Debug|Release]
REM Default: Debug (matches your existing layout). Use Release for Release builds.

REM Resolve the directory of this script (with trailing backslash)
set "SCRIPT_DIR=%~dp0"
REM Project root is parent of scripts directory
set "PROJECT_ROOT=%SCRIPT_DIR%.."

REM Allow optional BUILD_TYPE as first argument (default Debug)
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

echo Building Sentinel (config: %BUILD_TYPE%)...
REM Ensure we have MSVC tools (cl) available; if not attempt to call common vcvars locations
where cl >nul 2>&1
if errorlevel 1 (
  echo cl.exe not found on PATH. Attempting to call vcvars...
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\tools\vcvars64_wrapper.bat" (
    call "C:\tools\vcvars64_wrapper.bat"
  ) else (
    echo ERROR: Could not find vcvars64.bat. Open a Developer Command Prompt or install Visual Studio Build Tools.
    exit /b 1
  )
)

REM Configure & build using cmake (vcpkg toolchain if available)
cmake -S "%PROJECT_ROOT%" -B "%PROJECT_ROOT%\build" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -A x64 -D CMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
if errorlevel 1 (
  echo Error: Could not configure project with CMake.
  exit /b 1
)

cmake --build "%PROJECT_ROOT%\build" --config %BUILD_TYPE%
if errorlevel 1 (
  echo Build failed
  exit /b 1
)

echo Build succeeded (config: %BUILD_TYPE%).
exit /b 0
