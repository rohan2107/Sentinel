# Delivery State Machine (Planned)

> ⚠️ **Status: Planned Feature - Not Yet Implemented**
>
> This document describes the delivery state machine for Phase 2.
> The current implementation only has IDLE → POLICY_LOADED → EVALUATING → PERSISTED states.
> Delivery states (PENDING_DELIVERY, RETRY_PENDING, DELIVERED, FAILED) do not exist yet.

Explicit state machine that will ensure at-least-once delivery with durable persistence at each boundary.

## States

| State | Entry  | Exit | Persisted | Failure |
|-------|--------|------|-----------|---------|
| **IDLE** | Empty queue | Policy loaded | None | File not found |
| **POLICY_LOADED** | File read | Validated | None | Invalid JSON→IDLE, validation error→log |
| **EVALUATING** | Policy validated | Rules evaluated | In-memory only | Timeout→FAIL+continue, Lua error→FAIL+continue |
| **REPORT_PERSISTED** | Eval complete | SQLite commit | `BEGIN; INSERT runs; INSERT features; COMMIT` | Retry 3x, disk full→alert |
| **PENDING_DELIVERY** | Queued | Delivery sent | `INSERT retry_queue (state='PENDING')` | Conn fail→RETRY_PENDING |
| **DELIVERED** | ACK received | Queue cleaned | `UPDATE retry_queue SET state='DELIVERED'` | Retry update |
| **RETRY_PENDING** | Delivery failed | Backoff expired | `UPDATE retry_queue SET attempts++, next_retry` | Max(10)→FAILED |
| **FAILED** | Max retries | Manual | `UPDATE retry_queue SET state='FAILED'` | Terminal |

## Guarantees

1. **No Report Loss**: Once REPORT_PERSISTED, survives crashes (WAL).
2. **At-Least-Once**: Reports delivered ≥1 time.
3. **Ordered Local**: Single agent in-order, backend may receive out-of-order.
4. **Crash Recovery**: Restart loads PENDING/RETRY_PENDING, resumes.
5. **Idempotency**: Backend deduplicates via `report_hash`.

## Recovery

**Crash in EVALUATING**: Eval lost, policy file persists. Re-eval on next run.

**Crash in PENDING_DELIVERY**: Report persisted. Restart replays from retry_queue. Backend deduplicates via SHA-256 hash.

**Network Partition**: Accumulate in RETRY_PENDING. Exp backoff. Drain on recovery.

**Backend Crash After 200 OK**: Agent marks DELIVERED. If backend didn't persist before crash, report lost (edge case, backend should persist atomically).

## Schema

```sql
CREATE TABLE retry_queue (
    run_id INTEGER PRIMARY KEY,
    report_hash TEXT UNIQUE NOT NULL,
    attempts INTEGER DEFAULT 0,
    state TEXT CHECK (state IN ('PENDING', 'RETRY_PENDING', 'DELIVERED', 'FAILED')),
    next_retry_at TEXT,
    delivered_at TEXT,
    failed_at TEXT,
    FOREIGN KEY (run_id) REFERENCES runs(id)
);
CREATE INDEX idx_retry_state ON retry_queue(state, next_retry_at);
```

## Backoff

```cpp
int backoff_s = min(300, 1 << attempts);  // Cap 5min
int jitter = rand() % (backoff_s / 4);     // ±25%
next_retry = now + backoff_s + jitter;
```

## Monitoring

- `retry_queue` size > 100
- `FAILED` state count
- p99 latency REPORT_PERSISTED→DELIVERED
- Retry attempts histogram
