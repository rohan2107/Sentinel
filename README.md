# Sentinel

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Sentinel is a security compliance agent: it collects host state with osquery, evaluates it against policy rules written in Lua, and delivers the result with at-least-once, deduplicated semantics.**

Local evaluation runs standalone with no network dependency; delivery is opt-in via `--enable-delivery`. See [architecture/README.md](architecture/README.md) for how the system actually works, what it guarantees, and its known gaps — the honest, detailed counterpart to this overview.

---

## What's Implemented

- **Policy evaluation**: osquery data collection + Lua rule engine + weighted scoring
- **Crash-safe persistence**: SQLite with WAL mode, atomic transactions
- **Restricted rule execution**: Lua 5.4 with only `base`, `table`, `string` and
  `math` opened, so there is no file, process or network access. This is a
  restricted library surface, **not** a CPU bound and not a security boundary —
  there is currently no Lua timeout, so policy authorship must be trusted (the
  top item on the [roadmap](docs/ROADMAP.md))
- **Resource bounds**: osquery 10s wall time (enforced on Windows; best-effort on
  POSIX) and a 1MB output cap. See
  [architecture/README.md](architecture/README.md#resource-bounds) for exactly
  what is and is not enforced
- **State-change-triggered delivery**: two hashes — `posture_hash` (policy,
  score, details) decides whether to report at all; `report_hash` (the whole
  report, timestamp included) identifies the specific report on the wire and is
  what both receivers verify
- **At-least-once HTTP delivery**: retry queue with exponential backoff (1s →
  300s, jittered), crash recovery on restart, idempotent enqueue
- **Cross-language hash verification**: the report hash is a wire contract
  between three independently written JSON encoders (C++, Python, Go); a
  [differential test](tests/canonicalization/README.md) checks they agree
  byte-for-byte, because by default they don't

**Database schema (current):**
```sql
CREATE TABLE runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT NOT NULL,
  hostname TEXT,
  policy TEXT,
  score INTEGER,
  details_json TEXT
);

CREATE TABLE rule_results (
  run_id INTEGER NOT NULL,
  rule_id TEXT NOT NULL,
  passed INTEGER NOT NULL,
  weight INTEGER NOT NULL,
  PRIMARY KEY (run_id, rule_id),
  FOREIGN KEY(run_id) REFERENCES runs(id)
);

CREATE TABLE retry_queue (
  run_id INTEGER PRIMARY KEY,
  report_hash TEXT UNIQUE NOT NULL,  -- event hash: whole report, incl. timestamp
  posture_hash TEXT,                 -- posture only: policy/score/details
  report_json TEXT NOT NULL,
  attempts INTEGER DEFAULT 0,
  state TEXT NOT NULL CHECK (state IN ('PENDING', 'DELIVERED', 'FAILED')),
  next_retry_at TEXT,
  created_at TEXT NOT NULL,
  delivered_at TEXT,
  failed_at TEXT,
  last_error TEXT,
  FOREIGN KEY(run_id) REFERENCES runs(id)
);
```

The delivery layer is HTTP-only today — no TLS (terminate at a reverse proxy
if needed) and no authentication. `DeliveryClient` is an abstract interface
specifically so a second transport (MQTT is the plan — see
[docs/ROADMAP.md](docs/ROADMAP.md)) can be added without touching the retry
queue, scoring or persistence.

To run the backend and try delivery locally:
```bash
cd backend
pip install -r requirements.txt
uvicorn server:app --reload --port 8000
```
Then run the agent with `--enable-delivery` — see [Run](#run) below for the
per-platform command.

---

## Capabilities

### Policy Evaluation

Policies define security rules using osquery for data collection and Lua for evaluation logic:

```json
{
  "policy_name": "windows-security-baseline",
  "base_score": 100,
  "rules": [
    {
      "id": "firewall_enabled",
      "desc": "Windows Firewall must be enabled",
      "query": "SELECT state FROM windows_security_products WHERE type='Firewall'",
      "lua": "return results[1].state == 'On'",
      "weight": 20
    }
  ]
}
```

### Execution Flow

1. **Load Policy**: Parse JSON policy file, validate schema
2. **Collect Data**: Execute osquery with 10s timeout
3. **Evaluate Rules**: Run Lua code with a restricted library surface (no
   timeout — see [known gaps](architecture/README.md#known-gaps))
4. **Compute Score**: `base_score` minus the weight of each failed rule,
   clamped to 0-100
5. **Persist**: Atomic SQLite transaction (WAL mode)
6. **Report**: Write JSON to `reports/latest_report.json`

### Example Output

```json
{
  "policy": "sample-default-windows",
  "score": 75,
  "details": {
    "firewall_enabled": true,
    "av_installed": true,
    "security_center_status": false
  },
  "timestamp": "2026-02-18T10:30:00.123Z",
  "hostname": "DESKTOP-WIN10"
}
```

What the system guarantees — and, as importantly, what it doesn't (no
ordering, no exactly-once, no liveness signal, a real unbounded-growth path in
the retry queue) — is documented in
[architecture/README.md](architecture/README.md#delivery), not duplicated
here: keeping one authoritative copy is how this stays accurate as the code
changes.

---

## Quick Start

### Platform Support

| Platform | Build | Agent runs | CI | Policy |
|----------|-------|-----------|----|--------|
| macOS (Apple Silicon / Intel) | Homebrew + CMake | Yes | [`macos-build.yml`](.github/workflows/macos-build.yml) — every push/PR | [`policies/macos_policy.json`](policies/macos_policy.json) |
| Windows (x64) | vcpkg + MSBuild | Yes | [`windows-build.yml`](.github/workflows/windows-build.yml) — every push/PR | [`policies/sample_policy.json`](policies/sample_policy.json) |
| Linux | Homebrew/apt + CMake | Untested | None | — |

Presets exist for Linux and the agent source is POSIX-clean, but it has not been
built or run there, so it is listed as untested rather than supported.

Both supported platforms build on every push and PR to `master`, not just
when someone remembers to check. The agent genuinely is cross-platform — only
about 10% of its source sits inside `#ifdef _WIN32`, almost entirely in
`osquery_runner.cpp`'s process-spawning code, which necessarily differs by
OS — and continuous CI on both is what turns that from an intention into a
verified claim. Active development is macOS-only, which means Windows CI is
the only thing that would catch a POSIX-only regression introduced from that
work before it ships. See
[architecture/README.md](architecture/README.md#platform-and-dependencies).

Policies are **not** portable across platforms: each rule carries an osquery
query, and the tables differ (`windows_security_products` vs `alf`,
`gatekeeper`, `disk_encryption`, `sip_config`). The Lua evaluation logic is
portable; the queries are not.

### Prerequisites

**Windows**

- **Visual Studio 2022** with C++ build tools
- **[vcpkg](https://github.com/microsoft/vcpkg)** with packages:
  ```
  vcpkg install nlohmann-json spdlog sol2 lua sqlite3
  ```
- **[osquery](https://osquery.io/downloads/official)** with `osqueryi.exe` in `PATH`

**macOS**

```bash
brew install cmake lua@5.4 spdlog nlohmann-json sol2 cpp-httplib
brew install --cask osquery   # requires admin rights (installs a .pkg)
```

`lua@5.4` is keg-only and deprecated in Homebrew, but it is deliberate: vcpkg
provides Lua 5.4 on Windows, and policy rules are Lua source shipped as data, so
the two platforms must agree on the language version. CMake runs an ABI probe at
configure time and fails the build if the Lua headers and the linked library
disagree — see the note in [`CMakeLists.txt`](CMakeLists.txt) for why that check
exists.

osquery is a **runtime** dependency, not a build one. Without it the agent still
builds, runs, and persists, but every rule fails data collection and the score
reads 0 for that reason rather than because the host is misconfigured. Both test
suites are osquery-independent.

### Build

**Windows** (PowerShell 7):
```powershell
.\scripts\build.ps1 -Config Debug
```

**macOS / Linux**:
```bash
./scripts/build.sh                    # Release (default) -> build/
./scripts/build.sh --config Debug     # Debug           -> build-debug/
```

Or drive CMake directly:
```bash
cmake --preset macos-release
cmake --build --preset macos-release
```

The shell scripts default to Release where `build.ps1` defaults to Debug:
Release is what CI validates and it lands in `build/`, the path the run scripts
use. Single-config generators cannot share one build directory between
configurations the way the Visual Studio generator does, hence the separate
`build-debug/`.

### Run

**Windows** (PowerShell 7):
```powershell
.\scripts\run.ps1 -Policy policies\sample_policy.json
```

**macOS / Linux**:
```bash
./scripts/run.sh                                        # picks the host's policy
./scripts/run.sh --policy policies/macos_policy.json
./scripts/run.sh -- --enable-delivery --backend-url http://localhost:8000
```

Arguments after `--` are forwarded to the agent.

### Smoketest

**Windows**:
```powershell
.\scripts\smoketest.ps1
```

**macOS / Linux**:
```bash
./scripts/smoketest.sh
```

Validates: build succeeds, execution completes, report generated, database
persisted, retry-queue state readable.

---

## Testing & CI

### Before committing

```bash
./scripts/test.sh --config Both      # macOS / Linux
.\scripts\test.ps1                   # Windows
```

That mirrors what CI runs: build both configurations, both test binaries, policy
JSON validation, the canonicalization differential test, and the backend syntax
check.

### Integration Tests

**Windows** (PowerShell 7):
```powershell
.\scripts\test.ps1
```

**macOS / Linux**:
```bash
./scripts/test.sh                  # Release
./scripts/test.sh --config Both    # Debug and Release
```

**Or via ctest**:
```bash
ctest --preset macos-release
```

There are two test binaries, both of which run without osquery:

`test_delivery_foundation` — durable delivery:
- SHA-256 hash determinism (sorted JSON keys)
- MockDeliveryClient success/failure modes
- Retry queue operations (enqueue, load, mark delivered/failed)
- UNIQUE constraint on report_hash
- Dynamic timestamp handling (prevents test decay)
- End-to-end flow (persist → hash → enqueue → deliver)

`test_lua_evaluator` — the sandboxed rule engine:
- Each rule in `policies/macos_policy.json` driven against synthetic osquery
  rows, asserting both its pass and its fail path
- Empty result sets (a failed or empty osquery query must not pass a rule)
- JSON `null` column values, which exercise the `sol::lua_nil` path

The Lua suite is cross-platform despite using the macOS policy file: the policy
is data, and its Lua snippets evaluate identically wherever the agent is built.
It feeds rows directly to the evaluator rather than invoking osquery, so it
tests rule logic in isolation from data collection.

### Canonicalization Differential Test

The report hash is a wire-format contract between three independently written
JSON encoders: the C++ agent produces the hash, and both the Python backend and
the Go aggregator recompute it to verify. They do **not** agree by default -
they differ on non-ASCII escaping, HTML escaping and number formatting - and a
disagreement does not fail loudly, it makes every affected report return
`400 hash mismatch` indefinitely.

```bash
cmake --build --preset macos-release --target canon_dump
python3 tests/canonicalization/compare.py
```

Each emitter calls its project's real canonicalizer, not a copy. Runs in the
macOS CI job, the only one with all three toolchains. See
[tests/canonicalization/README.md](tests/canonicalization/README.md) for the
contract, the two documented cases that lie outside it, and why.

### Continuous Integration

| Workflow | Runner | Trigger | Covers |
|----------|--------|---------|--------|
| [`macos-build.yml`](.github/workflows/macos-build.yml) | `macos-latest` | every push/PR | Homebrew build, ctest, the canonicalization differential test, agent start-up check, artifact upload |
| [`windows-build.yml`](.github/workflows/windows-build.yml) | `windows-latest` | every push/PR | vcpkg build, both test binaries, artifact upload |
| [`validate-backend.yml`](.github/workflows/validate-backend.yml) | `ubuntu-latest` | every push/PR touching `backend/` or `policies/` | backend syntax check **and import check** (imports `server.py`, so a FastAPI startup error is caught — syntax-only checking missed exactly this bug once), policy JSON validation |
| [`go-aggregator.yml`](.github/workflows/go-aggregator.yml) | `ubuntu-latest` | every push/PR touching `go-aggregator/` | build, vet, `go test -race`, golangci-lint, govulncheck |

`validate-backend` used to be a second job inside `windows-build.yml`, despite
having nothing to do with Windows — a coupling that would have mattered a lot
if Windows CI had gone manual-only, which it briefly did before reverting
back to always-on below. Kept split out regardless: it is better hygiene for
each workflow to own one concern, matching how the other three are already
separated by platform/component rather than bundled.

---

## Architecture

Kept in one place — [architecture/README.md](architecture/README.md) — rather
than duplicated here: system diagram, execution order, the two-hash
suppression design, delivery state machine, data model, resource bounds, and
a **Known gaps** section that lists what doesn't work yet as plainly as this
page lists what does.

Performance numbers aren't included because none have been measured against
the current implementation — see
[docs/ROADMAP.md](docs/ROADMAP.md#5-measure-the-traffic-reduction-properly)
for the plan to produce a real one, with its methodology stated up front
rather than a number asserted after the fact.

---

## Project Structure

```
Sentinel/
├── src/
│   ├── main.cpp                  # Orchestration, CLI, delivery integration
│   ├── osquery_runner.cpp        # osquery execution with timeout
│   ├── lua_evaluator.cpp         # Sandboxed Lua runtime
│   ├── scoring.cpp               # Weighted score computation
│   ├── db.cpp                    # SQLite persistence (WAL mode)
│   ├── report_hasher.cpp         # SHA-256 content hashing
│   ├── delivery_client.cpp       # DeliveryClient interface + MockDeliveryClient
│   ├── http_delivery_client.cpp  # HTTP POST delivery (cpp-httplib)
│   └── retry_queue.cpp           # Retry manager with exponential backoff
├── backend/
│   ├── server.py                 # FastAPI backend with hash deduplication
│   ├── canonical.py              # Canonical JSON + hash (stdlib only, no app deps)
│   ├── requirements.txt          # Python dependencies
│   └── README.md                 # Backend API documentation
├── policies/
│   ├── sample_policy.json        # Windows baseline (windows_security_products/_center)
│   └── macos_policy.json         # macOS baseline (alf, gatekeeper, disk_encryption, sip_config)
├── architecture/
│   └── README.md                 # System architecture documentation
├── docs/
│   ├── trade-offs.md             # Decision matrix with failure thresholds
│   └── ROADMAP.md                # Short- and long-term plan
├── tests/
│   ├── test_delivery_foundation.cpp  # Delivery integration tests
│   ├── test_lua_evaluator.cpp        # Rule engine tests (no osquery required)
│   └── canonicalization/             # Cross-language report-hash contract test
│       ├── fixtures.json             # Shared fixtures, single source of truth
│       ├── canon_dump.cpp/.py        # C++ and Python emitters
│       ├── compare.py                # Driver: runs all three, diffs them
│       └── golden.json               # Pinned hashes, catches coordinated drift
└── scripts/
    ├── build.ps1 / build.sh
    ├── run.ps1 / run.sh
    ├── test.ps1 / test.sh
    └── smoketest.ps1 / smoketest.sh
```

---

## Documentation

| Document | Description |
|----------|-------------|
| **[README.md](README.md)** | Overview, quick start, testing |
| **[architecture/README.md](architecture/README.md)** | How the system works, what it guarantees, and its known gaps |
| **[docs/ROADMAP.md](docs/ROADMAP.md)** | What is next, short and long term |
| **[docs/trade-offs.md](docs/trade-offs.md)** | Decisions, alternatives, and the thresholds where each choice fails |
| **[tests/canonicalization/README.md](tests/canonicalization/README.md)** | The cross-language report-hash contract |
| **[scripts/README.md](scripts/README.md)** | Build, run and test scripts |
| **[backend/README.md](backend/README.md)** | Backend API reference |

---

## License

MIT License - see [LICENSE](LICENSE) file.
