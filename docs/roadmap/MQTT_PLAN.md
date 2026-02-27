# Phase 3.5 Implementation Plan: MQTT Delivery Client

> **Status:** ⏳ PLANNED — after Phase 3 (Go aggregator) is complete
> **Date:** February 27, 2026
> **Depends on:** Phase 3 (Go aggregator + Docker Compose) ✅

---

## Goal

Add an `MqttDeliveryClient` to the C++ agent, implementing the existing `DeliveryClient` interface with MQTT QoS 1 (at-least-once) semantics. This concretely demonstrates the pluggable delivery architecture that the interface was designed for — HTTP proved the pattern; MQTT proves the abstraction.

By Phase 3, Docker Compose already runs. Adding an MQTT broker to the stack is a one-service addition.

---

## Why MQTT Matters for the Portfolio

- **Protocol diversity**: HTTP and MQTT represent fundamentally different delivery models (request/response vs pub/sub). Supporting both shows architectural thinking.
- **QoS 1 at the protocol level**: MQTT QoS 1 guarantees at-least-once delivery natively, complementing the agent's own retry queue (defense-in-depth).
- **Real-world relevance**: MQTT is the dominant protocol for IoT/endpoint telemetry pipelines. Security agents at scale often use a message broker instead of direct HTTP.
- **Interview hook**: "The DeliveryClient interface allows us to swap HTTP for MQTT without touching the retry queue, scoring engine, or persistence layer — here's the MQTT client that proves it."

---

## Architecture

```
[C++ Agent]
  └── RetryQueue
        └── MqttDeliveryClient (QoS 1 publish)
              └── [Mosquitto broker] ← Docker Compose
                    └── [Go aggregator subscribes to sentinel/reports/#]
                          └── Dedup + SQLite storage
                          └── Prometheus metrics
```

The Go aggregator gains a second ingestion path (MQTT subscriber) alongside the existing HTTP `POST /reports` path. Both paths share the same dedup cache and storage layer.

---

## C++ Changes

### Dependency: paho-mqttpp3

```bash
vcpkg install paho-mqttpp3
```

Add to `vcpkg.json`:
```json
"paho-mqttpp3": ">=1.3.0"
```

Add to `CMakeLists.txt`:
```cmake
find_package(PahoMqttCpp CONFIG REQUIRED)
target_link_libraries(Sentinel PRIVATE PahoMqttCpp::PahoMqttCpp)
```

### New File: `src/mqtt_delivery_client.h` / `.cpp`

```cpp
// src/mqtt_delivery_client.h
#pragma once
#include "delivery_client.h"
#include <string>

// MQTT QoS 1 delivery client.
// Publishes to topic: sentinel/reports/<hostname>
// Payload: {"report": {...}, "hash": "..."}
// QoS 1 guarantees at-least-once delivery at the protocol level.
// Combined with the retry queue, this gives double at-least-once coverage.
class MqttDeliveryClient : public DeliveryClient {
public:
    MqttDeliveryClient(const std::string& broker_url,
                       const std::string& topic,
                       int qos = 1,
                       int connect_timeout_s = 10,
                       int publish_timeout_s = 30);

    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;

private:
    std::string broker_url_;
    std::string topic_;
    int qos_;
    int connect_timeout_s_;
    int publish_timeout_s_;
};
```

**Implementation notes:**
- Use `mqtt::connect_options` with `clean_session(true)` (stateless — retry queue owns persistence)
- Publish with `mqtt::message::create(topic, payload, qos, false)` (retain=false)
- On `mqtt::exception`: return `DeliveryResult{false, 0, e.what()}`
- Finalize/disconnect in destructor (RAII)
- No persistent MQTT session state needed — the SQLite retry queue owns durability

### CLI flag: `--delivery-mode`

Add to `main.cpp` argument parsing:

```
--delivery-mode http   (default, existing behavior)
--delivery-mode mqtt   (uses MqttDeliveryClient)
--broker-url           mqtt://localhost:1883  (used when --delivery-mode mqtt)
--mqtt-topic           sentinel/reports       (default topic prefix)
```

**Factory pattern in main.cpp:**
```cpp
std::unique_ptr<DeliveryClient> make_delivery_client(const CliOptions& opts) {
    if (opts.delivery_mode == "mqtt") {
        return std::make_unique<MqttDeliveryClient>(opts.broker_url, opts.mqtt_topic);
    }
    return std::make_unique<HttpDeliveryClient>(opts.backend_url);
}
```

---

## Go Aggregator Changes

The Go aggregator gains an MQTT subscriber alongside the existing HTTP server. Both paths share the dedup cache and storage.

### Dependency

```bash
go get github.com/eclipse/paho.mqtt.golang
```

### New File: `go-aggregator/internal/mqtt/subscriber.go`

```go
// Subscribes to sentinel/reports/# at QoS 1.
// On message receipt, runs the same dedup-then-store pipeline as the HTTP handler.
// This means a report delivered via MQTT gets the same Prometheus metrics,
// the same dedup guarantees, and the same idempotent storage.
type Subscriber struct {
    client  mqtt.Client
    cache   *dedup.SeenCache
    store   *storage.Store
    metrics *metrics.Registry
}

func (s *Subscriber) Start(brokerURL, topic string) error { ... }
func (s *Subscriber) handleMessage(client mqtt.Client, msg mqtt.Message) { ... }
func (s *Subscriber) Stop() { ... }
```

### Docker Compose addition

Add Mosquitto broker to `docker-compose.yml`:

```yaml
  mqtt-broker:
    image: eclipse-mosquitto:2
    ports:
      - "1883:1883"
    volumes:
      - ./mosquitto/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro
    restart: unless-stopped
```

**`mosquitto/mosquitto.conf`:**
```
listener 1883
allow_anonymous true
```

Aggregator subscribes on startup if `--mqtt-broker` flag is set:
```bash
# In docker-compose.yml aggregator service:
command: ["./aggregator", "--mqtt-broker", "mqtt://mqtt-broker:1883"]
```

---

## Testing

### C++ unit tests (extend `test_delivery_foundation.cpp`)

- `MqttDeliveryClient` with a local Mosquitto: publish → assert broker received message
- Failure path: broker unavailable → `DeliveryResult{false, 0, "..."}` (no panic)
- Retry queue drives retry correctly after MQTT failure

### Integration test

1. `docker-compose up` (includes Mosquitto broker)
2. Run agent with `--delivery-mode mqtt --broker-url mqtt://localhost:1883`
3. Verify Go aggregator receives report via MQTT subscriber
4. Verify `sentinel_reports_stored_total` increments in Prometheus
5. Verify duplicate suppression: send same report twice → second is deduplicated

---

## Verification Checklist

- [ ] `vcpkg install paho-mqttpp3` succeeds on Windows
- [ ] Zero MSVC `/W4` warnings from `mqtt_delivery_client.cpp`
- [ ] `--delivery-mode mqtt` flag routes through `MqttDeliveryClient`
- [ ] `--delivery-mode http` still works (no regression)
- [ ] All existing tests pass unchanged
- [ ] `docker-compose up` includes Mosquitto broker
- [ ] Go aggregator subscribes to `sentinel/reports/#` and processes messages
- [ ] Prometheus metrics reflect MQTT-delivered reports

---

## Quality Gates

- All tests pass: `.\scripts\test.ps1` (C++) + `go test ./...` (Go)
- Zero new MSVC `/W4` warnings
- `IMPLEMENTATION_PLAN.md` updated: MqttDeliveryClient ✅
- `README.md` architecture diagram updated to show MQTT path
- `docker-compose.yml` includes Mosquitto broker

---

## Interview Talking Points Unlocked

> "The DeliveryClient interface was designed for this — adding MQTT support required only a new implementation of the interface. The retry queue, scoring engine, and persistence layer were untouched. The Go aggregator subscribes to the MQTT topic and runs the same dedup-then-store pipeline as the HTTP path, so Prometheus metrics are accurate regardless of delivery protocol. MQTT QoS 1 gives at-least-once at the protocol level; combined with the SQLite retry queue, we have double at-least-once coverage — both can fail independently and the report still gets through."
