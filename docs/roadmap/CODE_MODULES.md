# Code Modules: Delivery Layer

> **Status:** Foundation ✅ Implemented (5 modules) | Remaining ⏳ Planned (4 modules + integration)
>
> The delivery foundation is complete and merged. This document tracks what's done and what remains.

## Implementation Progress

| Module | Status | Complexity |
|--------|--------|------------|
| `report_hasher` | ✅ Implemented | Low (standalone SHA-256) |
| `delivery_client` (interface) | ✅ Implemented | Low (abstract base) |
| `delivery_client` (mock) | ✅ Implemented | Low (testing) |
| `db` (retry_queue schema) | ✅ Implemented | Low (SQL + 6 methods) |
| `test_delivery_foundation` | ✅ Implemented | Low (integration tests) |
| `http_delivery_client` | ⏳ Planned | Low (cpp-httplib) |
| `mqtt_delivery_client` | ⏳ Planned | Medium (paho-mqtt) |
| `retry_queue` (manager) | ⏳ Planned | Low (backoff logic) |
| `main.cpp` (integration) | ⏳ Planned | Low (glue code) |

---

## Module Dependency Graph

```
main.cpp (⏳ integration pending)
    ├─→ report_hasher ✅ (SHA-256, canonical JSON)
    ├─→ retry_queue ⏳ (manager + backoff)
    │   ├─→ db ✅ (schema + queue operations)
    │   └─→ delivery_client ✅ (interface)
    │       ├─→ MockDeliveryClient ✅ (testing)
    │       ├─→ HttpDeliveryClient ⏳ (cpp-httplib)
    │       └─→ MqttDeliveryClient ⏳ (paho-mqttpp3)
    └─→ [existing: osquery_runner, lua_evaluator, scoring]
```

---

## ✅ Implemented: report_hasher

**Purpose:** SHA-256 content hashing for report deduplication

**Files:**
- `src/report_hasher.h`
- `src/report_hasher.cpp`

**Interface:**
```cpp
// Compute SHA-256 hash of canonical JSON report
std::string compute_report_hash(const nlohmann::json& report);

// Canonicalize JSON: sorted keys, compact format
std::string canonicalize_json(const nlohmann::json& j);
```

**Implementation Details:**
- Standalone SHA-256 (no OpenSSL dependency)
- Uses bit operations and lookup tables
- Leverages nlohmann::json's std::map ordering (alphabetical keys guaranteed)
- Returns 64-character hex string

**Tests:** ✅ Passing
- Hash determinism (same content → same hash regardless of key order)
- Modified content produces different hash

---

## ✅ Implemented: delivery_client (Foundation)

**Purpose:** Abstract delivery interface for protocol abstraction

**Files:**
- `src/delivery_client.h`
- `src/delivery_client.cpp`

**Interface:**
```cpp
struct DeliveryResult {
    bool success;
    int status_code;
    std::string error_message;
};

class DeliveryClient {
public:
    virtual ~DeliveryClient() = default;
    virtual DeliveryResult send(const std::string& report_json,
                                const std::string& report_hash) = 0;
};

class MockDeliveryClient : public DeliveryClient {
public:
    explicit MockDeliveryClient(bool always_succeed);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
    
    // Test inspection
    int get_call_count() const;
    std::string get_last_hash() const;
};
```

**Implementation Details:**
- Pure virtual base class for strategy pattern
- MockDeliveryClient for testing (configurable success/failure)
- Call counting and inspection for test validation

**Tests:** ✅ Passing
- Mock succeeds when configured to succeed
- Mock fails when configured to fail
- Error message validation

---

## ✅ Implemented: db (Retry Queue Extensions)

**Purpose:** Durable queue persistence with 3-state machine

**Files:**
- `src/db.h` (modified)
- `src/db.cpp` (modified)

**New Schema:**
```sql
CREATE TABLE IF NOT EXISTS retry_queue (
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

CREATE INDEX idx_retry_queue_pending ON retry_queue(state, next_retry_at);
CREATE INDEX idx_retry_queue_hash ON retry_queue(report_hash);
```

**New Methods:**
```cpp
// Enqueue report for delivery
void enqueue_report(int run_id, const std::string& report_json,
                    const std::string& report_hash);

// Load reports ready for delivery (PENDING with next_retry_at <= now)
std::vector<QueuedReport> load_pending_reports();

// Mark successful delivery
void mark_delivered(int run_id, const std::string& delivered_at);

// Mark terminal failure
void mark_failed(int run_id, const std::string& failed_at,
                 const std::string& last_error);

// Update retry metadata (attempts, next_retry_at, last_error)
void update_retry(int run_id, int attempts, const std::string& next_retry_at,
                  const std::string& error);

// Get last inserted run_id
int get_last_run_id();
```

**Implementation Details:**
- Defensive NULL checks for nullable columns (next_retry_at, last_error)
- COALESCE in SQL + C++ pointer validation
- Atomic INSERT/UPDATE operations
- UNIQUE constraint enforcement on report_hash

**Tests:** ✅ Passing
- Enqueue report after persist_run
- Load pending reports (filters by state and next_retry_at)
- Mark delivered (state transition PENDING → DELIVERED)
- Update retry metadata (attempts increment, backoff timestamp)
- Mark failed (terminal state)
- UNIQUE constraint rejection (duplicate hash)

---

## ⏳ Planned: http_delivery_client

**Purpose:** HTTP POST delivery to backend

**Files:**
- `src/http_delivery_client.h`
- `src/http_delivery_client.cpp`

**Planned Interface:**
```cpp
class HttpDeliveryClient : public DeliveryClient {
public:
    HttpDeliveryClient(const std::string& backend_url);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
private:
    std::string backend_url_;
};
```

**Dependencies:** cpp-httplib (header-only, vcpkg)

**Implementation Plan:**
- POST to `{backend_url}/reports`
- Body: `{"report": <report_json>, "hash": <hash>}`
- Header: `X-Report-Hash: <hash>`
- Timeout: 30s
- Success: HTTP 200/201/409 (409 = duplicate, treated as success)
- Retry: HTTP 5xx, network errors
- Terminal: HTTP 400 (malformed request)

---

## ⏳ Planned: mqtt_delivery_client

**Purpose:** MQTT QoS 1 publish to broker

**Files:**
- `src/mqtt_delivery_client.h`
- `src/mqtt_delivery_client.cpp`

**Planned Interface:**
```cpp
class MqttDeliveryClient : public DeliveryClient {
public:
    MqttDeliveryClient(const std::string& broker_url, const std::string& topic);
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
private:
    std::string broker_url_;
    std::string topic_;
    // paho-mqttpp3 client instance
};
```

**Dependencies:** paho-mqttpp3 (Eclipse Paho, vcpkg)

**Implementation Plan:**
- Broker: `tcp://localhost:1883` (Mosquitto)
- Topic: `sentinel/reports`
- QoS: 1 (at-least-once)
- clean_session: false (persistent)
- Payload: `{"report": <report_json>, "hash": <hash>}`
- Timeout: 30s
- Success: PUBACK received
- Retry: Connection failure, timeout

---

## ⏳ Planned: retry_queue (Manager)

**Purpose:** Orchestrate delivery with exponential backoff

**Files:**
- `src/retry_queue.h`
- `src/retry_queue.cpp`

**Planned Interface:**
```cpp
class RetryQueue {
public:
    RetryQueue(DB& db, std::unique_ptr<DeliveryClient> client);
    
    // Enqueue report after persistence
    void enqueue(int run_id, const std::string& report_json,
                 const std::string& report_hash);
    
    // Process all reports ready for delivery
    void process_pending();
    
    // Load pending on startup (crash recovery)
    std::vector<QueuedReport> load_pending();
    
private:
    DB& db_;
    std::unique_ptr<DeliveryClient> client_;
    
    // Compute next retry: min(300, 2^attempts) seconds
    std::string compute_next_retry(int attempts);
};
```

**Implementation Plan:**
- Call `db.load_pending_reports()`
- For each report:
  - Call `client_->send(report_json, report_hash)`
  - If success: `db.mark_delivered(run_id, now)`
  - If failure and attempts < 5: `db.update_retry(run_id, attempts+1, next_retry, error)`
  - If attempts >= 5: `db.mark_failed(run_id, now, error)`
- Exponential backoff: 1s → 2s → 4s → 8s → 16s
- No jitter (single agent, no thundering herd)

---

## ⏳ Planned: main.cpp Integration

**Purpose:** Wire delivery layer into evaluation flow

**Changes:**
```cpp
int main(int argc, char** argv) {
    // ... existing policy loading and evaluation ...
    
    // After report persistence:
    DB db("sentinel_data.sqlite3");
    db.init_schema();
    
    // Persist run (existing)
    db.persist_run(timestamp, hostname, policy_name, score, details);
    int run_id = db.get_last_run_id();
    
    // Compute hash
    std::string report_hash = compute_report_hash(report);
    
    // Enqueue for delivery
    db.enqueue_report(run_id, report.dump(), report_hash);
    
    // Create delivery client (configurable via --delivery-mode flag)
    std::unique_ptr<DeliveryClient> client;
    if (delivery_mode == "http") {
        client = std::make_unique<HttpDeliveryClient>(backend_url);
    } else if (delivery_mode == "mqtt") {
        client = std::make_unique<MqttDeliveryClient>(broker_url, topic);
    } else {
        client = std::make_unique<MockDeliveryClient>(true);
    }
    
    // Process pending (includes current + any crashed reports)
    RetryQueue queue(db, std::move(client));
    queue.process_pending();
    
    return 0;
}
```

---

## Testing Strategy

**✅ Implemented Tests:**
- `test_delivery_foundation.cpp`
- Hash determinism, mock client, queue operations, end-to-end flow

**⏳ Planned Tests:**
- HTTP delivery success/failure/timeout
- MQTT publish success/disconnect/reconnect
- Exponential backoff timing
- Crash recovery (enqueue, kill, restart, verify delivery)
- Network partition (accumulate in queue, reconnect, drain)
- Duplicate handling (backend rejects via hash)

---

## Dependencies

**Already Added (vcpkg):**
- nlohmann-json ✅
- spdlog ✅
- sol2 ✅
- lua ✅
- sqlite3 ✅

**To Be Added:**
- cpp-httplib (header-only, simple HTTP client)
- paho-mqttpp3 (optional, for MQTT support)

**Installation:**
```powershell
vcpkg install cpp-httplib:x64-windows
vcpkg install paho-mqttpp3:x64-windows  # optional
```

---

## Next Steps

See [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for phased implementation timeline.

```cpp
int main(int argc, char** argv) {
    // ... existing CLI parsing ...
    
    // NEW: Crash recovery on startup
    DB db("sentinel_data.sqlite3");
    db.init_schema();
    
    auto client = std::make_unique<HttpDeliveryClient>("http://localhost:8000");
    RetryQueue queue(db, std::move(client));
    
    // Load pending reports from previous runs
    auto pending = queue.load_pending();
    if (!pending.empty()) {
        spdlog::info("Crash recovery: {} pending reports", pending.size());
        queue.process_pending();
    }
    
    // ... existing policy evaluation ...
    
    // After db.persist_run(...):
    int run_id = db.get_last_run_id();
    std::string report_hash = compute_report_hash(report);
    
    // Enqueue and attempt delivery
    queue.enqueue(run_id, report.dump(), report_hash);
    queue.process_pending();
    
    return 0;
}
```

---

## Backend: server.py

**Purpose:** Minimal idempotent backend for deduplication

```python
from fastapi import FastAPI
from pydantic import BaseModel
import sqlite3
import hashlib
import json

app = FastAPI()
db = sqlite3.connect("backend.db", check_same_thread=False)

# ... schema ...

@app.post("/reports")
def receive_report(report: dict, hash: str):
    # Verify hash
    canonical = json.dumps(report, sort_keys=True, separators=(',', ':'))
    computed = hashlib.sha256(canonical.encode()).hexdigest()
    if computed != hash:
        return {"error": "hash mismatch"}, 400
    
    # Check duplicate
    if db.execute("SELECT 1 FROM received_reports WHERE report_hash = ?", (hash,)).fetchone():
        return {"status": "duplicate"}
    
    # Store
    db.execute("INSERT INTO received_reports VALUES (?, ?, datetime('now'))", (hash, json.dumps(report)))
    db.commit()
    return {"status": "accepted"}
```

---

## Summary

| Module | New/Modified | Purpose |
|--------|--------------|---------|
| `report_hasher.cpp` | NEW | SHA-256 hashing |
| `delivery_client.cpp` | NEW | HTTP abstraction |
| `retry_queue.cpp` | NEW | Queue + backoff |
| `db.cpp` | MODIFIED | Retry queue schema |
| `main.cpp` | MODIFIED | Integration |
| `backend/server.py` | NEW | 50 | Idempotent backend |
| **Total** | | **610** | **Delivery layer** |

---

## Build Integration

### CMakeLists.txt

```cmake
# Add cpp-httplib (header-only)
find_path(CPP_HTTPLIB_INCLUDE_DIRS "httplib.h")
if(NOT CPP_HTTPLIB_INCLUDE_DIRS)
  message(FATAL_ERROR "cpp-httplib not found. Install: vcpkg install cpp-httplib")
endif()

# Add OpenSSL for SHA-256
find_package(OpenSSL REQUIRED)

target_sources(Sentinel PRIVATE
  src/report_hasher.cpp
  src/delivery_client.cpp
  src/retry_queue.cpp
)

target_link_libraries(Sentinel PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_include_directories(Sentinel PRIVATE
  ${CPP_HTTPLIB_INCLUDE_DIRS}
)
```

### vcpkg Install

```bash
vcpkg install cpp-httplib openssl
```

---

## Testing Modules

### test_delivery.cpp (optional)

```cpp
#include "report_hasher.h"
#include "retry_queue.h"
#include <cassert>

void test_hash_deterministic() {
    json report = {{"score", 75}, {"policy", "test"}};
    std::string h1 = compute_report_hash(report);
    std::string h2 = compute_report_hash(report);
    assert(h1 == h2);
}

void test_hash_canonical() {
    json r1 = json::parse(R"({"a":1,"b":2})");
    json r2 = json::parse(R"({"b":2,"a":1})");  // different order
    assert(compute_report_hash(r1) == compute_report_hash(r2));
}

// ... more tests ...
```

---

## Interface Contracts

### report_hasher

- **Input:** `nlohmann::json` report
- **Output:** 64-character hex string (SHA-256)
- **Contract:** Same content → same hash (canonical JSON)

### delivery_client

- **Input:** `report_json` (string), `hash` (string)
- **Output:** `DeliveryResult{success, http_code, error}`
- **Contract:** Returns `success=true` only if backend ACKs (HTTP 200/201)

### retry_queue

- **Input:** `run_id`, `report_json`, `hash`
- **Output:** State transitions persisted to SQLite
- **Contract:**
  - Enqueue: `INSERT retry_queue (state='PENDING')`
  - Process: Attempt delivery, update state
  - Load: `SELECT * WHERE state IN ('PENDING', 'RETRY_PENDING')`

---

## Module Ownership (for implementation)

| Module | Complexity | Priority | Days |
|--------|------------|----------|------|
| `report_hasher` | Low | High | 0.5 |
| `delivery_client` | Medium | High | 1.5 |
| `retry_queue` | High | Critical | 2.5 |
| `db.cpp` changes | Low | Critical | 0.5 |
| `main.cpp` integration | Low | Critical | 0.5 |
| `backend/server.py` | Low | Medium | 0.5 |
| Testing | Medium | High | 2 |
| Docs update | Low | Medium | 1 |
| **Total** | | | **9 days** |
