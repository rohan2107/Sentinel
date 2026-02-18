# Implementation Plan: Delivery Layer

> ⚠️ **Status: Planned Feature - Not Yet Implemented**
>
> This document describes the delivery layer implementation plan for Phase 2.
> None of these features exist in the master branch yet.

## Overview

Add at-least-once delivery semantics to Sentinel with:
- Durable retry queue (SQLite)
- SHA-256 content hashing
- Delivery abstraction (interface-based)
- MQTT QoS 1 delivery (primary, via paho-mqttpp3)
- HTTP delivery (fallback/testing, via cpp-httplib)
- Exponential backoff retry
- Crash recovery
- Minimal idempotent backend

**Time Estimate:** 10-12 days focused work

---

## Phase 1: Schema & State Machine (Days 1-2)

### retry_queue Table

```sql
CREATE TABLE IF NOT EXISTS retry_queue (
    run_id INTEGER PRIMARY KEY,
    report_hash TEXT UNIQUE NOT NULL,
    report_json TEXT NOT NULL,
    attempts INTEGER DEFAULT 0,
    state TEXT NOT NULL CHECK (state IN ('PENDING', 'RETRY_PENDING', 'DELIVERED', 'FAILED')),
    next_retry_at TEXT,
    created_at TEXT NOT NULL,
    delivered_at TEXT,
    failed_at TEXT,
    last_error TEXT,
    FOREIGN KEY (run_id) REFERENCES runs(id)
);

CREATE INDEX IF NOT EXISTS idx_retry_state ON retry_queue(state, next_retry_at);
CREATE INDEX IF NOT EXISTS idx_retry_hash ON retry_queue(report_hash);
```

### State Transitions

```
PENDING
  ├─→ DELIVERED (delivery success)
  └─→ RETRY_PENDING (delivery failed, attempts < 10)
      ├─→ DELIVERED (retry success)
      └─→ FAILED (attempts >= 10)
```

### State Persistence Boundaries

| Transition | Persisted | Action |
|------------|-----------|--------|
| **Eval Complete** → REPORT_PERSISTED | `INSERT runs`, `INSERT features` | SQLite COMMIT |
| **Report Persisted** → PENDING | `INSERT retry_queue (state='PENDING')` | Queue for delivery |
| **Delivery Attempt** → RETRY_PENDING | `UPDATE attempts++, next_retry_at, last_error` | Network failed |
| **Delivery Success** → DELIVERED | `UPDATE state='DELIVERED', delivered_at` | Backend ACKed |
| **Max Retries** → FAILED | `UPDATE state='FAILED', failed_at` | Terminal |

---

## Phase 2: SHA-256 Hashing (Day 3)

### Report Hash Computation

```cpp
// src/report_hasher.h
#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Compute SHA-256 hash of canonical JSON report
std::string compute_report_hash(const nlohmann::json& report);

// Canonicalize JSON (sorted keys, no whitespace)
std::string canonicalize_json(const nlohmann::json& j);
```

### Implementation

- Use library: OpenSSL `SHA256()` or standalone sha256.cpp
- Canonicalize JSON: sorted keys, compact format (no whitespace)
- Hash the canonical string
- Return hex-encoded hash (64 chars)

### Why

- Content-addressable deduplication
- Idempotent backend processing
- Tamper detection
- Aligns with CV claim: "content-addressable deduplication (SHA-256 hashing)"

---

## Phase 3: Delivery Client Abstraction (Days 4-6)

### DeliveryClient Interface

```cpp
// src/delivery_client.h
#pragma once
#include <string>
#include <memory>

struct DeliveryResult {
    bool success;
    int http_code;  // or MQTT reason code
    std::string error_message;
};

class DeliveryClient {
public:
    virtual ~DeliveryClient() = default;
    
    // Send report_json with hash to backend
    // Returns DeliveryResult with success/failure
    virtual DeliveryResult send(const std::string& report_json,
                                const std::string& report_hash) = 0;
};

// MQTT QoS 1 publish (primary delivery path)
class MqttDeliveryClient : public DeliveryClient {
public:
    MqttDeliveryClient(const std::string& broker_url, const std::string& topic);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
private:
    std::string broker_url_;
    std::string topic_;
    // MQTT client instance (paho-mqttpp3)
};

// HTTP POST to backend /reports endpoint (fallback/testing)
class HttpDeliveryClient : public DeliveryClient {
public:
    HttpDeliveryClient(const std::string& backend_url);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
private:
    std::string backend_url_;
};

// For testing: always succeeds or always fails
class MockDeliveryClient : public DeliveryClient {
public:
    explicit MockDeliveryClient(bool always_succeed);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
private:
    bool always_succeed_;
};
```

### MQTT Implementation (Primary)

- Library: paho-mqttpp3 (Eclipse Paho MQTT C++ client)
- Broker: localhost:1883 (Mosquitto for dev)
- Topic: `sentinel/reports`
- QoS: 1 (at-least-once)
- clean_session: false (persistent session)
- Payload: `{"report": <report_json>, "hash": <sha256>}`
- Timeout: 30s
- Success: PUBACK received
- Retry: Connection failure, timeout
- Reconnect: Automatic with exponential backoff

**Why MQTT Primary:**
- Broker persistence (clean_session=false)
- QoS 1 guarantees delivery acknowledgement
- Efficient binary protocol
- Aligns with CV claim (MQTT experience)

### HTTP Implementation (Fallback/Testing)

- Library: libcurl or cpp-httplib (header-only)
- POST to `{backend_url}/reports`
- Body: `{"report": <report_json>, "hash": <sha256>}`
- Header: `X-Report-Hash: <sha256>`
- Timeout: 30s
- Success: HTTP 200/201
- Retry: HTTP 5xx, network errors
- Terminal: HTTP 400 (malformed), 409 (duplicate treated as success)

**Why HTTP Fallback:**
- Trivial local testing (no broker required)
- Standard REST debugging (curl, Postman)
- Firewall-friendly (port 443)

---

## Phase 4: Retry Queue Manager (Days 7-8)

### RetryQueue Class

```cpp
// src/retry_queue.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "db.h"
#include "delivery_client.h"

struct QueuedReport {
    int run_id;
    std::string report_hash;
    std::string report_json;
    int attempts;
    std::string state;
    std::string next_retry_at;
};

class RetryQueue {
public:
    RetryQueue(DB& db, std::unique_ptr<DeliveryClient> client);
    
    // Enqueue new report after persistence
    void enqueue(int run_id, const std::string& report_json, const std::string& report_hash);
    
    // Attempt delivery of all pending reports
    void process_pending();
    
    // Load pending reports on startup (crash recovery)
    std::vector<QueuedReport> load_pending();
    
private:
    DB& db_;
    std::unique_ptr<DeliveryClient> client_;
    
    // Compute next retry time: min(300, 2^attempts) + jitter
    std::string compute_next_retry(int attempts);
};
```

### Exponential Backoff

```cpp
int backoff_s = std::min(300, 1 << attempts);  // 1s, 2s, 4s, 8s, ..., 300s (cap)
int jitter_s = rand() % (backoff_s / 4);       // ±25% jitter
std::string next_retry = iso8601_now_plus_seconds(backoff_s + jitter_s);
```

---

## Phase 5: Integration (Day 9)

### main.cpp Changes

```cpp
int main(int argc, char** argv) {
    // ... existing policy loading and evaluation ...
    
    // After report persistence:
    DB db("sentinel_data.sqlite3");
    db.init_schema();  // now includes retry_queue table
    
    // Persist run (existing code)
    db.persist_run(...);
    int run_id = db.get_last_run_id();  // new method
    
    // Compute hash
    std::string report_hash = compute_report_hash(report);
    
    // Create delivery client (configurable)
    std::string delivery_mode = get_config("delivery_mode", "mqtt");  // mqtt or http
    std::unique_ptr<DeliveryClient> client;
    
    if (delivery_mode == "mqtt") {
        client = std::make_unique<MqttDeliveryClient>("tcp://localhost:1883", "sentinel/reports");
    } else {
        client = std::make_unique<HttpDeliveryClient>("http://localhost:8000");
    }
    
    RetryQueue queue(db, std::move(client));
    
    // Enqueue for delivery
    queue.enqueue(run_id, report.dump(), report_hash);
    
    // Attempt immediate delivery
    queue.process_pending();
    
    // On crash recovery (startup):
    // auto pending = queue.load_pending();
    // queue.process_pending();
    
    return 0;
}
```

---

## Phase 6: Minimal Backend (Days 10-11)

### FastAPI Server

```python
# backend/server.py
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
import sqlite3
import hashlib
import json

app = FastAPI()

class Report(BaseModel):
    report: dict
    hash: str

db = sqlite3.connect("backend.db", check_same_thread=False)
db.execute("""
    CREATE TABLE IF NOT EXISTS received_reports (
        report_hash TEXT PRIMARY KEY,
        report_json TEXT NOT NULL,
        received_at TEXT NOT NULL
    )
""")
db.commit()

@app.post("/reports")
def receive_report(report: Report):
    # Verify hash
    canonical = json.dumps(report.report, sort_keys=True, separators=(',', ':'))
    computed_hash = hashlib.sha256(canonical.encode()).hexdigest()
    
    if computed_hash != report.hash:
        raise HTTPException(status_code=400, detail="Hash mismatch")
    
    # Check for duplicate
    cursor = db.execute("SELECT 1 FROM received_reports WHERE report_hash = ?", (report.hash,))
    if cursor.fetchone():
        return {"status": "duplicate", "hash": report.hash}
    
    # Store
    db.execute(
        "INSERT INTO received_reports (report_hash, report_json, received_at) VALUES (?, ?, datetime('now'))",
        (report.hash, json.dumps(report.report))
    )
    db.commit()
    
    return {"status": "accepted", "hash": report.hash}

@app.get("/reports/{report_hash}")
def get_report(report_hash: str):
    cursor = db.execute("SELECT report_json FROM received_reports WHERE report_hash = ?", (report_hash,))
    row = cursor.fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="Not found")
    return json.loads(row[0])
```

### Run Backend

```bash
pip install fastapi uvicorn
uvicorn server:app --reload --port 8000
```

---

## Testing Plan

### Unit Tests

1. **SHA-256 Hashing**
   - Same report → same hash
   - Different whitespace → same hash (canonical)
   - Different content → different hash

2. **Retry Backoff**
   - Attempts: 0→1s, 1→2s, 2→4s, 9→300s (cap)
   - Jitter: ±25%

3. **State Transitions**
   - PENDING → DELIVERED (success)
   - PENDING → RETRY_PENDING (failure)
   - RETRY_PENDING → FAILED (max attempts)

### Integration Tests

1. **End-to-End Delivery**
   - Start backend
   - Run agent
   - Verify report in backend DB
   - Check retry_queue state = DELIVERED

2. **Crash Recovery**
   - Enqueue report
   - Kill agent before delivery
   - Restart agent
   - Verify report delivered (loaded from retry_queue)

3. **Network Failure**
   - Stop backend
   - Run agent (report goes to retry_queue)
   - Start backend
   - Process pending → report delivered

4. **Idempotency**
   - Send same report twice
   - Backend returns "duplicate"
   - Verify only one entry in backend DB

---

## Success Criteria

- ✅ `retry_queue` table exists with correct schema
- ✅ Reports have SHA-256 hash computed and persisted
- ✅ HTTP delivery to backend works
- ✅ Exponential backoff implemented (1s → 300s)
- ✅ Crash recovery loads pending reports on startup
- ✅ Backend deduplicates by hash
- ✅ State machine transitions (PENDING → DELIVERED/FAILED)
- ✅ All tests pass
- ✅ Documentation updated with new architecture

---

## Files to Create/Modify

### New Files

- `src/report_hasher.h` + `src/report_hasher.cpp`
- `src/delivery_client.h` + `src/delivery_client.cpp`
- `src/retry_queue.h` + `src/retry_queue.cpp`
- `backend/server.py`
- `backend/requirements.txt`
- `tests/test_delivery.cpp` (optional)

### Modified Files

- `src/main.cpp` - integrate delivery layer
- `src/db.h` + `src/db.cpp` - add retry_queue schema, get_last_run_id()
- `CMakeLists.txt` - add libcurl or cpp-httplib dependency
- `README.md` - update "Implementation Status" section
- `docs/delivery-state-machine.md` - update with actual schema

---

## Dependencies

### C++ Libraries

- **libcurl** or **cpp-httplib**: HTTP client
  ```bash
  vcpkg install curl  # or cpp-httplib (header-only)
  ```

- **OpenSSL** (SHA-256):
  ```bash
  vcpkg install openssl
  ```

### Python Backend

```txt
fastapi==0.104.1
uvicorn==0.24.0
pydantic==2.5.0
```

---

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| libcurl complex | Use cpp-httplib (header-only, simpler) |
| OpenSSL overkill | Use standalone sha256.cpp (public domain) |
| Backend complexity | Keep minimal (< 100 LOC) |
| Time overrun | Cut backend to mock client first |

---

## Post-Implementation

### Documentation Updates

- Update README.md "Implementation Status" to mark features ✅
- Update architecture/README.md to remove MQTT, show HTTP
- Update docs/delivery-state-machine.md with actual schema
- Add docs/DELIVERY.md with usage guide

### Demo Script

```bash
# Terminal 1: Start backend
cd backend
uvicorn server:app --port 8000

# Terminal 2: Run agent
.\scripts\run.ps1

# Verify delivery
curl http://localhost:8000/reports/<hash>
```

---

## Interview Talking Points

After implementation, you can say:

> "I implemented at-least-once delivery semantics using a durable SQLite retry queue with exponential backoff. Reports are content-addressable via SHA-256 hashing, enabling idempotent backend processing. The agent persists delivery intent before network attempts, and crash recovery loads pending reports on restart. The backend deduplicates by hash, handling duplicates gracefully."

**And you can point to code.**
