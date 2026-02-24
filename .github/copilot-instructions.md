# GitHub Copilot Instructions for Sentinel

## Project Overview

**Sentinel** is a security compliance evaluation agent demonstrating:
- Deterministic policy evaluation (osquery + Lua sandbox)
- Durable at-least-once delivery semantics (SQLite retry queue)
- Content-addressable deduplication (SHA-256)
- Clean architectural patterns and separation of concerns

This is a portfolio/CV project showcasing engineering thinking for agent-based systems with crash-safe persistence and production-quality error handling.

---

## Development Philosophy

### Foundation-First Approach
- Build abstractions and interfaces before concrete implementations
- Create test infrastructure early (e.g., `MockDeliveryClient` before real clients)
- Validate core patterns with integration tests before adding complexity
- Prefer incremental complexity: simple → working → production-ready

### Pragmatic Engineering
- **Quick wins matter**: MVP implementations over perfect solutions
- **Choose simple**: Header-only libraries over complex dependencies (cpp-httplib > libcurl)
- **Avoid overengineering**: Standalone implementations when dependencies are overkill (SHA-256 vs OpenSSL)
- **Document trade-offs**: Maintain `docs/trade-offs.md` for architectural decisions

### Quality Standards
- Comprehensive error handling with descriptive messages
- Defensive NULL checks for all nullable database fields
- Resource bounds (timeouts, memory limits, output caps)
- Atomic operations for state transitions (SQLite single UPDATE statements)
- Clean separation of concerns (DB, evaluation, delivery, scoring)

---

## Code Style & Conventions

### C++ Standards
```cpp
// Modern C++17 patterns
std::unique_ptr<DeliveryClient> client = std::make_unique<HttpDeliveryClient>(url);

// RAII for resource management
DB db("sentinel_data.sqlite3");  // auto-cleanup in destructor

// Explicit error handling
if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw std::runtime_error("sqlite step failed: " + error_msg);
}

// Defensive programming
const char* ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
std::string value = ptr ? ptr : "";  // NULL safety
```

### Naming Conventions
- **Classes**: PascalCase (`DeliveryClient`, `RetryQueue`)
- **Methods**: snake_case (`enqueue_report`, `load_pending_reports`)
- **Variables**: snake_case (`run_id`, `report_hash`)
- **Constants**: UPPER_SNAKE_CASE or const variables
- **Files**: snake_case (`report_hasher.cpp`, `delivery_client.h`)

### Project Structure
```
src/               # Core implementation
  main.cpp         # Entry point, orchestration
  db.{h,cpp}       # Persistence layer
  *_client.{h,cpp} # Delivery implementations
  
docs/              # Documentation
  roadmap/         # Implementation plans with status tracking
  PRE_COMMIT_TESTING.md
  
test_*.cpp         # Integration tests at root level
policies/          # Sample policy JSON files
scripts/           # Build/run/test automation (PowerShell + batch)
```

---

## Documentation Standards

### Status Tracking
Use emoji indicators consistently:
- ✅ **Complete**: Fully implemented, tested, merged
- ⏳ **Planned**: Designed but not yet implemented
- 🔄 **Partial**: Some components done, others pending
- ❌ **Not Started**: No code exists yet

### Implementation Plans
Maintain detailed roadmaps in `docs/roadmap/` with:
- **Phase-based organization** (Phase 1-3 foundation, Phase 4-7 integration)
- **Time estimates** (hours or days, be realistic)
- **Success criteria** (clear, testable outcomes)
- **Code snippets** (show expected API usage)
- **Testing plans** (unit + integration)
- **Risks & mitigations**

Example structure:
```markdown
## ⏳ Phase 4: HTTP Client - PLANNED

**Time Estimate:** 4-5 hours

### Implementation
[code snippet of header]

### Testing
1. Success case: HTTP 200
2. Retry case: HTTP 5xx
3. Terminal failure: HTTP 400

**Files:** `src/http_delivery_client.{h,cpp}`
```

### Code Comments
- **Why, not what**: Explain reasoning, not obvious behavior
- **Edge cases**: Document defensive checks and NULL handling
- **Resource bounds**: Note timeouts, limits, caps
- **State transitions**: Clarify state machine logic

```cpp
// Use 3-state machine: removed RETRY_PENDING since single agent
// doesn't need separate "ready to retry" state
state TEXT NOT NULL CHECK (state IN ('PENDING', 'DELIVERED', 'FAILED'))

// Cap backoff at 300s (5 min) to avoid indefinite delays
int backoff_s = std::min(300, 1 << attempts);
```

---

## Testing Strategy

### Test Pyramid
1. **Integration tests** (primary): Full workflows with real DB, mock network
2. **Unit tests** (targeted): Hash determinism, backoff calculation
3. **Manual smoke tests**: Scripts in `scripts/smoketest.ps1`

### Test Requirements
- ✅ Use temporary test databases (clean up: `remove(test_db)`)
- ✅ Dynamic timestamps to prevent test decay (`get_future_timestamp()`)
- ✅ Assert on both positive and negative cases
- ✅ Test error paths (UNIQUE constraint violations, NULL handling)
- ✅ Clear output with `[PASS]` indicators

```cpp
void test_something() {
    std::cout << "=== Testing Feature X ===\n";
    
    // Setup
    const char* test_db = "test_feature.db";
    remove(test_db);
    
    // Test
    assert(actual == expected);
    std::cout << "[PASS] Feature works\n\n";
    
    // Cleanup
    remove(test_db);
}
```

---

## Architecture Patterns

### Interface-Based Design
```cpp
// Abstract base class for dependency injection
class DeliveryClient {
public:
    virtual ~DeliveryClient() = default;
    virtual DeliveryResult send(const std::string& json, 
                               const std::string& hash) = 0;
};

// Concrete implementations
class HttpDeliveryClient : public DeliveryClient { ... };
class MqttDeliveryClient : public DeliveryClient { ... };
class MockDeliveryClient : public DeliveryClient { ... };  // testing

// Usage with unique_ptr for ownership
std::unique_ptr<DeliveryClient> client = std::make_unique<HttpDeliveryClient>(url);
```

### State Machine Discipline
- **Single source of truth**: Database state is authoritative
- **Atomic transitions**: One UPDATE statement per state change
- **Terminal states**: Once DELIVERED or FAILED, immutable
- **Crash recovery**: Load PENDING reports on startup via `load_pending_reports()`

### Persistence First
```cpp
// ALWAYS persist intent before side effects
db.enqueue_report(run_id, report.dump(), hash);  // Durable
client.send(report, hash);                        // Network (can fail)
db.mark_delivered(run_id, iso8601_now());        // Confirm
```

---

## Build & Dependency Management

### vcpkg Dependencies
```bash
vcpkg install nlohmann-json spdlog sol2 lua sqlite3

# Future additions:
vcpkg install cpp-httplib      # HTTP client (header-only)
vcpkg install paho-mqttpp3     # MQTT client
```

### CMake Patterns
- Find packages via `CONFIG` mode for vcpkg
- Manual Lua detection with fallback paths
- Create test executables alongside main binary
- Link only necessary libraries to each target

### Cross-Platform Considerations
```cpp
#ifdef _WIN32
    gmtime_s(&tm, &t);           // Windows-safe
    GetComputerNameA(buffer, &size);
#else
    gmtime_r(&t, &tm);           // POSIX
    gethostname(buffer, size);
#endif
```

---

## Database Conventions

### Schema Evolution
- ✅ Use `CREATE TABLE IF NOT EXISTS`
- ✅ Add indexes for query patterns (`WHERE state = 'PENDING'`)
- ✅ CHECK constraints for enums (`state IN (...)`)
- ✅ UNIQUE constraints for deduplication (`report_hash`)
- ✅ Composite indexes for multi-column queries (`state, next_retry_at`)

### Query Safety
```cpp
// Use COALESCE for nullable columns to avoid NULL pointer checks
COALESCE(next_retry_at, '')     // SQL level
const char* ptr = sqlite3_column_text(stmt, idx);
std::string val = ptr ? ptr : "";  // C++ level (defense in depth)
```

### Transactions
```cpp
sqlite3_exec(p->db, "BEGIN IMMEDIATE;", ...);  // Lock early
// ... multiple inserts/updates ...
sqlite3_exec(p->db, "COMMIT;", ...);           // Atomic
```

---

## Delivery Layer Principles

### At-Least-Once Delivery
1. **Persist before send**: Enqueue to retry_queue first
2. **Immediate attempt**: Try delivery right away (`process_pending()`)
3. **Exponential backoff**: 1s → 2s → 4s → ... → 300s (cap) with jitter
4. **Crash recovery**: On startup, load PENDING reports and retry
5. **Terminal failure**: After max retries, mark FAILED but keep record

### Idempotent Backend
- Backend deduplicates by `report_hash`
- HTTP 409 = "duplicate" = treat as success (already delivered)
- Backend verifies hash matches report content
- Store (hash, report, timestamp) for audit trail

### Content-Addressable Storage
```cpp
// Canonical JSON: sorted keys, no whitespace
std::string canonical = canonicalize_json(report);
std::string hash = compute_report_hash(report);  // SHA-256

// Hash is the identity
db.enqueue_report(run_id, report.dump(), hash);
// UNIQUE constraint on report_hash prevents duplicate enqueuing
```

---

## Error Handling Philosophy

### Fail Gracefully
```cpp
try {
    db.persist_run(...);
    spdlog::info("Persisted run to DB");
} catch (const std::exception& e) {
    spdlog::error("Failed to persist: {}", e.what());
    // Don't crash - best-effort persistence
}
```

### Specific Error Messages
```cpp
// BAD
throw std::runtime_error("database error");

// GOOD
throw std::runtime_error("sqlite prepare enqueue_report: " + std::string(sqlite3_errmsg(db)));
```

### Resource Cleanup
```cpp
if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);  // Clean up BEFORE throwing
    throw std::runtime_error("step failed");
}
sqlite3_finalize(stmt);
```

---

## Git Workflow

### Branch Strategy
- `master`: Stable, tested code only
- Feature branches: `feature/http-delivery-client`
- Merge only when:
  - ✅ Tests pass
  - ✅ Documentation updated
  - ✅ `IMPLEMENTATION_PLAN.md` status reflects completion

### Commit Messages
```
Add HTTP delivery client implementation

- Implement HttpDeliveryClient using cpp-httplib
- POST to /reports endpoint with JSON payload
- Handle 200/201 (success), 409 (duplicate), 5xx (retry)
- Add timeout configuration (30s default)
- Update CMakeLists.txt with cpp-httplib dependency
- Mark Phase 4 as complete in IMPLEMENTATION_PLAN.md

Fixes #issue-number
```

---

## Interview Talking Points

When code is complete, emphasize:

> "I implemented at-least-once delivery semantics using a durable SQLite retry queue with exponential backoff. Reports are content-addressable via SHA-256 hashing, enabling idempotent backend processing. The agent persists delivery intent before network attempts, ensuring crash recovery can reload pending reports on restart. The abstraction layer supports both HTTP and MQTT transports via interface-based design."

**Key Technical Points:**
- State machine with atomic transitions (3-state: PENDING/DELIVERED/FAILED)
- Crash-safe persistence (SQLite WAL mode, transactions)
- Exponential backoff with jitter (1s → 300s cap)
- Content-addressable deduplication (SHA-256)
- Interface-based design for testability (MockDeliveryClient)
- Resource bounds (timeouts, memory limits, sandbox isolation)

---

## Common Tasks

### Adding a New Delivery Client
1. Create `src/xxx_delivery_client.{h,cpp}`
2. Inherit from `DeliveryClient` base class
3. Implement `send()` returning `DeliveryResult`
4. Add to CMakeLists.txt
5. Create unit/integration test in `test_delivery_foundation.cpp`
6. Update `docs/roadmap/IMPLEMENTATION_PLAN.md` status
7. Update README.md capabilities table

### Adding a New Policy Rule
1. Add rule object to `policies/*.json`
2. Specify osquery query for data collection
3. Write Lua evaluation logic
4. Set weight (0-100) for scoring
5. Test with `.\scripts\run.ps1 -policy policies/your_policy.json`
6. Verify DB persistence and report generation

### Updating Schema
1. Modify `init_schema()` in `src/db.cpp`
2. Add migration logic if needed (ALTER TABLE)
3. Update struct definitions in `src/db.h`
4. Update query methods (SELECT/INSERT/UPDATE)
5. Add tests in `test_delivery_foundation.cpp`
6. Document in `docs/roadmap/` relevant file

---

## Performance Considerations

### SQLite Optimization
- ✅ WAL mode enabled: `PRAGMA journal_mode=WAL;`
- ✅ Indexes on query columns: `idx_retry_state`, `idx_retry_hash`
- ✅ Prepared statements reused (within function scope)
- ✅ Batch operations in transactions

### Resource Limits
- osquery timeout: 10s
- Lua evaluation timeout: 1s  
- osquery output cap: 1MB
- Retry backoff cap: 300s (5 minutes)
- Connection timeout: 30s (HTTP/MQTT)

### Scalability Notes
- Single-agent design (no distributed coordination needed)
- SQLite sufficient for local agent workload (1000s of runs)
- For multi-agent: consider PostgreSQL + distributed queue

---

## Security Considerations

### Sandboxing
- Lua runtime isolated (no I/O access)
- osquery runs with current user permissions (no elevation)
- Policy files validated before execution (JSON schema)

### Data Integrity
- SHA-256 hashing for tamper detection
- Backend verifies hash matches report content
- SQLite integrity checks (foreign keys, CHECK constraints)

### Secrets Management
- No credentials in code or config files
- Environment variables for backend URLs/auth tokens
- Clear separation: agent → broker/backend (unidirectional)

---

## When in Doubt

1. **Check the implementation plan**: `docs/roadmap/IMPLEMENTATION_PLAN.md`
2. **Follow existing patterns**: Look at `MockDeliveryClient` before writing `HttpDeliveryClient`
3. **Test early**: Write integration test before full implementation
4. **Document trade-offs**: Add to `docs/trade-offs.md` for big decisions
5. **Ask for pragmatic path**: Quick win > perfect solution

---

## Success Criteria

Code is ready to merge when:
- ✅ All tests pass (`.\scripts\test.ps1`)
- ✅ Smoke test passes (`.\scripts\smoketest.ps1`)
- ✅ No compiler warnings (MSVC `/W4` or equivalent)
- ✅ Documentation updated (README.md, IMPLEMENTATION_PLAN.md)
- ✅ Error messages are descriptive
- ✅ Resource cleanup verified (no leaks, DB files closed)
- ✅ Can explain implementation in interview context

---

**Remember:** This project demonstrates engineering judgment, not just coding skill. Clean architecture, pragmatic trade-offs, and production-quality error handling matter more than feature count.
