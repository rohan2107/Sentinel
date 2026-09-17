#!/usr/bin/env bash
# scripts/run.sh — run the Sentinel agent against a policy.
#
# Usage: ./scripts/run.sh [--policy PATH] [--config Debug|Release] [-- AGENT_ARGS...]
#
# With no --policy, selects the policy matching the host OS: macos_policy.json
# on Darwin, sample_policy.json (Windows baseline) elsewhere. Policies are not
# portable — they query platform-specific osquery tables.
#
# Anything after `--` is forwarded to the agent, e.g.:
#   ./scripts/run.sh -- --enable-delivery --backend-url http://localhost:8000

set -euo pipefail

CONFIG="Release"
POLICY=""
AGENT_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --policy)
      POLICY="${2:-}"
      shift 2
      ;;
    --config)
      CONFIG="${2:-}"
      shift 2
      ;;
    --)
      shift
      AGENT_ARGS=("$@")
      break
      ;;
    -h|--help)
      sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

case "$(uname -s)" in
  Darwin) DEFAULT_POLICY="policies/macos_policy.json" ;;
  Linux)  DEFAULT_POLICY="policies/sample_policy.json" ;;
  *)
    echo "ERROR: $(uname -s) is not supported by this script." >&2
    exit 1
    ;;
esac

POLICY="${POLICY:-$DEFAULT_POLICY}"
[[ "$POLICY" = /* ]] || POLICY="$PROJECT_ROOT/$POLICY"

if [[ "$CONFIG" == "Release" ]]; then
  BIN_DIR="$PROJECT_ROOT/build"
else
  BIN_DIR="$PROJECT_ROOT/build-debug"
fi
EXE="$BIN_DIR/Sentinel"

if [[ ! -x "$EXE" ]]; then
  echo "ERROR: agent binary not found at $EXE" >&2
  echo "       Run ./scripts/build.sh --config $CONFIG first." >&2
  exit 1
fi

if [[ ! -f "$POLICY" ]]; then
  echo "ERROR: policy file not found: $POLICY" >&2
  exit 1
fi

echo "Policy:     $POLICY"
echo "Binary:     $EXE"

# osquery is a runtime dependency, not a build one: the agent shells out to
# osqueryi. Without it every rule fails collection and the score reads 0,
# which looks like a posture failure rather than a missing tool.
if command -v osqueryi >/dev/null 2>&1; then
  echo "osqueryi:   $(command -v osqueryi)"
else
  echo "osqueryi:   NOT FOUND" >&2
  echo >&2
  echo "WARNING: osqueryi is not on PATH. Every rule will fail data collection" >&2
  echo "         and the reported score will be 0 for that reason, not because" >&2
  echo "         the host is misconfigured." >&2
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "         Install with: brew install --cask osquery" >&2
  else
    echo "         See https://osquery.io/downloads" >&2
  fi
  echo >&2
fi

echo "────────────────────────────────────────────────────────────────"

# Run from the project root: the agent writes reports/latest_report.json and
# sentinel_data.sqlite3 relative to its working directory.
cd "$PROJECT_ROOT"
set +e
"$EXE" --policy "$POLICY" ${AGENT_ARGS+"${AGENT_ARGS[@]}"}
EXIT_CODE=$?
set -e

echo "────────────────────────────────────────────────────────────────"
if [[ $EXIT_CODE -eq 0 ]]; then
  echo "Sentinel completed successfully"
else
  echo "Sentinel exited with code $EXIT_CODE" >&2
fi
exit $EXIT_CODE
