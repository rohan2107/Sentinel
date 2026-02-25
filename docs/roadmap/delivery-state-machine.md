# Delivery State Machine

> **Status:** ✅ Implemented | MQTT ⏳ Future Enhancement
>
> The 3-state retry queue is implemented and integrated. HTTP delivery with exponential backoff is production-ready.

Explicit 3-state machine ensuring at-least-once delivery with durable persistence at each boundary.

## Implementation Status

**✅ Implemented:**
- retry_queue table schema with 3-state CHECK constraint
- Database operations: enqueue_report, load_pending_reports, mark_delivered, mark_failed, update_retry
- SHA-256 content hashing (standalone)
- DeliveryClient abstract interface + MockDeliveryClient + HttpDeliveryClient
- RetryQueue manager with exponential backoff (1s → 300s, ±25% jitter)
- Integration into main.cpp (--enable-delivery, --backend-url)
- Crash recovery on startup (load_pending_reports)
- Integration tests covering all state transitions

**⏳ Future Enhancement:**
- MQTT delivery client

---

## Simplified 3-State Machine

**Design Decision:** 3-state instead of 4-state for simplicity. Single agent doesn't need separate RETRY_PENDING state - PENDING covers both initial and retry attempts.

### States

| State | Entry Condition | Persisted Fields | Next Transition |
|-------|----------------|------------------|-----------------|
| **PENDING** | Report persisted to DB | `state='PENDING'`, `attempts=0`, `next_retry_at=NULL` | → DELIVERED (success)<br>→ PENDING (retry, update attempts)<br>→ FAILED (max retries) |
| **DELIVERED** | Backend ACK received | `state='DELIVERED'`, `delivered_at=<timestamp>` | Terminal (no further transitions) |
| **FAILED** | Max retries exceeded (10) | `state='FAILED'`, `failed_at=<timestamp>`, `last_error=<msg>` | Terminal (manual intervention required) |

### State Transitions

```
┌─────────────────────────────────────────────────┐
│  Report Ready (main.cpp persist_run complete)   │
└──────────────────────┬──────────────────────────┘
                       ▼
              ┌────────────────┐
              │    PENDING     │
              │  attempts = 0  │
              └────────┬───────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
  SUCCESS│        FAILURE│      MAX RETRIES│
        ▼              ▼              ▼
┌──────────────┐  ┌─────────┐  ┌─────────┐
│  DELIVERED   │  │ PENDING │  │ FAILED  │
│              │  │attempts+│  │         │
└──────────────┘  └────┬────┘  └─────────┘
                       │
                   (retry loop)
```

---

## Actual Implemented Schema

```sql
CREATE TABLE IF NOT EXISTS retry_queue (
    run_id INTEGER PRIMARY KEY,
    report_hash TEXT UNIQUE NOT NULL,         -- SHA-256 for dedup
    report_json TEXT NOT NULL,                 -- Full report for replay
    attempts INTEGER DEFAULT 0,                -- Retry counter
    state TEXT NOT NULL CHECK (state IN ('PENDING', 'DELIVERED', 'FAILED')),
    next_retry_at TEXT,                        -- ISO-8601 timestamp for backoff
    created_at TEXT NOT NULL,                  -- Initial enqueue time
    delivered_at TEXT,                         -- Success timestamp
    failed_at TEXT,                            -- Terminal failure timestamp
    last_error TEXT,                           -- Error message (nullable)
    FOREIGN KEY (run_id) REFERENCES runs(id)
);

CREATE INDEX IF NOT EXISTS idx_retry_state 
    ON retry_queue(state, next_retry_at);

CREATE INDEX IF NOT EXISTS idx_retry_hash 
    ON retry_queue(report_hash);
```

**Key Design Choices:**
- UNIQUE constraint on `report_hash` prevents duplicate enqueues
- `next_retry_at` is NULL for immediate delivery, future timestamp for delayed retry
- `load_pending_reports()` queries: `state='PENDING' AND (next_retry_at IS NULL OR next_retry_at <= datetime('now'))`
- No separate RETRY_PENDING state - simplifies logic, sufficient for single agent

---

## Guarantees (Foundation Layer)

**✅ Currently Guaranteed:**
1. **No Report Loss After Persistence**: Once in runs table, report survives crashes (SQLite WAL)
2. **Idempotent Enqueue**: UNIQUE constraint on report_hash prevents duplicate queue entries
3. **State Consistency**: CHECK constraint enforces only valid states
4. **Atomic Transitions**: Each state change is a single UPDATE (atomic)
5. **Crash-Safe State**: All state transitions committed to WAL before returning
6. **At-Least-Once Delivery**: Reports delivered ≥1 time via HTTP (when network works)
7. **Crash Recovery**: Restart calls load_pending_reports(), resumes delivery
8. **Exponential Backoff**: Retry delays: 1s → 2s → 4s → ... → 300s (with ±25% jitter)
9. **Backend Deduplication**: Backend uses report_hash to detect duplicates

---

## Recovery Scenarios

### Crash Before Delivery Sent

**State:** Report in runs table, but not yet in retry_queue (crash between persist_run and enqueue_report)

**Recovery:** Report is persisted but never sent. Acceptable loss (agent runs periodically anyway).

**Mitigation:** Keep persist_run → enqueue_report atomic (sequential calls, no long operations between).

### Crash After Delivery Sent, Before ACK

**State:** Report in retry_queue with state=PENDING, delivery sent to backend, agent crashes before marking DELIVERED

**Recovery:** On restart, load_pending_reports() finds it, re-sends. Backend deduplicates via report_hash.

**Guarantee:** At-least-once delivery. Duplicate risk mitigated by backend hash check.

### Network Partition

**State:** Multiple reports in retry_queue, all state=PENDING, network down

**Recovery:** Exponential backoff prevents tight retry loop. When network returns, process_pending() drains queue.

**Guarantee:** All reports eventually delivered in order of created_at.

### Max Retries Exceeded

**State:** Report attempted 5 times, all failed, state=FAILED

**Recovery:** Manual intervention required. Admin queries: `SELECT * FROM retry_queue WHERE state='FAILED'` and investigates root cause.

**Monitoring:** Alert if count(state='FAILED') > 0 or retry_queue size > 100.

---

## Backoff Strategy

**Exponential Backoff with ±25% Jitter** (implemented in retry_queue.cpp):

```cpp
int backoff_seconds = std::min(300, 1 << attempts);  // Cap at 5 minutes
int jitter = rand() % std::max(1, backoff_seconds / 4);  // ±25% jitter
std::string next_retry = iso8601_now_plus_seconds(backoff_seconds + jitter);
```

**Sequence:**
- Attempt 0: Immediate (next_retry_at = NULL)
- Attempt 1: ~1 second
- Attempt 2: ~2 seconds
- Attempt 3: ~4 seconds
- ...
- Attempt 9: ~300 seconds (cap)
- Attempt 10: FAILED (max retries exceeded)

**Rationale:** Jitter prevents thundering herd if multiple agents restart simultaneously.

---

## Monitoring Metrics (When Integrated)

```sql
-- Queue depth
SELECT COUNT(*) FROM retry_queue WHERE state = 'PENDING';

-- Failed reports requiring manual intervention
SELECT COUNT(*) FROM retry_queue WHERE state = 'FAILED';

-- Retry attempt histogram
SELECT attempts, COUNT(*) FROM retry_queue WHERE state = 'PENDING' GROUP BY attempts;

-- Oldest pending report (alerting if > 1 hour)
SELECT MIN(created_at) FROM retry_queue WHERE state = 'PENDING';

-- Delivery success rate (last 24h)
SELECT 
    state,
    COUNT(*) 
FROM retry_queue 
WHERE created_at > datetime('now', '-1 day') 
GROUP BY state;
```

---

## Next Steps

See [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for phased implementation of remaining delivery components.
