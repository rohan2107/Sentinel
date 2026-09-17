#!/usr/bin/env bash
# scripts/build.sh — configure and build Sentinel on macOS or Linux.
#
# Usage: ./scripts/build.sh [--config Debug|Release]
#
# Defaults to Release, which differs from build.ps1's Debug default. Reason:
# Release is what CI validates, and the *-release presets put the binary in
# build/ (the path the README and run.sh use). Debug lands in build-debug/
# because single-config generators cannot share one directory the way the
# Visual Studio generator does on Windows.

set -euo pipefail

CONFIG="Release"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      CONFIG="${2:-}"
      shift 2
      ;;
    -h|--help)
      sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--config Debug|Release]" >&2
      exit 2
      ;;
  esac
done

if [[ "$CONFIG" != "Debug" && "$CONFIG" != "Release" ]]; then
  echo "ERROR: --config must be Debug or Release (got '$CONFIG')" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

case "$(uname -s)" in
  Darwin) PLATFORM="macos" ;;
  Linux)  PLATFORM="linux" ;;
  *)
    echo "ERROR: $(uname -s) is not supported by this script." >&2
    echo "       On Windows use scripts\\build.ps1 instead." >&2
    exit 1
    ;;
esac

PRESET="${PLATFORM}-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake not found on PATH." >&2
  if [[ "$PLATFORM" == "macos" ]]; then
    echo "       brew install cmake" >&2
  else
    echo "       apt install cmake" >&2
  fi
  exit 1
fi

echo "Building Sentinel (preset: $PRESET)..."
cd "$PROJECT_ROOT"

cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Build succeeded (config: $CONFIG)"
