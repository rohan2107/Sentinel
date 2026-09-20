# Roadmap

What is planned next and why, in priority order. The short-term list hardens the
existing agent; the long-term list builds out the delivery path beyond it.

Two principles set the order:

1. **Correctness before new surface.** Work that closes a known gap in the
   current behaviour ranks above work that adds capability.
2. **One path, finished, before breadth.** The delivery story — suppressed,
   queued, stored, measured — is completed end to end before further components
   are added. The Go aggregator is where that path currently stops.

Current behaviour is described in [`architecture/README.md`](../architecture/README.md);
its [Known gaps](../architecture/README.md#known-gaps) section is the input to
this list.

---

## Short term

### 1. Heartbeat

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

### 2. Bound the retry queue

`retry_queue` has no size cap, no age cutoff and no pruning of terminal rows. It
holds the full `report_json` per entry, so an agent that cannot reach its backend
accumulates rows indefinitely, and `DELIVERED` and `FAILED` rows are never
removed even after they have served their purpose. On an endpoint — a laptop that
sleeps, changes network, or sits behind a captive portal — partitions are normal
operating conditions rather than incidents.

Suppression masks this rather than fixing it: unchanged posture no longer
enqueues at all, so a stable offline host now grows the queue slowly. The case
that defeats both is a **flapping** host — a VPN or firewall check toggling as
the machine moves between networks — which produces a genuine posture change
every cycle *and* has nowhere to deliver it. That is the common endpoint case,
not an edge case.

**Simple time-based eviction, first.** Predictable, easy to reason about under
partition, and easy to state in the guarantees:

- Delete terminal rows (`DELIVERED`, `FAILED`) older than a retention window.
  These are history; they carry no delivery obligation.
- Drop `PENDING` rows older than a maximum age, **oldest first**. A report from
  last week has little value once newer ones for the same host exist, whereas
  evicting the newest would discard the most recent known state.
- Both windows configurable, with defaults documented.

Two interactions to get right:

*It changes the delivery guarantee.* Evicting a `PENDING` report deliberately
abandons it, so at-least-once becomes at-least-once **within the retention
window**. That is a legitimate trade for a bounded agent, but it is a semantic
change and
[`architecture/README.md`](../architecture/README.md#what-does-not-hold) must say
so rather than continuing to claim the stronger property.

*It interacts with suppression.* `last_reported_posture_hash` reads the newest
non-`FAILED` row. If eviction removes the row it was reading, the next evaluation
sees a different or absent last-reported posture and re-reports. That is the safe
direction — biased toward sending — but it should be a tested behaviour rather
than an accident.

**Priority-based eviction, later.** A refinement to build once the simple version
is in place and provides a baseline to compare against.

Not all reports are equally valuable. For compliance, what matters is the current
state plus the *transitions between states*, not every sample. Under pressure the
queue should keep a posture **degradation** — a control switching off, a score
dropping — and shed the intermediate samples of a flap, which collapse to "this
host toggled between A and B N times" without losing meaning. The rule weights in
the policy already encode severity, and the score delta between consecutive
reports gives the direction, so the inputs for ranking exist.

This is log compaction applied to posture history. It degrades along the axis
that matters (state transitions) rather than the one that is easy to measure
(age). It follows the simple evictor rather than replacing it: the simple version
bounds the disk, and the priority version is then evaluated against it.

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

Design the measurement after items 1–3, and collect it later. The mechanism works
and is tested but no number exists, and the number is the point. A credible
figure needs a fleet of simulated agents with a controlled posture-change rate,
which is [stage D](#d-load-simulation-and-failure-injection); the methodology is
fixed here so the number can't be tuned after the fact.

Design the measurement before collecting it:

- Fix the parameters first — evaluation interval, how often posture actually
  changes, fleet size, run count.
- **Count heartbeat bytes in the total.** Measuring suppression without
  heartbeats measures "stopped sending data", which is a different and weaker
  claim.
- State the assumption that posture is near-static, rather than letting the
  headline figure imply a general result.
- Report the distribution, not just a mean, and name the confound.

Done when the README can say something like "at a 60s evaluation interval with
posture changing every N hours, delivered bytes fall X% against unconditional
reporting, heartbeats included" and a script reproduces it. The design is done
with this item; the number arrives with stage D.

### 5. Compiler warnings

No warning flags are enabled anywhere in the build, on any platform. Enable
`-Wall -Wextra` (GCC/Clang) and `/W4` (MSVC), fix what surfaces, and only then
consider `-Werror` in CI. Small and self-contained; do it first so the rest of the
C++ work is written against it. Windows can only be checked in CI, so read that
log rather than guessing.

---

## Long term

Sequenced, not optional. The short-term list corrects gaps in the current
behaviour; everything here adds capability, and is built on top of that corrected
base.

Two databases appear below and are unrelated. **The agent's SQLite** is its local
retry queue and does not change. **The aggregator's store** is separate, and is
what the storage work in A1 is about.

A stage is complete when it is tagged and documented, with measured results and
their assumptions stated, and can be reproduced from a script or a running
deployment.

### A. Go aggregator, instrumented

Today it verifies the hash, deduplicates against an in-memory LRU, and **discards
the report**, so it does less than the Python backend it is intended to replace.
The module scaffold, `POST /reports` handler and LRU cache are in place; storage,
backpressure, metrics and packaging remain.

**A1. Storage behind an interface, with a bounded worker pool.**
Put storage behind a small `Store` interface with two implementations:

- **SQLite** (`modernc.org/sqlite`) for single-node use and Compose. Schema
  mirrors the Python backend so the two stay comparable:
  `received_reports(report_hash PRIMARY KEY, report_json, received_at)`.
- **Postgres** for cluster mode ([stage C](#c-package-and-deploy)), so the
  aggregator can run as more than one replica.

Writes go through N worker goroutines reading a job channel. With SQLite this
avoids every request goroutine contending for its single write lock; with
Postgres it is what bounds concurrency and makes A2 possible.
Either way it is the load-bearing decision in the service.

Two-layer dedup becomes real here. The LRU is a per-process hot path and nothing
more; the database's unique constraint on `report_hash`
(`INSERT ... ON CONFLICT DO NOTHING`) is the source of truth, and is what keeps
dedup correct across replicas. A cold duplicate falls through from one layer to
the other. The metric for that split (`source=cache` vs `source=db`) is what
makes the layering visible rather than asserted.

*Driver choice:* `mattn/go-sqlite3` requires CGO, which undercuts the single
static binary that is a main reason for using Go and complicates a minimal
container image. `modernc.org/sqlite` is pure Go and avoids both.

**A2. Backpressure.**
When the write channel is full, return `503` with `Retry-After: 1`. The agent's
exponential backoff then handles it with no new code on the agent side.

This is the central design property of the service: two components built
independently whose failure semantics compose, because both follow the same
delivery model. A test should fill the pool deliberately and assert the header,
since the behaviour is the property.

**A3. Metrics, logs, profiling, SLOs.**
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

**Heartbeats must be counted separately from reports.** Without that split, the
metrics cannot distinguish "quiet fleet" from "dead fleet", and the traffic
measurement would overstate the reduction by omitting heartbeat volume.

Alongside the counters:

- Rate, errors and duration per endpoint.
- Structured logs carrying a request id.
- `pprof` profiling, with at least one before/after optimisation driven by a real
  measurement. Don't manufacture a bottleneck; if the profile is flat, say so.
- `go test -bench` benchmarks run in CI, so a regression is visible.
- **SLOs with alert rules**: ingest availability, p99 ingest latency, and
  **posture freshness** (how long since each host was last heard from). Freshness
  is the one that depends on the heartbeat, and is the reason the heartbeat is
  first in line.

Distributed tracing is optional. With one service it shows little; it becomes
worthwhile only if context is propagated from the load generator through to the
database call.

### B. Ingest authentication

The endpoint is currently unauthenticated plain HTTP, which is a gap for a
security-compliance product and should not be packaged and deployed as it is.
Add minimal per-agent authentication (a token or a client certificate) before
[stage C](#c-package-and-deploy), so that a deployed instance is not open to
anything that can reach it.

This is the small first slice of transport security. The rest — full mTLS with
per-agent identity, report signing — is under [optional work](#optional-work)
below.

### C. Package and deploy

1. **Container and Compose.** Multi-stage Dockerfile, then one `docker-compose up`
   bringing up aggregator + Prometheus + Grafana with anonymous read access. This
   lets the receiving side be run without building the C++ agent and its vcpkg
   toolchain, and gives A3's metrics a dashboard to read them from.
2. **Helm on a local cluster** (kind or k3d; nothing here needs paid
   infrastructure). A chart deploying aggregator, Postgres, Prometheus and
   Grafana, with RBAC, a HorizontalPodAutoscaler, a PodDisruptionBudget and a
   rolling upgrade. Postgres is what makes more than one replica sound: the
   database, not each pod's local file, holds the dedup truth.
3. **A thin Terraform layer** using the Docker, Kubernetes and Helm providers
   against that local cluster. It manages local infrastructure only and does not
   provision cloud resources.

### D. Load simulation and failure injection

A Go tool spawning N simulated agents as goroutines, each with independent
posture that changes on a schedule, producing reproducible benchmarks.

The parameter that matters is **posture change rate**, because that is precisely
what determines whether suppression helps — a fleet whose posture never changes
shows a 99% reduction, and one that changes every cycle shows none. Reporting a
single number without naming this rate would be meaningless. It also needs a
configurable duplicate rate and failure rate so the dedup and retry paths are
exercised under load rather than assumed.

Then break things deliberately: kill the aggregator, slow or lock the database,
partition the network, fill the disk. For each, assert the delivery guarantee and
the backpressure behaviour rather than eyeballing them, and write one or two
postmortems and a runbook from the incidents that surface. They are induced
incidents, and should be labelled as such.

This is also where the [traffic-reduction number](#4-measure-the-traffic-reduction-properly)
finally gets collected, against the methodology fixed earlier.

Depends on A1 and A3: there is nothing to load until the aggregator stores, and
nothing to read until it has metrics.

### E. C++ agent hardening

The agent parses input it doesn't control (osquery output, policy JSON, Lua source
shipped as data) and is otherwise untouched after the short-term list. Two
additions, placed here because they need the code paths from items 1–2 to
exist, and because it returns to the C++ agent between the aggregator and
infrastructure stages:

- **Sanitizers in CI.** ASan and UBSan on the C++ test binaries, with TSan if
  anything concurrent appears. Cheap enough to pull forward if a memory bug
  turns up first.
- **Fuzzing.** Targets for the canonicalizer and the Lua evaluator's JSON-to-Lua
  conversion, and for policy JSON parsing. The canonicalizer is the sharpest
  target: the three-language contract already has documented divergences, and a
  fuzzer is a systematic way to find the ones nobody thought of.

Done when a sanitizer job is required in CI and each fuzz target has a corpus, a
recorded run length, and any bugs it found written up.

### F. Sentinel Operator

A Kubernetes operator (Go, kubebuilder / controller-runtime) that manages the
aggregator. A custom resource such as `SentinelAggregator` declares replicas,
retention, storage and SLO configuration; the controller reconciles the
Deployment, Service, ConfigMap and HPA to match, and reports status conditions.

The parts that make it more than a scaffold: finalizers, leader election, a
validating admission webhook, tests using envtest, and controller metrics.
Retention and storage are real inputs here, not decoration, because A1 and the
retry-queue work make them real concerns.

Depends on stage C, since it reconciles what C deploys by hand. Certificate
rotation driven by the operator is a natural extension of
[B](#b-ingest-authentication) and is the natural motivation for the fuller
transport-security work.

### G. Performance and design write-up

The README section this arc supports: architecture diagram, latency
percentiles under load, dedup efficiency, backpressure behaviour, and the traffic
reduction with its assumptions stated. Add a short set of design records for the
decisions that were actually contested (storage interface, why the LRU is only an
optimisation, retention semantics) and link the postmortems.

How each figure was measured matters as much as the figure: the measurement
design is fixed in advance, and known confounds are named alongside the results.

---

## Optional work

Not in the main sequence. Do it if there is a concrete reason.

- **MQTT delivery client.** `MqttDeliveryClient` implements the existing
  `DeliveryClient` interface, so the retry queue, scoring and persistence are
  untouched — HTTP proved the pattern, MQTT proves the abstraction was real. Cheap
  once Compose exists (Mosquitto is one more service). A design question it
  raises: **if QoS 1 already guarantees at-least-once, is the retry queue
  redundant?** No. QoS 1 covers an in-flight message between a connected client
  and the broker; the queue survives process death, broker unavailability and
  host reboot. They are layers at different lifetimes, not duplicates.
- **Full transport security.** mTLS with client certificates, per-agent identity,
  and report signing, beyond the minimal authentication in stage B.
- **Kafka ingest path** (consumer groups, lag metrics) and a small load generator
  in another language. Only if something specific needs them.

---

## Also worth doing, not sequenced

These do not depend on each other or on the arc above.

### Versioned schema migrations

Changes are currently additive: `CREATE TABLE IF NOT EXISTS` plus a guarded
`ALTER TABLE ... ADD COLUMN`. That is sufficient so far, but the first
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
  host, independent; coordination is outside this project's scope.
- **Rewriting the agent in another language.** The C++ agent is the
  core of the project, and a native, low-footprint host agent is the point of it.
- **A UI.** The reports are JSON and the metrics story belongs in Grafana.
