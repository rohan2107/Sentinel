# Roadmap

What is next and why, in priority order. This replaces the previous
`docs/roadmap/` directory, which had grown into several thousand words of plans
for work that was either already finished or never started.

Two rules this list tries to follow:

1. **Close gaps before adding surface.** The repo already claims more capability
   than it has. Every item that fixes something claimed-but-absent ranks above
   every item that adds something new.
2. **Depth on one path beats breadth across many.** Finishing the delivery story
   end to end — suppressed, queued, stored, measured — is worth more than five
   half-built components. This is not an argument for doing less; it is an
   argument for finishing what is started, which is exactly what the Go service
   needs.

Current state is described in [`architecture/README.md`](../architecture/README.md);
its [Known gaps](../architecture/README.md#known-gaps) section is the input to
this list.

---

## Short term

### 1. Bound Lua execution — *the one real bug*

A policy containing `while true do end` hangs the agent forever. There is no
timeout despite three documents having claimed one. Measured: a rule running a
3-billion-iteration loop ran 9.75s and returned normally.

This matters more than anything else here because policies are *data* that the
evaluation loop executes, so this is reachable by a bad policy file, not just by
a bug.

The design decision to make: `lua_sethook` with `LUA_MASKCOUNT` is simple but
counts VM instructions rather than time, so the effective limit varies with what
the rule does. A wall-clock check inside the hook is more faithful but needs care
about where it is safe to raise an error from. Either way the sandbox stays a
resource bound and not a security boundary — policy authorship remains trusted,
and that should stay written down.

Done when: a rule that exceeds the bound is recorded as failed, the agent
continues to the next rule, and a test asserts it with a real runaway loop.

### 2. Heartbeat

Now that unchanged posture is suppressed, silence is ambiguous: a healthy agent
and a dead one look identical. Suppression is only correct if liveness is
reported separately.

Minimum viable: `POST /heartbeat` carrying `{hostname, posture_hash, timestamp}`
on every evaluation where delivery was suppressed, with receivers tracking
last-seen per host. Sending `posture_hash` lets a receiver detect the case where
its view of a host disagrees with what the host currently thinks, and ask for a
full report.

Note this changes the wire format, so
[`tests/canonicalization`](../tests/canonicalization/README.md) is the guard —
expect `golden.json` to change and review that diff deliberately.

### 3. Validate the number range at the hash boundary

The canonicalization contract supports integers within ±(2^53−1) and no floats.
Outside that, the three implementations disagree, because Go decodes JSON numbers
into `float64`. Today an out-of-range value produces a hash one side cannot
reproduce, surfacing as a permanent `400 hash mismatch` rather than a clear
error.

Done when: values outside the contract are rejected with a specific message at
the agent *and* at both receivers, and the two `expect: "diverge"` fixtures
become rejection tests.

### 4. Measure the traffic reduction properly

Only after items 1–3. The mechanism works and is tested but no number exists,
and the number is the point.

Design the measurement before collecting it:

- Fix the parameters first — evaluation interval, how often posture actually
  changes, fleet size, run count.
- **Count heartbeat bytes in the total.** Measuring suppression without
  heartbeats measures "stopped sending data", which is a different and much less
  interesting claim.
- State the assumption that posture is near-static, rather than letting the
  headline figure imply a general result.
- Report the distribution, not just a mean, and name the confound.

Done when the README can say something like "at a 60s evaluation interval with
posture changing every N hours, delivered bytes fall X% against unconditional
reporting, heartbeats included" and a script reproduces it.

### 5. Finish the honest-README pass

Corrected so far: the Lua timeout, the resource bounds, and the scoring formula.
Still overstated: "PRODUCTION-READY", "Comprehensive testing", and a Phase 2
checklist that reads as more complete than the code is. This is the cheapest item
on the list and the one that most changes how the repo reads to someone opening
it cold.

---

## Long term

These are sequenced, not optional-and-unordered. The original `PHASE3_PLAN.md`
was a good plan that stopped roughly 40% in, and the part it stopped before is
where all of its engineering substance lives. The plan is restored here rather
than trimmed, with the corrections that re-reading it suggested.

Ordering principle: the short-term list above closes gaps between what the repo
claims and what it does. Everything here adds capability. Gaps first, but "gaps
first" does not mean "capability never" — it means don't build the demo on top of
a known hang.

### A. Finish the Go aggregator

Today it verifies a hash, deduplicates against an in-memory LRU, and **discards
the report** — strictly less functional than the Python backend it was written to
replace. Steps 1–3 of the original plan (module scaffold, `POST /reports`
handler, LRU cache) are done. Steps 4–7 are not, and they carry the whole point.

**A1. Storage with a bounded worker pool.**
SQLite permits exactly one writer. Funnel writes through N worker goroutines
reading a job channel, rather than letting every request goroutine contend for
the write lock. This is the load-bearing design decision in the service.

Schema mirrors the Python backend so the two stay comparable:
`received_reports(report_hash PRIMARY KEY, report_json, received_at)`.

Two-layer dedup becomes real here: LRU is the hot path, the database is the
source of truth, and a cold duplicate falls through from one to the other. The
metric for that split (`source=cache` vs `source=db`) is what makes the layering
visible rather than asserted.

*Correction to the original plan:* it specified `mattn/go-sqlite3`, which
requires CGO — and that undercuts the "single static binary" reason the plan gave
for choosing Go in the first place, and complicates the Alpine container.
`modernc.org/sqlite` is pure Go and avoids both problems. Worth using unless
something needs the C library specifically.

**A2. Backpressure.**
When the write channel is full, return `503` with `Retry-After: 1`. The agent's
exponential backoff then handles it with no new code on the agent side.

This is the most interesting thing in the whole service: two components built
independently, whose failure semantics compose because both were designed around
the same delivery model. Worth a test that fills the pool deliberately and
asserts the header, because the behaviour is the claim.

**A3. Prometheus metrics.**
```
sentinel_reports_received_total          counter
sentinel_reports_deduplicated_total      counter  label: source=cache|db
sentinel_reports_stored_total            counter
sentinel_reports_failed_total            counter  label: reason=hash_mismatch|backpressure
sentinel_report_latency_seconds          histogram
sentinel_dedup_cache_hit_ratio           gauge
sentinel_storage_queue_depth             gauge
sentinel_active_connections              gauge
```

Add one dimension the original plan could not have anticipated, because
suppression did not exist yet: **heartbeats must be counted separately from
reports.** Without that split, the metrics cannot distinguish "quiet fleet" from
"dead fleet", and the traffic-reduction measurement below would flatter itself by
ignoring heartbeat volume.

**A4. Container and Compose.**
Multi-stage Dockerfile, then one `docker-compose up` bringing up aggregator +
Prometheus + Grafana with anonymous read access.

This is not polish. It is the difference between a repo someone reads and a
system someone runs — nobody evaluating this will install vcpkg and build a C++
agent first. It is also the part that turns A3 into a screenshot.

### B. Load simulation

A Go tool spawning N simulated agents as goroutines, each with independent
posture that changes on a schedule, producing reproducible benchmarks.

The parameter that matters is **posture change rate**, because that is precisely
what determines whether suppression helps — a fleet whose posture never changes
shows a 99% reduction, and one that changes every cycle shows none. Reporting a
single number without naming this rate would be meaningless. Also needs a
configurable duplicate rate and failure rate so the dedup and retry paths are
exercised under load rather than assumed.

Depends on A1 and A3: there is nothing to load until the aggregator stores, and
nothing to read until it has metrics.

### C. Performance write-up

The README section the whole arc exists to support: architecture diagram,
latency percentiles under load, dedup efficiency, backpressure behaviour, and the
traffic reduction with its assumptions stated.

This is where the CV claim becomes defensible instead of asserted — and the value
is as much in *how* it is measured as in the number, since a pre-specified
measurement design with named confounds is rarer than a benchmark.

### D. MQTT delivery client

I previously called this expensive infrastructure work. The original plan had
already solved that by ordering it after Compose: once the stack is running,
**adding Mosquitto is a one-service addition**, and the certificate and topic
work is the same work transport security needs anyway. Sequenced this way it is
cheap, which is why it belongs in the arc rather than off the end of it.

`MqttDeliveryClient` implements the existing `DeliveryClient` interface, so the
retry queue, scoring and persistence are untouched. HTTP proved the pattern;
MQTT proves the abstraction was real and not decorative.

The genuinely interesting question it raises — and the one to have an answer
ready for, because it is the obvious thing to probe: **if MQTT QoS 1 already
guarantees at-least-once, is the agent's retry queue redundant?** No. QoS 1
covers an in-flight message between a connected client and the broker. The queue
survives process death, broker unavailability and host reboot, none of which QoS 1
addresses. They are layers at different lifetimes, not duplicates. A broker
retaining the latest policy per topic then gives reconnecting agents self-healing
state on top.

Worth noting this is also the part of the project that most directly re-evidences
prior work — owning a mutual-TLS Mosquitto deployment, a multi-tenant topic
hierarchy and versioned idempotent policy delivery is on the CV but not currently
demonstrable anywhere public. Same argument as Sentinel existing at all.

See [transport security](#e-transport-security-and-authentication) below; the
certificate work overlaps almost entirely.

### E. Transport security and authentication

Plain HTTP with no authentication today. mTLS with client certificates is the
natural fit, matches the broker model, and gives per-agent identity, which is a
prerequisite for taking a report's origin seriously. Report signing would follow.

---

## Also worth doing, not sequenced

These do not depend on each other or on the arc above.

### Versioned schema migrations

Changes are currently additive: `CREATE TABLE IF NOT EXISTS` plus a guarded
`ALTER TABLE ... ADD COLUMN`. That is honest and sufficient so far, but the first
change that needs to rewrite or drop data will need ordered, versioned,
idempotent migrations with a recorded schema version.

### Failure handling the agent does not have

From the [failure scenarios](../architecture/README.md#failure-scenarios) table:
corrupt-database detection and recovery, and rate-limited drain of a large
backlog after a long outage. Both are real production concerns and neither is
handled.

### Enforce foreign keys

`PRAGMA foreign_keys=ON` is never set, so the declared constraints do not
execute. Enabling it is one line, but it may surface existing violations, so it
belongs with the migration work rather than as a drive-by change.

---

## Explicitly not planned

- **Fleet coordination, clustering, agent-to-agent anything.** One agent per
  host, independent. Scope creep with no learning payoff here.
- **Rewriting the agent in another language.** The C++ agent is the part that
  most closely evidences prior work; replacing it removes the reason it exists.
- **A UI.** The reports are JSON and the metrics story belongs in Grafana.
