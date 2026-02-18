# Sentinel Architecture

Production-grade distributed agent for security compliance monitoring with at-least-once delivery, crash-safe persistence, and offline-first operation.

## System Architecture

```mermaid
graph TB
    subgraph "Agent Host"
        AC[Agent Core]
        PE[Policy Engine<br/>Lua Runtime]
        LP[(Local Persistence<br/>SQLite + WAL)]
        RQ[Retry Queue<br/>Durable]
        MC[MQTT Client<br/>QoS 1]
        OQ[osquery Engine]
        
        AC --> PE
        AC --> OQ
        PE --> AC
        OQ --> AC
        AC --> LP
        AC --> RQ
        RQ --> MC
        MC --> RQ
        LP -.persistence.-> RQ
    end
    
    subgraph "Network Boundary"
        NW[Network]
    end
    
    subgraph "Backend Infrastructure"
        MB[MQTT Broker<br/>Persistent Session]
        BS[Backend Server<br/>Idempotent Handler]
        BDB[(Backend Database)]
        
        MB --> BS
        BS --> BDB
    end
    
    MC -->|QoS 1<br/>at-least-once| NW
    NW --> MB
    
    classDef persistence fill:#e1f5ff,stroke:#01579b
    classDef network fill:#fff3e0,stroke:#e65100
    classDef compute fill:#f3e5f5,stroke:#4a148c
    
    class LP,RQ,BDB persistence
    class MC,MB,NW network
    class AC,PE,OQ,BS compute
```

## Components

**Agent Core**: Orchestrates evaluation lifecycle, state transitions, persistence, crash recovery.

**Policy Engine**: Sandboxed Lua runtime for deterministic rule evaluation.

**Local Persistence**: SQLite with WAL, atomic transactions, content-addressable deduplication.

**Retry Queue**: Durable (SQLite), exponential backoff, crash-safe.

**MQTT Client**: QoS 1, persistent session (clean_session=false), auto-reconnect.

**osquery**: System data collection, 10s timeout, 1MB output limit.

## Data Flow

**Normal**: Policy (MQTT) → Persist → osquery → Lua eval → Score → Persist report → Queue → MQTT publish → PUBACK → Mark delivered

**Crash Recovery**: Load retry queue → Reconnect MQTT → Replay pending → Resume

## Persistence

**Agent (SQLite)**: Policies, evaluations, retry queue, delivery status.

**Network (MQTT)**: Policy updates (backend→agent), reports (agent→backend).

**Backend (DB)**: Aggregated trends, device inventory, policy history.

## Network Guarantees

**Agent→Broker**: QoS 1, persistent session, content-hash deduplication.

**Broker→Backend**: Durable subscription, idempotent handler, hash-based duplicate detection.

## Design Principles

1. Offline-first operation
2. Crash-safe persistence
3. Idempotent backend
4. Explicit state machine
5. Bounded resources
6. Fail-safe defaults

## Scale

**Target**: 100k agents, 5-60min interval, Mosquitto/HiveMQ/AWS IoT Core.

**Bottlenecks**: SQLite ~10k writes/sec, broker ~50k connections, backend dedup overhead.

**Future**: RocksDB for writes, broker sharding, bloom filters for dedup.
