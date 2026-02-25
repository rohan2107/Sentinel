# Sentinel

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Sentinel is a security compliance evaluation agent demonstrating deterministic policy evaluation, durable persistence, and clean architectural patterns.**

This project showcases engineering thinking for agent-based systems: crash-safe persistence (SQLite WAL), sandboxed rule execution (Lua), at-least-once HTTP delivery with exponential backoff, and separation of concerns. Phase 1 handles local evaluation; Phase 2 adds durable delivery with SHA-256 content-addressable deduplication.

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

### Phase 2: Delivery Layer ✅ **COMPLETE**

**HTTP Delivery Implementation** ✅ **PRODUCTION-READY**
- ✅ Retry queue database schema (3-state: PENDING/DELIVERED/FAILED)
- ✅ SHA-256 content hashing (standalone implementation, no OpenSSL)
- ✅ DeliveryClient interface with abstract base class
- ✅ MockDeliveryClient for testing
- ✅ HttpDeliveryClient with cpp-httplib (header-only)
- ✅ RetryQueue manager with exponential backoff (1s → 300s)
- ✅ Queue operations: enqueue_report, load_pending_reports, mark_delivered, mark_failed, update_retry
- ✅ Crash recovery on startup (retries pending reports)
- ✅ main.cpp integration (`--enable-delivery` flag)
- ✅ FastAPI backend with hash deduplication
- ✅ Integration test suite (all tests passing)
- ✅ Defensive NULL checks for nullable fields
- ✅ Dynamic timestamp handling (prevents test decay)
- ✅ UNIQUE constraint enforcement on report_hash
- ✅ Idempotent delivery (409 = duplicate = success)

**Future Enhancement:**

| Feature | Complexity | Estimate | Status |
|---------|-----------|----------|--------|
| MQTT Delivery Client (QoS 1) | Medium | 6-8 hours | Planned |

**Delivery Usage:**
```bash
# Start backend
cd backend
pip install -r requirements.txt
uvicorn server:app --reload --port 8000

# Run agent with delivery enabled (HTTP-only; for TLS, terminate at a reverse proxy)
.\build\Release\Sentinel.exe --enable-delivery --backend-url http://localhost:8000
```

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
- ✅ **At-Least-Once Delivery**: HTTP delivery with exponential backoff (1s → 300s)
- ✅ **Crash Recovery**: Pending reports retried on agent restart
- ✅ **Backend Deduplication**: FastAPI backend deduplicates by SHA-256 hash

**Future Enhancement:**

- ⏳ **MQTT Delivery**: MQTT QoS 1 client for broker-based delivery (HTTP implemented)
- ⏳ **TLS/HTTPS**: Encrypted transport (currently terminate TLS at reverse proxy)
- ⏳ **Authentication**: API key/token in HTTP headers

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
    A --> RQ[RetryQueue<br/>Exp. Backoff]
    
    F --> G[runs table]
    F --> H[features table]
    F --> J[retry_queue table<br/>PENDING/DELIVERED/FAILED]
    
    RQ --> K[DeliveryClient<br/>Interface]
    K --> L[MockDeliveryClient]
    K --> M[HttpDeliveryClient<br/>cpp-httplib]
    K -.planned.-> N[MqttDeliveryClient]
    
    M --> BE[FastAPI Backend<br/>SHA-256 Dedup]
    
    O[report_hasher] --> P[SHA-256<br/>standalone]
    
    style F fill:#e1f5ff
    style C fill:#fff3e0
    style D fill:#fff3e0
    style J fill:#d4edda
    style K fill:#d4edda
    style O fill:#d4edda
    style RQ fill:#d4edda
    style M fill:#d4edda
    style BE fill:#d4edda
```

**Legend:**
- Solid boxes: Implemented
- Dashed lines: Planned (MQTT)
- Blue: Persistence layer
- Orange: External process execution
- Green: Delivery layer

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
│   ├── main.cpp                  # Orchestration, CLI, delivery integration
│   ├── osquery_runner.cpp        # osquery execution with timeout
│   ├── lua_evaluator.cpp         # Sandboxed Lua runtime
│   ├── scoring.cpp               # Weighted score computation
│   ├── db.cpp                    # SQLite persistence (WAL mode)
│   ├── json_to_lua.cpp           # JSON-Lua conversion
│   ├── report_hasher.cpp         # SHA-256 content hashing
│   ├── delivery_client.cpp       # DeliveryClient interface + MockDeliveryClient
│   ├── http_delivery_client.cpp  # HTTP POST delivery (cpp-httplib)
│   └── retry_queue.cpp           # Retry manager with exponential backoff
├── backend/
│   ├── server.py                 # FastAPI backend with hash deduplication
│   ├── requirements.txt          # Python dependencies
│   └── README.md                 # Backend API documentation
├── policies/
│   └── sample_policy.json        # Example Windows security policy
├── architecture/
│   └── README.md                 # System architecture documentation
├── docs/
│   ├── trade-offs.md             # Architectural decisions and alternatives
│   ├── DELIVERY_QUICKSTART.md    # Delivery layer usage guide
│   ├── IMPLEMENTATION_SUMMARY.md # Delivery implementation details
│   └── roadmap/                  # Design documents
│       ├── IMPLEMENTATION_PLAN.md
│       ├── CODE_MODULES.md
│       ├── delivery-state-machine.md
│       ├── failure-scenarios.md
│       └── delivery-guarantees.md
├── test_delivery_foundation.cpp  # Integration tests
├── reports/
│   └── latest_report.json        # Last evaluation result
├── sentinel_data.sqlite3         # Local database
└── scripts/
    ├── build.ps1 / build.bat
    ├── run.ps1 / run.bat
    ├── test.ps1 / test.bat
    └── smoketest.ps1 / smoketest.bat
```

---

## Why This Structure?

**Demonstrates Engineering Depth:**
- ✅ Working local evaluation engine (Phase 1)
- ✅ Complete HTTP delivery pipeline (Phase 2: retry queue, backoff, dedup, backend)
- ✅ Production thinking (state machines, failure modes, resource bounds)
- ✅ Extensible architecture (MQTT can be added via DeliveryClient interface)

**Honest Scope Communication:**
- Clear separation of "implemented" vs "planned"
- Working code first, architectural plans for future
- No vaporware - every checked item has code behind it

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
| **[docs/DELIVERY_QUICKSTART.md](docs/DELIVERY_QUICKSTART.md)** | ✅ Current | Delivery layer usage guide |
| **[docs/IMPLEMENTATION_SUMMARY.md](docs/IMPLEMENTATION_SUMMARY.md)** | ✅ Current | Delivery implementation details |
| **[docs/roadmap/IMPLEMENTATION_PLAN.md](docs/roadmap/IMPLEMENTATION_PLAN.md)** | ✅ Complete | Phased delivery layer implementation |
| **[docs/roadmap/CODE_MODULES.md](docs/roadmap/CODE_MODULES.md)** | ✅ Complete | Module architecture and dependencies |
| **[docs/roadmap/delivery-state-machine.md](docs/roadmap/delivery-state-machine.md)** | 📋 Reference | Delivery state transitions |
| **[docs/roadmap/failure-scenarios.md](docs/roadmap/failure-scenarios.md)** | 📋 Reference | Network/crash recovery patterns |
| **[docs/roadmap/delivery-guarantees.md](docs/roadmap/delivery-guarantees.md)** | 📋 Reference | Formal delivery specifications |

---

## Explicit Scope

### This Project IS:

- ✅ Working local evaluation agent with osquery + Lua (Phase 1)
- ✅ Complete HTTP delivery pipeline with retry queue, backoff, dedup (Phase 2)
- ✅ FastAPI backend with SHA-256 content-addressable deduplication
- ✅ Demonstration of production-quality patterns (state machines, crash recovery)
- ✅ Clean separation of concerns architecture

### This Project is NOT:

- ❌ Production distributed system
- ❌ Multi-agent coordination platform
- ❌ MQTT broker infrastructure
- ❌ Horizontally scalable backend
- ❌ Enterprise deployment tooling

**Sentinel is a focused demonstration of agent architecture, durable delivery semantics, and systems engineering.**

---

## License

MIT License - see [LICENSE](LICENSE) file.
