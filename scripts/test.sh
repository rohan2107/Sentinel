#!/usr/bin/env bash
# scripts/test.sh — run the C++ quality checks on macOS or Linux.
# Mirrors scripts/test.ps1 and the CI jobs: build, integration tests via ctest,
# policy JSON validation, and the backend syntax check.
#
# Usage: ./scripts/test.sh [--config Debug|Release|Both]

set -euo pipefail

CONFIG="Release"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

case "$(uname -s)" in
  Darwin) PLATFORM="macos" ;;
  Linux)  PLATFORM="linux" ;;
  *) echo "ERROR: $(uname -s) unsupported; use scripts\\test.ps1 on Windows." >&2; exit 1 ;;
esac

case "$CONFIG" in
  Both)          CONFIGS=("Debug" "Release") ;;
  Debug|Release) CONFIGS=("$CONFIG") ;;
  *) echo "ERROR: --config must be Debug, Release or Both (got '$CONFIG')" >&2; exit 2 ;;
esac

fail() {
  echo
  echo "==================================================="
  echo "  FAILED: $1"
  echo "==================================================="
  exit 1
}

echo "==================================================="
echo "        Sentinel Integration Tests"
echo "==================================================="
echo "Platform:  $PLATFORM"
echo "Config:    ${CONFIGS[*]}"
echo "Timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
echo

for cfg in "${CONFIGS[@]}"; do
  preset="${PLATFORM}-$(echo "$cfg" | tr '[:upper:]' '[:lower:]')"
  echo "---------------------------------------------------"
  echo "Config: $cfg (preset: $preset)"
  echo "---------------------------------------------------"

  cmake --preset "$preset" >/dev/null || fail "cmake configure ($cfg)"
  cmake --build --preset "$preset" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
    || fail "build ($cfg)"

  # Debug has no testPreset (ctest presets are defined for release only), so
  # drive ctest by build directory instead of by preset name.
  if [[ "$cfg" == "Release" ]]; then
    bin_dir="$PROJECT_ROOT/build"
  else
    bin_dir="$PROJECT_ROOT/build-debug"
  fi
  ctest --test-dir "$bin_dir" --output-on-failure || fail "tests ($cfg)"

  echo "[OK] tests ($cfg)"
  echo
done

# --- Policy JSON validation (mirrors the validate-backend CI job) -----------
echo "---------------------------------------------------"
echo "Policy JSON validation"
echo "---------------------------------------------------"
if command -v python3 >/dev/null 2>&1; then
  python3 - <<'PY' || fail "policy JSON validation"
import json, pathlib, sys
errors = 0
for p in sorted(pathlib.Path("policies").glob("*.json")):
    try:
        data = json.loads(p.read_text())
        assert "policy_name" in data or "rules" in data, "missing policy_name or rules key"
        print(f"  [OK] {p}")
    except Exception as e:
        print(f"  [FAIL] {p}: {e}", file=sys.stderr)
        errors += 1
sys.exit(1 if errors else 0)
PY
  echo "[OK] all policy files valid"
else
  echo "--- skipped (python3 not on PATH)"
fi
echo

# --- Backend syntax check (mirrors the validate-backend CI job) -------------
echo "---------------------------------------------------"
echo "Backend syntax check"
echo "---------------------------------------------------"
if [[ ! -f backend/server.py ]]; then
  echo "--- skipped (backend/server.py not found)"
elif command -v python3 >/dev/null 2>&1; then
  python3 -m py_compile backend/server.py || fail "backend/server.py syntax check"
  echo "[OK] backend/server.py"
else
  echo "--- skipped (python3 not on PATH)"
fi
echo

echo "==================================================="
echo "  ALL TESTS PASSED"
echo "==================================================="
