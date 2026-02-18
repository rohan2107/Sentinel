# Delivery Guarantees (Planned)

> ⚠️ **Status: Planned Feature - Not Yet Implemented**
>
> This document describes the delivery semantics that will be implemented in Phase 2.
> Currently (Phase 1), Sentinel only implements local evaluation and SQLite persistence.
> None of the delivery guarantees below are active in the master branch.

---

## Planned Delivery Semantics

This document outlines the delivery guarantees that will be implemented when the delivery layer is added to Sentinel.

### Guaranteed Behaviors (Future)

| Guarantee | Formal Spec | Conditions | Fails When |
|-----------|-------------|------------|------------|
| **At-Least-Once Delivery** | `∀ report r: r ∈ SQLite → delivered to backend` | Report COMMITed, network eventually available, <10 retries | In-memory (not persisted), permanent network partition >7d, backend unavailable >7d |
| **Idempotent Policy** | `eval(p,s) = eval(p,eval(p,s))` | Static system state, deterministic osquery, pure Lua | Policy uses `os.time()`, osquery `RANDOM()`, concurrent state changes |
| **Eventual Consistency** | `backend_state(a, t+δ) = agent_state(a,t)` where `δ<5min` | At-least-once holds, backend keeps up, NTP sync | Backend queue backlog, agent offline for days |
| **Crash Recovery (Delivery)** | Reports in retry_queue will resume delivery after restart <60s | SQLite WAL intact, filesystem OK, agent restarted | In-memory delivery state lost |
| **Bounded Resources** | CPU <10% avg / <50% peak; Memory <100MB; Disk <1GB; Network <10KB/min | Eval interval ≥5min, osquery <1MB, queue <10k pending | Malicious policy (mitigated: 1s timeout), large osquery result (mitigated: 1MB limit) |

### What Will NOT Be Guaranteed

| Non-Guarantee | Why | Impact | Mitigation |
|---------------|-----|--------|------------|
| **Exactly-Once Delivery** ❌ | At-least-once chosen for simplicity | Duplicates at backend | Backend dedup via SHA-256 hash |
| **Ordered Delivery** ❌ | Network delays + retries reorder | Reports out-of-sequence | Backend sorts by timestamp field |
| **Real-Time Delivery** ❌ | Exponential backoff → 5min delays | Stale backend view | Detect "stale agent" if no reports >1h |
| **Byzantine Fault Tolerance** ❌ | Agents trusted (same org as backend) | Compromised agent can forge reports | (Future: TLS certs + report signing) |

### Infrastructure Assumptions

**OS**: Filesystem durable (fsync works), no kernel panics, fair scheduling.

**Network**: TCP reliable, eventual connectivity (no permanent partition), HTTP/MQTT backend eventually available.

**Backend**: Idempotent handler, hash-based deduplication, returns ACK for both new and duplicate reports.

**SQLite**: WAL crash-safe, atomic transactions, no silent corruption.

**Hardware**: Disk ≥100 IOPS, CPU capacity for agent+osquery, no RAM bit flips or bad sectors.

---

See [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for implementation timeline.
