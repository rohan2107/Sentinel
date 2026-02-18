# Failure Scenarios (Planned)

> ⚠️ **Status: Planned Feature - Not Yet Implemented**
>
> This document describes failure handling for the delivery layer (Phase 2).
> Current implementation only handles local evaluation failures (osquery timeout, Lua errors, disk full).
> Network and delivery failure scenarios described below are not yet implemented.

Analysis of failure modes, detection, and recovery mechanisms for the planned delivery layer.

## Scenario Matrix

| Scenario | Detection | Recovery | Preserved | Duplicate Risk |
|----------|-----------|----------|-----------|----------------|
| **Network Partition** | MQTT/HTTP connection timeout/refused | Exp backoff (1s→300s), drain queue on reconnect | At-least-once | Low |
| **Backend Unavailable** | MQTT/HTTP 503 or connection refused | Exp backoff, persistent retry queue | At-least-once | Low |
| **Crash Before ACK** | Pending in retry_queue on restart | Republish all pending via MQTT/HTTP | At-least-once | Medium |
| **Duplicate Delivery** | Backend hash check | Idempotent handler returns ACK | Idempotency | N/A |
| **Corrupt DB** | `SQLITE_CORRUPT`, integrity_check | `.recover` or fresh schema | System operational | None |
| **Large Backlog** | retry_queue size > 100 | Rate-limited drain (10/sec) | Eventual delivery | Low |

## Detailed Analysis

### 1. Network Partition

**Timeline**: Disconnect → MQTT/HTTP POST fails (connection refused) → exp backoff → accumulate in retry_queue → reconnect → drain queue.

**Behavior**: Evals continue locally, reports in RETRY_PENDING, backoff caps at 5min (300s).

**Recovery**: Query `SELECT * FROM retry_queue WHERE state IN ('PENDING', 'RETRY_PENDING')`, attempt MQTT PUBLISH (or HTTP POST), backend deduplicates via hash.

**Guarantees**: Reports delivered when network recovers. No data loss.

**Mitigation**: Queue cap 10k reports (~50MB), critical (score<20) alerts via syslog, backend staleness detection >2h.

### 2. Backend Unavailable

**What**: Backend service down/restarting → MQTT broker unavailable or HTTP 503 Service Unavailable.

**Recovery**: Exponential backoff (1s → 2s → 4s → ... → 300s), max 10 attempts, retry queue persists across agent restarts.

**Guarantees**: At-least-once. Reports delivered when backend recovers. Low duplicate risk (only if HTTP 200 OK lost in transit).

**Mitigation**: Backend health monitoring, auto-restart on crash, load balancer failover (if using multiple backend instances).

### 3. Crash Before Protocol Acknowledgement

**What**: Report persisted, MQTT PUBLISH sent (or HTTP POST), **agent crash before receiving acknowledgement**, backend may or may not have received it.

**Recovery**: Restart finds pending in retry_queue, re-sends via MQTT/HTTP. Backend deduplicates via `report_hash`.

**Guarantees**: At-least-once. Medium duplicate risk (if backend received but agent crashed before reading response).

**Why Not Exactly-Once**: Requires distributed 2PC (agent+backend coordination), 10x latency, complex failure modes.

### 4. Duplicate Delivery
MQTT/HTTP delivery succeeds at backend → backend ACKs (PUBACK or 200 OK) → network timeout before agent receives response → agent retries → backend receives duplicate.

**Detection**: Backend `SELECT FROM reports WHERE report_hash=?` or in-memory cache (LRU for recent reports). Alternative: Bloom filter (1% false positive rate).

**Recovery**: Backend returns ACK (PUBACK for MQTT, 200 OK for HTTP) - idempotent, accepts duplicates. Agent marks DELIVERED on any acknowledgement.

**Guarantees**: Idempotent processing. No double-counting.

### 5. Corrupt Database

**What**: Disk failure, bad RAM, unclean shutdown → `SQLITE_CORRUPT`.

**Detection**: `PRAGMA integrity_check` on startup.

**Recovery**:
1. Backup corrupt file
2. `sqlite3 db.sqlite3 .recover > dump.sql`
3. Recreate from dump or start fresh
4. Query backend for last 100 reports, reconcile

**Guarantees**: System continues. Historical data may be lost. Pending reports lost (acceptable).

**Mitigation**: WAL mode, ECC RAM, daily cloud backup, SMART monitoring.

### 6. Large Backlog Replay

**What**: Agent offline 7 days → 2,016 pending reports → reconnect → burst.

**Recovery**: Rate-limited drain 10 msg/sec, recent reports first, discard >7 days old.

**Guarantees**: All delivered eventually. Backend rate limiter protects against burst.

**Mitigation**: Adaptive rate increase, backend auto-scale, summarize old reports (single aggregate for 7d).

**Guarantees**: All delivered eventually. Backend rate limiter protects.

**Mitigation**: Adaptive rate increase, backend auto-scale, summarize old reports.
