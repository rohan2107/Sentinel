# Sentinel Architecture

Local security compliance evaluation agent with deterministic policy execution, durable persistence, and delivery foundation.

## System Architecture

```mermaid
graph TB
    subgraph "Agent (Single Process)"
        M[main.cpp<br/>Orchestrator]
        PV[Policy Validator]
        OR[osquery Runner<br/>10s timeout]
        LE[Lua Evaluator<br/>1s timeout, sandboxed]
        SC[Scoring Engine<br/>Weighted sum]
        DB[(SQLite<br/>WAL Mode)]
        RW[Report Writer<br/>JSON]
        
        RH[Report Hasher<br/>SHA-256]
        DC[DeliveryClient<br/>Interface]
        MOCK[MockDeliveryClient<br/>Testing]
        
        M --> PV
        PV --> M
        M --> OR
        OR --> LE
        LE --> SC
        SC --> DB
        DB --> RW
        
        RW --> RH
        RH --> DB
        DB --> DC
        DC --> MOCK
    end
    
    subgraph "Filesystem"
        POL[policies/<br/>sample_policy.json]
        REP[reports/<br/>latest_report.json]
        DBFILE[(sentinel_data.sqlite3<br/>runs + features + retry_queue)]
    end
    
    M -.reads.-> POL
    RW -.writes.-> REP
    DB -.persists.-> DBFILE
    
    classDef storage fill:#e1f5ff,stroke:#01579b
    classDef compute fill:#fff3e0,stroke:#e65100
    classDef delivery fill:#e8f5e9,stroke:#2e7d32
    
    class DB,DBFILE storage
    class M,PV,OR,LE,SC,RW compute
    class RH,DC,MOCK delivery
```

## Implementation Status

**Phase 1 (Local Evaluation):** ✅ Complete
**Phase 2 (Delivery Foundation):** ✅ Partial (foundation merged, integration pending)

See [`docs/roadmap/IMPLEMENTATION_PLAN.md`](../docs/roadmap/IMPLEMENTATION_PLAN.md) for detailed breakdown.

---

## Components

### **main.cpp** - Agent Orchestrator
- **Purpose**: Controls evaluation lifecycle, error handling, resource cleanup
- **Responsibilities**:
  - Command-line argument parsing
  - Policy file loading
  - Orchestrate evaluation pipeline
  - Exception handling and logging
  - Exit code determination

### **Policy Validator**
- **Purpose**: Parse and validate JSON policy files
- **Validation**:
  - JSON schema correctness
  - Required fields present
  - Rule structure well-formed
  - Lua syntax check (basic)
- **Failures**: Invalid JSON → graceful error + exit code 1

### **osquery Runner** (osquery_runner.cpp)
- **Purpose**: Execute osquery data collection with timeout enforcement
- **Implementation**:
  - Spawns `osqueryi.exe` subprocess
  - 10-second timeout (SIGTERM then SIGKILL)
  - Captures stdout/stderr
  - Parses JSON output
- **Failures**: Timeout → empty results, osquery crash → logged + continue

### **Lua Evaluator** (lua_evaluator.cpp)
- **Purpose**: Execute rule logic in sandboxed Lua runtime
- **Sandboxing**:
  - No file I/O (`io` library disabled)
  - No network (`socket` disabled)
  - No process spawn (`os.execute` disabled)
  - 1-second timeout via instruction count limit
- **Input**: osquery results (JSON → Lua table)
- **Output**: Boolean (rule pass/fail)
- **Failures**: Timeout → rule fails, Lua error → logged + rule fails

### **Scoring Engine** (scoring.cpp)
- **Purpose**: Compute weighted score from rule results
- **Algorithm**:
  ```
  score = SUM(rule.weight IF rule passed)
  max_possible = SUM(all rule weights)
  percentage = (score / max_possible) * 100
  ```
- **Output**: Integer score (0-100)

### **SQLite Database** (db.cpp)
- **Purpose**: Durable persistence with crash-safe guarantees
- **Configuration**:
  - WAL mode (`PRAGMA journal_mode=WAL`)
  - Atomic transactions (`BEGIN IMMEDIATE; ... COMMIT;`)
  - Foreign key constraints enforced
- **Schema**:
  ```sql
  -- Phase 1: Evaluation results
  CREATE TABLE runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,            -- ISO-8601 timestamp
    hostname TEXT,
    policy TEXT,
    score INTEGER,
    details_json TEXT            -- Full evaluation details
  );
  
  CREATE TABLE features (
    run_id INTEGER PRIMARY KEY,
    firewall_enabled INTEGER,
    av_installed INTEGER,
    FOREIGN KEY(run_id) REFERENCES runs(id)
  );
  
  -- Phase 2: Delivery queue (foundation)
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
    FOREIGN KEY (run_id) REFERENCES runs(id)
  );
  ```
- **New Methods (Phase 2 Foundation)**:
  - `enqueue_report()` - Add report to delivery queue
  - `load_pending_reports()` - Crash recovery
  - `mark_delivered()`, `mark_failed()`, `update_retry()` - State transitions
- **Guarantees**: WAL ensures durability, atomic transactions prevent partial writes

### **Report Writer**
- **Purpose**: Generate JSON report files
- **Output Format**:
  ```json
  {
    "policy": "policy-name",
    "score": 75,
    "details": { "rule_id": true/false },
    "timestamp": "2026-02-18T10:30:00.123Z",
    "hostname": "MACHINE-NAME"
  }
  ```
- **Location**: `reports/latest_report.json`
- **Failures**: Disk full → logged, but doesn't prevent database persistence

### **Report Hasher** (report_hasher.cpp) - Phase 2 Foundation ✅
- **Purpose**: SHA-256 content hashing for idempotent delivery
- **Implementation**:
  - Standalone SHA-256 (no OpenSSL dependency)
  - Canonicalize JSON (sorted keys via nlohmann::json)
  - Returns 64-character hex string
- **Use Case**: Deduplication at backend (same report = same hash)

### **DeliveryClient** (delivery_client.cpp) - Phase 2 Foundation ✅
- **Purpose**: Abstract delivery interface for protocol flexibility
- **Implemented**:
  - `DeliveryClient` abstract base class (pure virtual)
  - `MockDeliveryClient` for testing (configurable success/failure)
- **Planned**:
  - `HttpDeliveryClient` for HTTP POST delivery
  - `MqttDeliveryClient` for MQTT QoS 1 publish

---

## Data Flow (Current)

```
1. Parse CLI args → policy file path
2. Load policy JSON → validate schema
3. For each rule:
   3a. Execute osquery (timeout 10s)
   3b. Parse JSON results
   3c. Evaluate Lua code (timeout 1s, sandboxed)
   3d. Record pass/fail
4. Compute weighted score
5. BEGIN TRANSACTION
6. INSERT INTO runs (...)
7. INSERT INTO features (...)
8. COMMIT
9. Write JSON report file
10. Exit (code 0 if success)
```

---

## Persistence Guarantees

### What SQLite WAL Provides

**Atomicity**: Transaction either fully commits or fully rolls back (no partial writes)

**Durability**: Once `COMMIT` returns, data survives power loss (WAL fsync)

**Isolation**: Readers see consistent snapshot (MVCC via WAL)

**Crash Recovery**: WAL checkpoint on next open replays committed transactions

### Phase 2 Foundation Guarantees (Implemented)

**No Report Loss After Persistence**: Once in runs table, report survives crashes (SQLite WAL)

**Idempotent Enqueue**: UNIQUE constraint on report_hash prevents duplicate queue entries

**State Consistency**: CHECK constraint enforces only valid states (PENDING, DELIVERED, FAILED)

**Atomic State Transitions**: Each state change is a single UPDATE (atomic)

**Crash-Safe Queue**: load_pending_reports() restores PENDING reports on restart

### What Is NOT Yet Guaranteed (Phase 2 Remaining)

- **No Active Delivery**: Reports enqueued but not automatically delivered (RetryQueue manager not integrated)
- **No Exponential Backoff**: Retry metadata schema exists but manager class not implemented
- **No Backend Integration**: No HTTP/MQTT clients wired to main.cpp yet

---

## Resource Bounds (Enforced)

| Resource | Limit | Enforcement | Failure Mode |
|----------|-------|-------------|--------------|
| **osquery CPU** | 10s timeout | Process kill (SIGTERM → SIGKILL) | Empty results, evaluation continues |
| **Lua CPU** | 1s timeout | Instruction count hook | Rule fails, evaluation continues |
| **Lua Memory** | Process heap limit | Lua allocator | Out-of-memory → Lua error |
| **osquery Output** | 1MB | Stdout buffer truncation | Parsed as much as fits |
| **SQLite Disk** | Filesystem limit | Write failure on disk full | Transaction aborted, eval fails |
| **Policy File Size** | 10MB | fread size check | File too large → validation error |

---

## Error Handling (Current)

### Recoverable Errors (Continue Evaluation)

- osquery timeout → empty results, Lua sees empty table
- Lua timeout → rule marked as failed
- Lua runtime error (e.g., nil access) → rule fails, logged
- Single rule failure → other rules still evaluated

### Terminal Errors (Exit Immediately)

- Policy file not found → exit code 1
- Policy JSON malformed → exit code 1
- SQLite database locked (concurrent access) → exit code 1
- Disk full during persistence → exit code 1

---

## Performance (Observed During Local Testing)

**Hardware**: Windows 10, i7-8750H (6 cores), 16GB RAM, SSD

**Execution**:
- Policy validation: <1ms
- osquery (3 queries): 150ms average
- Lua evaluation (5 rules): 2ms total
- SQLite transaction: 3ms (WAL mode)
- **Total**: ~160ms per evaluation

**Resource Usage**:
- CPU: 3% average (spikes to 25% during osquery)
- Memory: 45MB RSS (stable)
- Disk I/O: ~2 writes/sec (WAL checkpoints)

**Capacity**:
- Evaluations/sec: ~6 (limited by osquery, not Sentinel)
- Typical use case: 1 eval/5min = 0.003/sec (far below capacity)
- SQLite: Handles 10k writes/sec (single agent needs ~0.003/sec)

---

## Design Principles

1. **Fail-Safe Defaults**: Errors default to "fail" (rule fails, score reduced) not "crash"
2. **Resource Bounds**: Every external operation has timeout to prevent hangs
3. **Separation of Concerns**: Policy, data, logic, persistence all separate modules
4. **Crash-Safe Persistence**: WAL mode ensures durability, no partial writes
5. **Offline-First**: No network dependency, works in air-gapped environments
6. **Observable**: All errors logged (spdlog), exit codes meaningful

---

## Current Limitations

### By Design (Intentional Constraints)

- **No Delivery**: Phase 1 only does local evaluation
- **No Retry Logic**: Failed evaluations are not retried
- **No Distributed Coordination**: Single-agent only
- **Windows-Only**: Windows Security Center queries, `osqueryi.exe` paths

### Technical Debt (Could Improve)

- **Hardcoded Paths**: osquery binary path not configurable
- **Limited Testing**: No automated test suite (manual testing only)
- **Basic Logging**: spdlog used but minimal structured logging
- **No Metrics**: No Prometheus/StatsD instrumentation

---

## Delivery Foundation (Phase 2 - Partial)

**✅ Implemented (Merged to master):**
- Durable retry queue (SQLite 3-state machine)
- SHA-256 content hashing (standalone)
- DeliveryClient interface + MockDeliveryClient
- Crash recovery foundation (load_pending_reports)
- Integration test suite

**⏳ Remaining (3-4 days):**
- HTTP delivery client
- MQTT delivery client
- RetryQueue manager with exponential backoff
- Main.cpp integration
- Minimal backend for testing

See [`docs/roadmap/`](../docs/roadmap/) for detailed implementation status.

---

## Source Files

**Phase 1 (Evaluation):**

| File | Purpose |
|------|---------||
| `src/main.cpp` | Orchestration, error handling |
| `src/osquery_runner.cpp` | osquery execution with timeout |
| `src/lua_evaluator.cpp` | Sandboxed Lua runtime |
| `src/scoring.cpp` | Weighted score computation |
| `src/db.cpp` | SQLite persistence (WAL) |
| `src/json_to_lua.cpp` | JSON → Lua table conversion |

**Phase 2 (Delivery Foundation):**

| File | Purpose |
|------|---------||
| `src/db.cpp` (additions) | retry_queue schema + methods |
| `src/report_hasher.cpp` | SHA-256 content hashing |
| `src/delivery_client.cpp` | Interface + MockDeliveryClient |
| `tests/test_delivery_foundation.cpp` | Integration tests |

---

## Dependencies

- **nlohmann/json**: JSON parsing (header-only)
- **sol2**: Lua C++ bindings (header-only)
- **spdlog**: Logging
- **sqlite3**: Embedded database
- **lua**: Lua 5.4 runtime

All installed via vcpkg.

---

This architecture demonstrates clean separation of concerns, crash-safe persistence, and resource-bounded execution without network layer complexity.
