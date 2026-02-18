# Code Modules: Delivery Layer

> ⚠️ **Status: Planned Feature - Not Yet Implemented**
>
> This document describes the code modules that will comprise the delivery layer.
> None of these modules exist in the master branch yet.

## Module Dependency Graph

```
main.cpp
    ├─→ report_hasher.cpp (SHA-256 computation)
    ├─→ retry_queue.cpp (queue management + backoff)
    │   ├─→ db.cpp (schema + persistence)
    │   └─→ delivery_client.cpp (abstraction layer)
    │       ├─→ mqtt_delivery_client.cpp (paho-mqttpp3, primary)
    │       ├─→ http_delivery_client.cpp (cpp-httplib, fallback)
    │       └─→ mock_delivery_client.cpp (testing)
    └─→ [existing: osquery_runner, lua_evaluator, scoring]
```

---

## New Module: report_hasher

**Purpose:** SHA-256 content hashing for idempotent deduplication

```cpp
// src/report_hasher.h
#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Compute SHA-256 hash of canonical JSON
std::string compute_report_hash(const nlohmann::json& report);

// Canonicalize: sorted keys, no whitespace
std::string canonicalize_json(const nlohmann::json& j);
```

**Dependencies:** OpenSSL (SHA256) or standalone sha256.cpp

**LOC:** ~80 lines

---

## New Module: delivery_client

**Purpose:** Abstract delivery with protocol selection (MQTT primary, HTTP fallback)

```cpp
// src/delivery_client.h
#pragma once
#include <string>

struct DeliveryResult {
    bool success;
    int http_code;  // or MQTT reason code
    std::string error;
};

class DeliveryClient {
public:
    virtual ~DeliveryClient() = default;
    virtual DeliveryResult send(const std::string& report_json,
                                const std::string& hash) = 0;
};

// MQTT QoS 1 (primary delivery path)
class MqttDeliveryClient : public DeliveryClient {
public:
    MqttDeliveryClient(const std::string& broker_url, const std::string& topic);
    DeliveryResult send(const std::string& report_json,
                        const std::string& hash) override;
private:
    std::string broker_url_;
    std::string topic_;
    // paho-mqttpp3 client instance
};

// HTTP POST (fallback/testing)
class HttpDeliveryClient : public DeliveryClient {
public:
    HttpDeliveryClient(const std::string& backend_url);
    DeliveryResult send(const std::string& report_json,
                        const std::string& hash) override;
private:
    std::string backend_url_;
};

// Testing mock
class MockDeliveryClient : public DeliveryClient {
public:
    MockDeliveryClient(bool always_succeed);
    DeliveryResult send(const std::string& report_json,
                        const std::string& hash) override;
private:
    bool always_succeed_;
};
```

**Dependencies:** 
- paho-mqttpp3 (Eclipse Paho MQTT C++)
- cpp-httplib (header-only) or libcurl

**LOC:** ~250 lines total
- MqttDeliveryClient: ~120 lines
- HttpDeliveryClient: ~80 lines
- MockDeliveryClient: ~50 lines

---

## New Module: retry_queue

**Purpose:** Durable queue with exponential backoff + crash recovery

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
    
    // Enqueue after persistence
    void enqueue(int run_id, const std::string& report_json,
                 const std::string& hash);
    
    // Process all pending (immediate + retry-ready)
    void process_pending();
    
    // Load on startup (crash recovery)
    std::vector<QueuedReport> load_pending();
    
private:
    DB& db_;
    std::unique_ptr<DeliveryClient> client_;
    
    std::string compute_next_retry(int attempts);
    void update_state_delivered(int run_id);
    void update_state_retry(int run_id, int attempts, const std::string& error);
    void update_state_failed(int run_id);
};
```

**Dependencies:** db.cpp, delivery_client.cpp

**LOC:** ~200 lines

---

## Modified Module: db.cpp

**Changes:**
- Add `retry_queue` table to `init_schema()`
- Add `get_last_run_id()` method
- Add retry queue state update methods

```cpp
// src/db.h additions
class DB {
public:
    // ... existing methods ...
    
    // Get last inserted run_id
    int get_last_run_id();
    
    // Retry queue operations
    void enqueue_report(int run_id, const std::string& report_json,
                        const std::string& hash);
    void update_retry_state(int run_id, const std::string& state,
                            int attempts, const std::string& next_retry = "",
                            const std::string& error = "");
};
```

**LOC Added:** ~100 lines

---

## Modified Module: main.cpp

**Changes:**
- Integrate delivery layer after report persistence
- Add crash recovery on startup

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

**LOC Added:** ~30 lines

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

**LOC:** ~50 lines

---

## Summary

| Module | New/Modified | LOC | Purpose |
|--------|--------------|-----|---------|
| `report_hasher.cpp` | NEW | 80 | SHA-256 hashing |
| `delivery_client.cpp` | NEW | 150 | HTTP abstraction |
| `retry_queue.cpp` | NEW | 200 | Queue + backoff |
| `db.cpp` | MODIFIED | +100 | Retry queue schema |
| `main.cpp` | MODIFIED | +30 | Integration |
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
