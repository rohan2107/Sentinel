# Architectural Trade-Offs (Current Implementation)

Key decisions for Phase 1 (local evaluation engine) with alternatives, benefits, limitations, and failure thresholds.

> **Note**: Delivery layer trade-offs (at-least-once vs exactly-once, MQTT vs HTTP, content hashing) are documented in [`docs/roadmap/`](roadmap/) as Phase 2 is not yet implemented.

## Decision Matrix

| Decision | Chosen | Alternative | Benefit | Limitation | Fails At |
|----------|--------|-------------|---------|------------|----------|
| **Persistence** | SQLite | RocksDB | SQL queries, ACID, 700KB, ubiquitous | 10k writes/sec | >10k sustained writes/sec |
| **Rules** | Lua | Compiled C++ | Dynamic updates, human-readable, 200KB VM | 100x slower | Complex rules, µs latency |
| **Data Collection** | osquery | WMI | Cross-platform, SQL interface, maintained | Windows-only in practice | Non-osquery data sources |
| **JSON Library** | nlohmann/json | RapidJSON | Header-only, intuitive API, wide adoption | Slower than RapidJSON | Parse >10MB/sec |

## Detailed Analysis

### 1. SQLite Persistence

**Why**: SQL queries for debugging, ACID transactions, WAL mode (1-5ms writes), 700KB binary, zero config.

**Alternatives**: RocksDB (100k writes/sec), LevelDB, in-memory+AOF.

**Benchmark** (SSD): SQLite P50 0.8ms / P99 15ms / 12k writes/sec vs  RocksDB P50 0.3ms / P99 2ms / 85k writes/sec.

**Fails When**: >10k sustained writes/sec, P99 <5ms required, no SQL needed (KV only).

### 2. Embedded Lua

**Why**: Deploy rules as JSON (no recompile), sandboxed (no I/O), human-readable, 200KB VM, 0.1-1ms eval.

**Cost**: 100x slower than C++. Security: malicious infinite loops (mitigated by 1s timeout + instruction limit).

**Fails When**: Complex rules (>100 LOC), µs latency required, formal verification needed.

**Hybrid**: Critical rules in C++, custom rules in Lua.

### 3. osquery Data Collection

**Why**: Cross-platform SQL interface to system data, actively maintained (Facebook → Osquery Foundation), rich table schema (firewall, processes, users).

**Cost**: 150ms latency per query, 3-5MB memory, Windows-only in practice (Linux support underused in Windows agent).

**Fails When**: Non-osquery data sources (custom APIs, SIEM), <50ms latency required, memory <10MB.

**Alternatives**: WMI (Windows-only, COM complexity), PowerShell (parsing overhead), native Win32 API (rigid code).

### 4. nlohmann/json

**Why**: Header-only (no build config), intuitive API (`j["key"]`), wide adoption, MIT license.

**Cost**: 2-3x slower than RapidJSON, 1-2MB header parse overhead.

**Fails When**: Parse >10MB/sec, minimal header-only footprint required, older C++11 constraints.

**Benchmark**: 12KB policy file → 0.3ms parse (nlohmann) vs 0.1ms (RapidJSON). Acceptable for 1 eval/5min.

## Reconsider If

**SQLite**: Writes >10k/sec, P99 <5ms required, no SQL needed.

**Lua**: Rules become CPU bottleneck (>50%), formal verification required (aerospace/medical), rule logic >100 LOC.

**osquery**: Need non-system data (cloud APIs, SIEM), latency <50ms required, cross-platform support unnecessary.

**nlohmann/json**: Parse >10MB/sec required, header-only footprint problematic, C++11 constraints.

---

**For delivery layer trade-offs** (at-least-once vs exactly-once, MQTT vs HTTP, content hashing), see [`docs/roadmap/`](roadmap/) documentation for Phase 2 planning.
