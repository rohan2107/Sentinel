# Sentinel

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Sentinel is a security compliance evaluation agent demonstrating deterministic policy evaluation, durable persistence, and clean architectural patterns.**

This project showcases engineering thinking for agent-based systems: crash-safe persistence (SQLite WAL), sandboxed rule execution (Lua), and separation of concerns. It's structured as a two-phase implementation to demonstrate both working code (Phase 1) and architectural planning (Phase 2 includes explicit state machines for delivery).

---

## Project Status

### Phase 1: Local Evaluation Engine ✅ **IMPLEMENTED**

Currently built and working:

- **Deterministic Policy Evaluation**: osquery data collection + Lua rule engine + weighted scoring
- **Crash-Safe Persistence**: SQLite with WAL mode, atomic transactions
- **Sandboxed Rule Execution**: Lua runtime with timeout enforcement (1s), no I/O access
- **Resource Bounds**: osquery timeout (10s), memory limits, 1MB output cap
- **Structured Reporting**: JSON output with ISO-8601 timestamps, hostname detection
- **Clean Architecture**: Separation between data collection, rule evaluation, scoring, persistence

**Database Schema (Current):**
```sql
CREATE TABLE runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT NOT NULL,
  hostname TEXT,
  policy TEXT,
  score INTEGER,
  details_json TEXT
);

CREATE TABLE features (
  run_id INTEGER PRIMARY KEY,
  firewall_enabled INTEGER,
  av_installed INTEGER,
  FOREIGN KEY(run_id) REFERENCES runs(id)
);

-- Phase 2: Delivery layer
CREATE TABLE retry_queue (
  run_id INTEGER PRIMARY KEY,
  report_hash TEXT UNIQUE NOT NULL,
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

### Phase 2: Delivery Layer 🔄 **PARTIAL**

**Delivery Foundation** ✅ **IMPLEMENTED** (Merged to master)
- ✅ Retry queue database schema (3-state: PENDING/DELIVERED/FAILED)
- ✅ SHA-256 content hashing (standalone implementation, no OpenSSL)
- ✅ DeliveryClient interface with abstract base class
- ✅ MockDeliveryClient for testing
- ✅ Queue operations: enqueue_report, load_pending_reports, mark_delivered, mark_failed, update_retry
- ✅ Integration test suite (all tests passing)
- ✅ Defensive NULL checks for nullable fields
- ✅ Dynamic timestamp handling (prevents test decay)
- ✅ UNIQUE constraint enforcement on report_hash

**Remaining Work** (See [`docs/roadmap/`](docs/roadmap/) for implementation plans):

| Feature | Complexity | Estimate | Status |
|---------|-----------|----------|--------|
| HTTP Delivery Client | Low | 4-5 hours | Planned |
| MQTT Delivery Client (QoS 1) | Medium | 6-8 hours | Planned |
| RetryQueue Manager + Exponential Backoff | Low | 6-7 hours | Planned |
| main.cpp Integration | Low | 4-5 hours | Planned |
| FastAPI Backend (hash deduplication) | Low | 4-5 hours | Planned |

**Total Remaining:** ~25-30 hours (3-4 days focused work)

---

## Current Capabilities (Phase 1)

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

### Execution Flow (Current)

1. **Load Policy**: Parse JSON policy file, validate schema
2. **Collect Data**: Execute osquery with 10s timeout
3. **Evaluate Rules**: Run Lua code in sandboxed environment (1s timeout)
4. **Compute Score**: Weighted sum of passing rules
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

### Current Guarantees

**Implemented (Phase 1 + Delivery Foundation):**

- ✅ **Deterministic Evaluation**: Same policy + same system state = same score
- ✅ **Crash-Safe Persistence**: SQLite WAL ensures committed data survives crashes  
- ✅ **Sandboxed Execution**: Lua runtime has no file/network I/O, enforced timeouts
- ✅ **Resource Bounds**: CPU/memory/disk usage limited by timeouts and output caps
- ✅ **Offline Operation**: Agent works without network (local evaluation only)
- ✅ **Content Hashing**: SHA-256 hashing for report deduplication (standalone implementation)
- ✅ **Delivery Queue Schema**: Durable retry_queue with 3-state machine
- ✅ **Idempotent Deduplication**: UNIQUE constraint on report_hash prevents duplicates

**Not Yet Implemented (Planned Phase 2):**

- ⏳ **Network Delivery**: No HTTP/MQTT clients exist yet
- ⏳ **Retry Logic**: Exponential backoff not yet wired into main.cpp
- ⏳ **Crash Recovery on Startup**: load_pending_reports() exists but not called
- ⏳ **Backend Server**: No endpoint to receive reports

---

## Quick Start

### Prerequisites

- **Visual Studio 2022** with C++ build tools (Windows)
- **[vcpkg](https://github.com/microsoft/vcpkg)** with packages:
  ```
  vcpkg install nlohmann-json spdlog sol2 lua sqlite3
  ```
- **[osquery](https://osquery.io/downloads/official)** installed and `osqueryi.exe` in `PATH`

**Optional Tools:**
- **sqlite3 CLI** (for manual database inspection): `winget install SQLite.SQLite`
  - Not required for building or testing
  - Tests validate schema automatically

### Build

**PowerShell 7** (recommended):
```powershell
.\scripts\build.ps1 -Config Debug
```

**Command Prompt**:
```bat
scripts\build.bat
```

### Run

**PowerShell 7**:
```powershell
.\scripts\run.ps1 -Policy policies\sample_policy.json
```

**Command Prompt**:
```bat
scripts\run.bat policies\sample_policy.json
```

### Smoketest

```powershell
.\scripts\smoketest.ps1
```

Validates: Build succeeds, execution completes, report generated, database persisted.

---

## Testing & CI

### Quick Validation (5 minutes)

Before committing, run the three core checks:

```powershell
.\scripts\build.ps1   # Clean build
.\scripts\test.ps1    # Integration tests
.\scripts\run.ps1     # Phase 1 backward compatibility
```

**See:** [docs/QUICK_TEST.md](docs/QUICK_TEST.md) for quick reference.

### Comprehensive Pre-Commit Testing (10-15 minutes)

Before merging to master, run full validation including:
- Build integrity (Debug + Release)
- Edge case testing
- CLI argument variations
- Database schema verification

**See:** [docs/PRE_COMMIT_TESTING.md](docs/PRE_COMMIT_TESTING.md) for complete checklist.

### Integration Tests

Run delivery foundation integration tests:

**PowerShell 7**:
```powershell
.\scripts\test.ps1
```

**Command Prompt**:
```bat
scripts\test.bat
```

**Or directly**:
```powershell
.\build\Debug\test_delivery_foundation.exe
```

**Tests cover:**
- SHA-256 hash determinism (sorted JSON keys)
- MockDeliveryClient success/failure modes
- Retry queue operations (enqueue, load, mark delivered/failed)
- UNIQUE constraint on report_hash
- Dynamic timestamp handling (prevents test decay)
- End-to-end flow (persist → hash → enqueue → deliver)

### Continuous Integration

GitHub Actions workflow runs on every push/PR to master:
- ✅ Build (Release + Debug)
- ✅ Run integration tests
- ✅ Upload build artifacts

See [`.github/workflows/windows-build.yml`](.github/workflows/windows-build.yml) for details.

---

## Architecture (Current Implementation)

```mermaid
graph LR
    A[main.cpp] --> B[Policy Validator]
    A --> C[osquery Runner<br/>10s timeout]
    A --> D[Lua Evaluator<br/>1s timeout]
    A --> E[Scoring Engine]
    A --> F[(SQLite + WAL)]
    A --> I[JSON Report Writer]
    
    F --> G[runs table]
    F --> H[features table]
    F --> J[retry_queue table<br/>PENDING/DELIVERED/FAILED]
    
    K[DeliveryClient<br/>Interface] --> L[MockDeliveryClient]
    K -.planned.-> M[HttpDeliveryClient]
    K -.planned.-> N[MqttDeliveryClient]
    
    O[report_hasher] --> P[SHA-256<br/>standalone]
    
    style F fill:#e1f5ff
    style C fill:#fff3e0
    style D fill:#fff3e0
    style J fill:#d4edda
    style K fill:#d4edda
    style O fill:#d4edda
```

**Legend:**
- Solid boxes: Implemented
- Dashed boxes: Planned for completion
- Blue: Persistence layer
- Orange: External process execution
- Green: Delivery foundation (Phase 2 partial)

**See:** [architecture/README.md](architecture/README.md) for detailed component descriptions.

---

## Performance Characteristics (Phase 1)

**Observed during local testing (Windows 10, i7-8750H, 16GB RAM):**

- **CPU**: 3% average, 25% peak (during osquery execution)
- **Memory**: 45MB RSS
- **Disk**: ~5ms write latency (SQLite WAL mode)
- **Execution Time**:
  - Policy validation: <1ms
  - osquery execution: 50-200ms (query-dependent)
  - Lua evaluation: <1ms per rule
  - SQLite transaction: 1-5ms

**Capacity (Single Agent):**
- SQLite: ~10k writes/sec (far exceeds single-agent needs)
- Evaluation rate: Limited by policy complexity and osquery queries (typically 1-10/min suffices)

---

## Project Structure

```
Sentinel/
├── src/
│   ├── main.cpp              # Orchestration and evaluation flow
│   ├── osquery_runner.cpp    # osquery execution with timeout
│   ├── lua_evaluator.cpp     # Sandboxed Lua runtime
│   ├── scoring.cpp           # Weighted score computation
│   ├── db.cpp                # SQLite persistence (WAL mode)
│   └── json_to_lua.cpp       # JSON-Lua conversion
├── policies/
│   └── sample_policy.json    # Example Windows security policy
├── architecture/
│   └── README.md             # Current system architecture
├── docs/
│   ├── trade-offs.md         # Architectural decisions (current)
│   └── roadmap/              # Phase 2 planning docs
│       ├── IMPLEMENTATION_PLAN.md
│       ├── CODE_MODULES.md
│       ├── delivery-state-machine.md
│       ├── failure-scenarios.md
│       └── delivery-guarantees.md
├── reports/
│   └── latest_report.json    # Last evaluation result
├── sentinel_data.sqlite3     # Local database
└── scripts/
    ├── build.ps1 / build.bat
    ├── run.ps1 / run.bat
    └── smoketest.ps1 / smoketest.bat
```

---

## Why This Structure?

**Demonstrates Engineering Depth:**
- ✅ Working code (Phase 1: complete local evaluation engine)
- ✅ Foundation implementation (Phase 2 partial: retry queue, hashing, abstractions)
- ✅ Architectural planning (detailed roadmap for remaining work)
- ✅ Production thinking (state machines, failure modes, resource bounds)

**Honest Scope Communication:**
- Clear separation of "implemented" vs "planned"
- Explicit time estimates for remaining work
- Realistic complexity assessments
- No vaporware - working code first, then architectural plans

**Engineering Quality Signals:**
- Crash-safe persistence (SQLite WAL)
- Security-focused (sandboxing, timeouts, resource limits)
- Comprehensive testing (integration test suite + CI/CD)
- Defensive programming (NULL checks, input validation)
- Professional documentation (architecture docs, trade-off analysis)

---

## Documentation

| Document | Status | Description |
|----------|--------|-------------|
| **[README.md](README.md)** | ✅ Current | Project overview and quick start |
| **[architecture/README.md](architecture/README.md)** | ✅ Current | Detailed system architecture |
| **[docs/trade-offs.md](docs/trade-offs.md)** | ✅ Current | Architectural decisions and alternatives |
| **[docs/QUICK_TEST.md](docs/QUICK_TEST.md)** | ✅ Current | 5-minute pre-commit testing guide |
| **[docs/PRE_COMMIT_TESTING.md](docs/PRE_COMMIT_TESTING.md)** | ✅ Current | Comprehensive 15-minute validation |
| **[scripts/README.md](scripts/README.md)** | ✅ Current | Build/run/test script documentation |
| **[docs/roadmap/IMPLEMENTATION_PLAN.md](docs/roadmap/IMPLEMENTATION_PLAN.md)** | 📋 Planning | Phased delivery layer implementation |
| **[docs/roadmap/CODE_MODULES.md](docs/roadmap/CODE_MODULES.md)** | 📋 Planning | Module architecture and implementation plan |
| **[docs/roadmap/delivery-state-machine.md](docs/roadmap/delivery-state-machine.md)** | 📋 Planning | Delivery state transitions |
| **[docs/roadmap/failure-scenarios.md](docs/roadmap/failure-scenarios.md)** | 📋 Planning | Network/crash recovery patterns |
| **[docs/roadmap/delivery-guarantees.md](docs/roadmap/delivery-guarantees.md)** | 📋 Planning | Formal delivery specifications |

---

## Explicit Scope

### This Project IS:

- ✅ Working local evaluation agent (Phase 1)
- ✅ Thoughtful delivery layer planning (Phase 2 roadmap)
- ✅ Demonstration of production-inspired thinking
- ✅ Clean separation of concerns architecture
- ✅ Honest about implementation status

### This Project is NOT:

- ❌ Production distributed system
- ❌ Multi-agent coordination platform
- ❌ MQTT broker infrastructure
- ❌ Horizontally scalable backend
- ❌ Enterprise deployment tooling

**Sentinel is a focused, honest demonstration of agent architecture and delivery semantics planning.**

---

## License

MIT License - see [LICENSE](LICENSE) file.
