# Failure Scenarios

Analysis of failure modes, detection, and recovery mechanisms.

## Scenario Matrix

| Scenario | Detection | Recovery | Preserved | Duplicate Risk |
|----------|-----------|----------|-----------|----------------|
| **Network Partition** | MQTT timeout, conn fail | Exp backoff (1s→300s), drain queue on reconnect | At-least-once | Low |
| **Broker Unavailable** | Conn fail all agents | Persistent session, replay unacked QoS 1 | At-least-once | Medium |
| **Crash Before ACK** | Pending in retry_queue on restart | Republish all pending | At-least-once | High |
| **Duplicate Delivery** | Backend hash check | Idempotent handler ignores | Idempotency | N/A |
| **Corrupt DB** | `SQLITE_CORRUPT`, integrity_check | `.recover` or fresh schema | System operational | None |
| **Large Backlog** | retry_queue size > 100 | Rate-limited drain (10/sec) | Eventual delivery | Low |

## Detailed Analysis

### 1. Network Partition

**Timeline**: Disconnect → keepalive timeout (5s) → reconnect attempts with exp backoff → accumulate in retry_queue → reconnect → drain queue.

**Behavior**: Evals continue locally, reports in RETRY_PENDING, backoff caps at 5min.
**Recovery**: Query `SELECT * FROM retry_queue WHERE state IN ('PENDING', 'RETRY_PENDING')`, reconnect MQTT (persistent session), publish pending with hash for dedup.

**Guarantees**: Reports delivered when network recovers. No data loss.

**Mitigation**: Queue cap 10k reports (~50MB), critical (score<20) alerts via syslog, backend staleness detection >2h.

### 2. Broker Unavailable

**What**: Broker crash → all agents disconnect → thundering herd on restart.

**Recovery**: Persistent session (`clean_session=false`), broker replays unacked QoS 1, jittered reconnect (0-60s spread).

**Guarantees**: At-least-once. Subscriptions persist. May duplicate.

**Mitigation**: Broker clustering, k8s liveness probe, rate limit 500 conn/sec.

### 3. Crash Before ACK

**What**: Report persisted, PUBLISH sent, **agent crash before PUBACK**, broker delivers to backend, agent doesn't know.

**Recovery**: Restart finds pending in retry_queue, republishes. Backend deduplicates via `report_hash`.

**Guarantees**: At-least-once. High duplicate risk.

**Why Not Exactly-Once**: Requires distributed 2PC (agent+broker+backend), 10x latency, complex failure modes.

### 4. Duplicate Delivery

**What**: PUBACK lost in network congestion → agent timeout → retry → backend receives twice.

**Detection**: Backend `SELECT FROM reports WHERE report_hash=?` or Redis cache.

**Recovery**: Discard duplicate. Optional bloom filter (1% FP, 120MB for 240M hashes).

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

**Recovery**: Rate-limited drain 10 msg/sec, batch 100, recent reports first, discard >7 days.

**Guarantees**: All delivered eventually. Backend rate limiter protects.

**Mitigation**: Adaptive rate increase, backend auto-scale, summarize old reports.
