# Architectural Trade-Offs

Key decisions with alternatives, benefits, limitations, and failure thresholds.

## Decision Matrix

| Decision | Chosen | Alternative | Benefit | Limitation | Fails At |
|----------|--------|-------------|---------|------------|----------|
| **Delivery** | At-least-once | Exactly-once | Simpler (500 vs 2000 LOC), 50ms vs 200ms latency, any broker | Requires idempotent backend | Expensive non-idempotent ops |
| **Persistence** | SQLite | RocksDB | SQL queries, ACID, 700KB, ubiquitous | 10k writes/sec | >10k sustained writes/sec |
| **Network** | MQTT | HTTP poll | Push (100ms vs 30s), 0.27 vs 27 bits/sec keepalive | Broker required | <100 devices, firewalls |
| **Rules** | Lua | Compiled C++ | Dynamic updates, human-readable, 200KB VM | 100x slower | Complex rules, µs latency |
| **Dedup** | Content hash | Sequence ID | Idempotent, no coordination, tamper-detect | 32B vs 8B overhead | Mutable reports |

## Detailed Analysis

### 1. At-Least-Once Delivery

**Why**: Simpler than 2PC, lower latency (50ms vs 200ms), works with all MQTT brokers.

**Cost**: Backend must deduplicate (SHA-256 hash). Storage: 24h TTL = 100MB/1M reports.

**Fails When**: Non-idempotent expensive operations (e.g., $10/report processing, financial transactions).

### 2. SQLite Persistence

**Why**: SQL queries for debugging, ACID transactions, WAL mode (1-5ms writes), 700KB binary, zero config.

**Alternatives**: RocksDB (100k writes/sec), LevelDB, in-memory+AOF.

**Benchmark** (SSD): SQLite P50 0.8ms / P99 15ms / 12k writes/sec vs  RocksDB P50 0.3ms / P99 2ms / 85k writes/sec.

**Fails When**: >10k sustained writes/sec, P99 <5ms required, no SQL needed (KV only).

### 3. MQTT vs HTTP

**Why**: Push model (policy updates instant vs 30s poll avg), bandwidth efficient (2B vs 200B keepalive), topic routing scales.

**Cost**: Additional broker infrastructure, port 1883 firewall issues, harder to debug than HTTP.

**Fails When**: <100 devices (broker overhead not justified), firewall traversal critical, stateless required.

**Hybrid**: MQTT for policy push, HTTP for reports (simpler debugging).

### 4. Embedded Lua

**Why**: Deploy rules as JSON (no recompile), sandboxed (no I/O), human-readable, 200KB VM, 0.1-1ms eval.

**Cost**: 100x slower than C++. Security: malicious infinite loops (mitigated by 1s timeout + instruction limit).

**Fails When**: Complex rules (>100 LOC), µs latency required, formal verification needed.

**Hybrid**: Critical rules in C++, custom rules in Lua.

### 5. Content-Addressable Hashing

**Why**: Deterministic dedup (same content→same hash), no global coordination, backend idempotency, immutability verification.

**Cost**: SHA-256 collision at 2^-256 (negligible), 32B vs 8B storage, content-sensitive (whitespace changes hash—mitigated by normalization).

**Fails When**: Mutable reports, strict ordering required (use timestamp in payload), gap detection needed.

## Reconsider If

**Delivery**: Non-idempotent backend ops, duplicate cost > exactly-once implementation.

**SQLite**: Writes >10k/sec, P99 <5ms.

**MQTT**: <100 devices, firewall traversal primary issue.

**Lua**: Rules become CPU bottleneck (>50%), formal verification required (aerospace/medical).

**Hashing**: Reports mutable, strict ordering for compliance.
