#!/usr/bin/env bash
# scripts/smoketest.sh — build, run against the host's policy, and inspect the
# resulting report and database row. macOS/Linux equivalent of smoketest.ps1.
#
# Usage: ./scripts/smoketest.sh

set -euo pipefail

case "${1:-}" in
  -h|--help)
    sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    echo "Unknown argument: $1" >&2
    echo "Usage: $0" >&2
    exit 2
    ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

echo "═══════════════════════════════════════════════════"
echo "        Sentinel Smoketest"
echo "═══════════════════════════════════════════════════"
echo "Timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
echo

echo "──────────────────────────────────────────────────"
echo "Step 1: Build (Release)"
echo "──────────────────────────────────────────────────"
"$SCRIPT_DIR/build.sh" --config Release
echo

echo "──────────────────────────────────────────────────"
echo "Step 2: Run agent"
echo "──────────────────────────────────────────────────"
# The agent exits non-zero when osqueryi is missing or rules fail collection,
# which is expected on a machine without osquery. Keep going so the report and
# database steps still report what did happen.
set +e
"$SCRIPT_DIR/run.sh" --config Release
RUN_EXIT=$?
set -e
echo

echo "──────────────────────────────────────────────────"
echo "Step 3: Latest report"
echo "──────────────────────────────────────────────────"
REPORT="$PROJECT_ROOT/reports/latest_report.json"
if [[ -f "$REPORT" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    python3 -m json.tool "$REPORT"
    echo
    python3 - "$REPORT" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
print("Report validation:")
for key in ("policy", "score", "hostname", "timestamp"):
    if key in r:
        print(f"  [OK] {key}: {r[key]}")
    else:
        print(f"  [MISSING] {key}")
PY
  else
    cat "$REPORT"
  fi
else
  echo "WARNING: report not found at $REPORT" >&2
fi
echo

echo "──────────────────────────────────────────────────"
echo "Step 4: Database verification"
echo "──────────────────────────────────────────────────"
DB="$PROJECT_ROOT/sentinel_data.sqlite3"
if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "--- skipped (sqlite3 not on PATH)"
elif [[ ! -f "$DB" ]]; then
  echo "WARNING: database not found at $DB" >&2
else
  echo "Last run row:"
  sqlite3 -header -column "$DB" \
    "SELECT id, ts, policy, score FROM runs ORDER BY id DESC LIMIT 1;"
  echo
  echo "Retry queue state counts:"
  sqlite3 -header -column "$DB" \
    "SELECT state, COUNT(*) AS n FROM retry_queue GROUP BY state;"
fi
echo

echo "═══════════════════════════════════════════════════"
if [[ $RUN_EXIT -eq 0 ]]; then
  echo "        Smoketest completed successfully"
else
  echo "        Smoketest finished; agent exited $RUN_EXIT"
  echo "        (expected when osqueryi is not installed)"
fi
echo "═══════════════════════════════════════════════════"
exit 0
