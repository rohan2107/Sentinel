# System Guarantees

Formal specification of Sentinel's guarantees and non-guarantees. Assumes correct operation of underlying infrastructure (OS, filesystem, network).

## Guaranteed Behaviors

| Guarantee | Formal Spec | Conditions | Fails When |
|-----------|-------------|------------|------------|
| **At-Least-Once Delivery** | `∀ report r: r ∈ SQLite → delivered to backend` | Report COMMITed, network eventually available, <10 retries | In-memory (not persisted), permanent network partition >7d, broker data loss |
| **Idempotent Policy** | `eval(p,s) = eval(p,eval(p,s))` | Static system state, deterministic osquery, pure Lua | Policy uses `os.time()`, osquery `RANDOM()`, concurrent state changes |
| **Eventual Consistency** | `backend_state(a, t+δ) = agent_state(a,t)` where `δ<5min` | At-least-once holds, backend keeps up, NTP sync | Backend queue backlog, agent offline for days |
| **Offline-First** | Network unavailable → agent evaluates + persists | Disk space >1GB, SQLite intact, osquery available | Disk full, SQLite corrupted |
| **Crash Recovery** | Reports persisted before crash remain after restart, delivery resumes <60s | SQLite WAL intact, filesystem OK, agent restarted | In-memory state lost, EVALUATING reports not persisted |
| **Bounded Resources** | CPU <10% avg / <50% peak; Memory <100MB; Disk <1GB; Network <10KB/min | Eval interval ≥5min, osquery <1MB, queue <10k pending | Malicious policy (mitigated: 1s timeout), large osquery result (mitigated: 1MB limit) |

### Verification Examples

**At-Least-Once**: Disconnect network, persist report, reconnect → delivered within 60s.

**Idempotent**: Run same policy twice on identical state → identical scores (`diff <(jq -S .score report1.json) <(jq -S .score report2.json)`).

**Crash Recovery**: `kill -9` mid-evaluation → report persists in SQLite → delivered after restart.

**Bounded Resources**: 1h monitoring: CPU <10%, memory <100MB RSS.


## Explicitly NOT Guaranteed

| Non-Guarantee | Why | Impact | Mitigation |
|---------------|-----|--------|------------|
| **Exactly-Once Delivery** ❌ | At-least-once chosen for simplicity | Duplicates at backend | Backend dedup via SHA-256 hash |
| **Ordered Delivery** ❌ | Network delays + retries reorder | Reports out-of-sequence | Backend sorts by timestamp field |
| **Real-Time Delivery** ❌ | Exponential backoff → 5min delays | Stale backend view | Detect "stale agent" if no reports >1h |
| **Byzantine Fault Tolerance** ❌ | Agents trusted (same org as backend) | Compromised agent can forge reports | (Future: TLS certs + report signing) |
| **Data Durability Beyond SQLite** ❌ | No local DB replication | Disk failure loses undelivered reports | Backend serves as remote backup |
| **Deterministic Eval Timing** ❌ | osquery time varies by system state | 10ms-5s range per query | Timeout ensures <10s max |

**Example: Exactly-Once Failure**
- Agent sends report → broker receives → PUBACK lost (network glitch) → agent retries → backend receives duplicate.

**Example: Ordered Delivery Failure**
- Report A (T=100) queued, Report B (T=200) delivered immediately, Report A retried at T=300 → backend receives B then A.

## Infrastructure Assumptions

**OS**: Filesystem durable (fsync works), no kernel panics, fair scheduling.

**Network**: TCP reliable, eventual connectivity (no permanent partition), no Byzantine failures (packet tampering).

**MQTT Broker**: Persistent sessions (clean_session=false), PUBACK only after durable storage, no data loss before backend forwarding.

**SQLite**: WAL crash-safe, atomic transactions, no silent corruption.

**Hardware**: Disk ≥100 IOPS, CPU capacity for agent+osquery, no RAM bit flips or bad sectors.

## SLOs (Targets, Not Hard Guarantees)

- **Availability**: 99.5% uptime (3.6h downtime/month failure budget)
- **Delivery Latency**: P95 <5min (report generated → backend receipt)
- **Data Loss**: <0.01% reports lost (excludes catastrophic disk failure, permanent partition)
- **Duplicate Rate**: <1% (alert if >5% indicates MQTT PUBACK timeout too short)

## Degraded Conditions

| Condition | Evaluation | Persistence | Delivery | Backend Updates |
|-----------|-----------|-------------|----------|-----------------|
| Network Partition | ✅ Continues | ✅ Local DB | ❌ Queued | ❌ Blocked |
| Broker Outage | ✅ Continues | ✅ Local DB | ❌ Queued | ❌ Blocked |
| Backend Outage | ✅ Continues | ✅ Local DB | ✅ Broker queues | ❌ Processing paused |
| Disk Full | ✅ Continues | ❌ Lost | ❌ No new reports | ✅ Existing queue OK |
| CPU Exhaustion | ⚠️ Slows (timeout enforced) | ⚠️ Delayed | ⚠️ MQTT thread starved | ⚠️ Degraded |
| Memory OOM | ❌ Killed by OS | ✅ Crash recovery on restart | ✅ Resumes | ❌ In-memory eval lost |

