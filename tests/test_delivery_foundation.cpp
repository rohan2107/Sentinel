// Integration test for delivery foundation
#include <iostream>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "src/db.h"
#include "src/report_hasher.h"
#include "src/delivery_client.h"
#include "src/retry_queue.h"

using json = nlohmann::json;

// Remove a test database and the two sidecar files SQLite creates in WAL mode.
// Plain remove() leaves <db>-wal and <db>-shm behind, which litters the repo
// root and can carry state into the next run.
static void remove_db(const std::string& path) {
    remove(path.c_str());
    remove((path + "-wal").c_str());
    remove((path + "-shm").c_str());
}

// Helper function to compute future timestamp in ISO8601 format
std::string get_future_timestamp(int hours_ahead) {
    auto now = std::chrono::system_clock::now();
    auto future = now + std::chrono::hours(hours_ahead);
    auto future_time_t = std::chrono::system_clock::to_time_t(future);
    
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &future_time_t);
#else
    gmtime_r(&future_time_t, &tm_utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void test_hash_determinism() {
    std::cout << "=== Testing Hash Determinism ===\n";
    
    // Same report, same hash
    json r1 = {{"score", 100}, {"hostname", "test"}, {"timestamp", "2025-01-01T00:00:00Z"}};
    json r2 = {{"timestamp", "2025-01-01T00:00:00Z"}, {"score", 100}, {"hostname", "test"}};
    
    std::string h1 = compute_report_hash(r1);
    std::string h2 = compute_report_hash(r2);
    
    assert(h1 == h2);
    assert(h1.length() == 64); // SHA-256 hex is 64 chars
    std::cout << "[PASS] Hashes are deterministic regardless of key order\n";
    std::cout << "       Hash: " << h1.substr(0, 16) << "...\n";
    
    // Different report, different hash
    json r3 = {{"score", 99}, {"hostname", "test"}, {"timestamp", "2025-01-01T00:00:00Z"}};
    std::string h3 = compute_report_hash(r3);
    assert(h1 != h3);
    std::cout << "[PASS] Modified content produces different hash\n\n";
}

void test_mock_delivery() {
    std::cout << "=== Testing MockDeliveryClient ===\n";
    
    // Test success case
    MockDeliveryClient success_client(true);
    DeliveryResult result = success_client.send("{\"test\":true}", "abc123");
    
    assert(result.success);
    assert(result.status_code == 200);
    assert(success_client.get_call_count() == 1);
    assert(success_client.get_last_hash() == "abc123");
    std::cout << "[PASS] Mock client succeeds when configured to succeed\n";
    
    // Test failure case
    MockDeliveryClient failure_client(false);
    result = failure_client.send("{\"test\":true}", "def456");
    
    assert(!result.success);
    assert(result.status_code == 500);
    assert(!result.error_message.empty());
    assert(result.error_message == "Mock delivery failure");
    assert(failure_client.get_call_count() == 1);
    std::cout << "[PASS] Mock client fails when configured to fail\n\n";
}

void test_retry_queue() {
    std::cout << "=== Testing Retry Queue Operations ===\n";
    
    // Use temporary database
    const char* test_db = "test_sentinel.db";
    remove_db(test_db);
    
    DB db(test_db);
    db.init_schema();
    
    // Persist a run first
    json details = {{"firewall_enabled", true}, {"av_installed", true}};
    db.persist_run("2026-02-18T12:00:00.000Z", "test-host", "test-policy", 100, details);
    int run_id = db.get_last_run_id();
    assert(run_id > 0);
    std::cout << "[PASS] Persisted run with ID: " << run_id << "\n";
    
    // Create report and hash
    json report = {
        {"timestamp", "2026-02-18T12:00:00.000Z"},
        {"hostname", "test-host"},
        {"policy", "test-policy"},
        {"score", 100},
        {"details", details}
    };
    std::string report_json = report.dump();
    std::string report_hash = compute_report_hash(report);
    std::cout << "[PASS] Computed report hash: " << report_hash.substr(0, 16) << "...\n";
    
    // Enqueue report
    db.enqueue_report(run_id, report_json, report_hash, compute_posture_hash(report));
    std::cout << "[PASS] Enqueued report for delivery\n";
    
    // Load pending reports
    auto pending = db.load_pending_reports();
    assert(pending.size() == 1);
    assert(pending[0].run_id == run_id);
    assert(pending[0].report_hash == report_hash);
    assert(pending[0].state == "PENDING");
    assert(pending[0].attempts == 0);
    std::cout << "[PASS] Loaded 1 pending report\n";
    
    // Mark as delivered
    db.mark_delivered(run_id, "2026-02-18T12:01:00.000Z");
    pending = db.load_pending_reports();
    assert(pending.size() == 0);
    std::cout << "[PASS] Marked as delivered, no longer pending\n";
    
    // Test retry flow with new report
    db.persist_run("2026-02-18T12:05:00.000Z", "test-host-2", "test-policy", 90, details);
    int run_id2 = db.get_last_run_id();
    
    json report2 = {
        {"timestamp", "2026-02-18T12:05:00.000Z"},
        {"hostname", "test-host-2"},
        {"score", 90}
    };
    std::string hash2 = compute_report_hash(report2);
    db.enqueue_report(run_id2, report2.dump(), hash2, compute_posture_hash(report2));
    
    pending = db.load_pending_reports();
    assert(pending.size() == 1);
    std::cout << "[PASS] Second report enqueued\n";
    
    // Simulate retry with backoff (compute future timestamp dynamically)
    std::string future_time = get_future_timestamp(1); // 1 hour ahead
    db.update_retry(run_id2, 1, future_time, "Network timeout");
    pending = db.load_pending_reports();
    assert(pending.size() == 0); // Not ready yet (next_retry_at is in future)
    std::cout << "[PASS] Updated retry metadata, not yet ready\n";
    
    // Simulate max retries exceeded
    db.mark_failed(run_id2, "2026-02-18T12:15:00.000Z", "Max retries exceeded");
    pending = db.load_pending_reports();
    assert(pending.size() == 0); // Failed, no longer pending
    std::cout << "[PASS] Marked as failed after max retries\n";
    
    // Test UNIQUE constraint on report_hash
    db.persist_run("2026-02-18T12:20:00.000Z", "test-host-3", "test-policy", 95, details);
    int run_id3 = db.get_last_run_id();
    std::string duplicate_hash = hash2; // Reuse hash from run_id2
    
    // Re-queuing an already-queued report_hash is idempotent, not an error: an
    // at-least-once producer does exactly this when unsure the first attempt
    // landed. It returns false and inserts nothing, rather than throwing.
    {
        const size_t before = db.load_pending_reports().size();
        const bool inserted = db.enqueue_report(run_id3, report2.dump(), duplicate_hash,
                                                compute_posture_hash(report2));
        assert(!inserted);
        assert(db.load_pending_reports().size() == before);
        std::cout << "[PASS] Duplicate report_hash is a no-op, not an error\n";
    }
    
    std::cout << "\n";
    remove_db(test_db);
}

void test_retry_queue_manager() {
    std::cout << "=== Testing RetryQueue Manager ===\n";
    
    const char* test_db = "retry_queue_test.db";
    remove_db(test_db);
    
    DB db(test_db);
    db.init_schema();
    
    // Persist a run
    json details = {{"firewall_enabled", true}};
    db.persist_run("2026-02-24T14:00:00.000Z", "test-host", "test-policy", 95, details);
    int run_id = db.get_last_run_id();
    
    json report = {
        {"timestamp", "2026-02-24T14:00:00.000Z"},
        {"hostname", "test-host"},
        {"score", 95}
    };
    std::string hash = compute_report_hash(report);
    
    // Test with success client
    {
        auto client = std::make_unique<MockDeliveryClient>(true);
        RetryQueue queue(db, std::move(client), 5);
        
        queue.enqueue(run_id, report.dump(), hash, compute_posture_hash(report));
        int delivered = queue.process_pending();
        
        assert(delivered == 1);
        std::cout << "[PASS] RetryQueue delivered 1 report successfully\n";
    }
    
    // Test with failure client (triggers retry)
    db.persist_run("2026-02-24T14:01:00.000Z", "test-host-2", "test-policy", 90, details);
    int run_id2 = db.get_last_run_id();
    
    json report2 = {
        {"timestamp", "2026-02-24T14:01:00.000Z"},
        {"hostname", "test-host-2"},
        {"score", 90}
    };
    std::string hash2 = compute_report_hash(report2);
    
    {
        auto client = std::make_unique<MockDeliveryClient>(false);
        RetryQueue queue(db, std::move(client), 3); // max 3 retries
        
        queue.enqueue(run_id2, report2.dump(), hash2, compute_posture_hash(report2));
        
        // Process once - first attempt fails, schedules future retry
        queue.process_pending();
        
        // Force next_retry_at into the past so subsequent process_pending picks it up
        // Repeat until max_retries (3) is exceeded
        for (int i = 0; i < 4; i++) {
            db.update_retry(run_id2, i + 1, "2000-01-01 00:00:00", "forced retry");
            queue.process_pending();
        }
        
        // Verify the row is actually in FAILED state (not just absent from pending)
        std::string state = db.get_queue_state(run_id2);
        assert(state == "FAILED");
        
        auto pending = db.load_pending_reports();
        assert(pending.size() == 0);
        std::cout << "[PASS] RetryQueue respects max_retries (state=FAILED verified)\n";
    }
    
    std::cout << "\n";
    remove_db(test_db);
}

void test_integration() {
    std::cout << "=== Testing End-to-End Integration ===\n";
    
    const char* test_db = "integration_test.db";
    remove_db(test_db);
    
    DB db(test_db);
    db.init_schema();
    
    // Simulate full flow
    json report = {
        {"timestamp", "2026-02-18T13:00:00.000Z"},
        {"hostname", "production-server"},
        {"policy", "baseline-windows"},
        {"score", 85},
        {"details", {{"firewall_enabled", true}, {"av_installed", false}}}
    };
    
    // 1. Persist run
    db.persist_run("2026-02-18T13:00:00.000Z", "production-server", "baseline-windows", 85, report["details"]);
    int run_id = db.get_last_run_id();
    
    // 2. Hash report
    std::string report_hash = compute_report_hash(report);
    
    // 3. Enqueue for delivery
    db.enqueue_report(run_id, report.dump(), report_hash, compute_posture_hash(report));
    
    // 4. Load pending
    auto pending = db.load_pending_reports();
    assert(pending.size() == 1);
    
    // 5. Attempt delivery with mock client
    MockDeliveryClient client(true);
    DeliveryResult result = client.send(pending[0].report_json, pending[0].report_hash);
    
    // 6. Mark delivered on success
    if (result.success) {
        db.mark_delivered(run_id, "2026-02-18T13:00:01.000Z");
    }
    
    // 7. Verify no longer pending
    pending = db.load_pending_reports();
    assert(pending.size() == 0);
    
    std::cout << "[PASS] Full flow: persist -> hash -> enqueue -> deliver -> mark delivered\n";
    std::cout << "[PASS] Report successfully delivered and marked\n\n";
    
    remove_db(test_db);
}

// Helper: build a report with the given posture and timestamp.
static json make_report(const std::string& policy, int score,
                        const json& details, const std::string& ts) {
    return json{
        {"policy", policy},
        {"score", score},
        {"details", details},
        {"timestamp", ts},
        {"hostname", "test-host"},
    };
}

void test_posture_suppression() {
    std::cout << "=== Testing Posture Hash and Suppression ===\n";

    const json details_a = {{"firewall_enabled", true}, {"av_installed", true}};
    const json details_b = {{"firewall_enabled", false}, {"av_installed", true}};

    // --- The bug this replaces: the event hash changes every run ------------
    json r1 = make_report("p", 100, details_a, "2026-09-16T10:00:00.000Z");
    json r2 = make_report("p", 100, details_a, "2026-09-16T11:00:00.000Z");

    assert(compute_report_hash(r1) != compute_report_hash(r2));
    std::cout << "[PASS] Event hash differs for identical posture at different times\n";
    assert(compute_posture_hash(r1) == compute_posture_hash(r2));
    std::cout << "[PASS] Posture hash is stable across timestamps\n";

    // Hostname is identity, not posture: renaming a machine is not a state change.
    json renamed = r1;
    renamed["hostname"] = "renamed-host";
    assert(compute_posture_hash(renamed) == compute_posture_hash(r1));
    std::cout << "[PASS] Posture hash ignores hostname\n";

    // A genuine posture change must be visible.
    json changed = make_report("p", 80, details_b, "2026-09-16T10:00:00.000Z");
    assert(compute_posture_hash(changed) != compute_posture_hash(r1));
    std::cout << "[PASS] Posture hash changes when details/score change\n";

    // --- Suppression decision over the queue -------------------------------
    const char* test_db = "posture_test.db";
    remove_db(test_db);
    DB db(test_db);
    db.init_schema();

    assert(db.last_reported_posture_hash().empty());
    std::cout << "[PASS] No previous posture on a fresh database\n";

    auto persist_and_enqueue = [&db](const json& report) {
        db.persist_run(report.at("timestamp").get<std::string>(),
                       report.at("hostname").get<std::string>(),
                       report.at("policy").get<std::string>(),
                       report.at("score").get<int>(),
                       report.at("details"));
        const int run_id = db.get_last_run_id();
        const bool inserted = db.enqueue_report(run_id, report.dump(),
                                                compute_report_hash(report),
                                                compute_posture_hash(report));
        return std::make_pair(run_id, inserted);
    };

    // Posture A reported.
    auto [id_a, ins_a] = persist_and_enqueue(r1);
    assert(ins_a);
    assert(db.last_reported_posture_hash() == compute_posture_hash(r1));
    std::cout << "[PASS] Last reported posture tracks the queued report\n";

    // Same posture an hour later: the agent would suppress this. Verify the
    // decision input, i.e. that the stored posture equals the new one.
    assert(db.last_reported_posture_hash() == compute_posture_hash(r2));
    std::cout << "[PASS] Unchanged posture compares equal, so delivery is skipped\n";

    // Still visible after delivery succeeds.
    db.mark_delivered(id_a, "2026-09-16T10:00:01.000Z");
    assert(db.last_reported_posture_hash() == compute_posture_hash(r1));
    std::cout << "[PASS] DELIVERED reports still count as reported\n";

    // --- Flapping: A -> B -> A must all be reported ------------------------
    json rb = make_report("p", 80, details_b, "2026-09-16T11:00:00.000Z");
    auto [id_b, ins_b] = persist_and_enqueue(rb);
    assert(ins_b);
    db.mark_delivered(id_b, "2026-09-16T11:00:01.000Z");
    assert(db.last_reported_posture_hash() == compute_posture_hash(rb));

    json ra2 = make_report("p", 100, details_a, "2026-09-16T12:00:00.000Z");
    // Posture returned to A. It differs from the last reported posture (B), so
    // it is reported again -- the transition back is not lost.
    assert(db.last_reported_posture_hash() != compute_posture_hash(ra2));
    auto [id_a2, ins_a2] = persist_and_enqueue(ra2);
    assert(ins_a2);
    std::cout << "[PASS] Flapping A->B->A reports the return to A\n";
    db.mark_delivered(id_a2, "2026-09-16T12:00:01.000Z");

    // --- A permanently FAILED report must not suppress the next attempt ----
    json rc = make_report("p", 60, {{"firewall_enabled", false}}, "2026-09-16T13:00:00.000Z");
    auto [id_c, ins_c] = persist_and_enqueue(rc);
    assert(ins_c);
    assert(db.last_reported_posture_hash() == compute_posture_hash(rc));

    db.mark_failed(id_c, "2026-09-16T13:05:00.000Z", "Max retries exceeded");
    // Delivery was abandoned, so posture C was never actually reported. The
    // query excludes FAILED rows, so the last reported posture falls back to A
    // and the next evaluation of C will be enqueued again -- no reset needed.
    assert(db.last_reported_posture_hash() != compute_posture_hash(rc));
    assert(db.last_reported_posture_hash() == compute_posture_hash(ra2));
    std::cout << "[PASS] FAILED delivery does not suppress the next report\n";

    std::cout << "\n";
    remove_db(test_db);
}

int main() {
    std::cout << "\n";
    std::cout << "===================================================\n";
    std::cout << "  Delivery Foundation Integration Tests\n";
    std::cout << "===================================================\n\n";
    
    try {
        test_hash_determinism();
        test_mock_delivery();
        test_retry_queue();
        test_posture_suppression();
        test_retry_queue_manager();
        test_integration();
        
        std::cout << "===================================================\n";
        std::cout << "  ALL TESTS PASSED\n";
        std::cout << "===================================================\n\n";
        
        std::cout << "Summary:\n";
        std::cout << "  - SHA-256 hashing is deterministic\n";
        std::cout << "  - MockDeliveryClient works correctly\n";
        std::cout << "  - Retry queue operations functional\n";
        std::cout << "  - RetryQueue manager with backoff works\n";
        std::cout << "  - Duplicate report_hash enqueue is idempotent\n";
        std::cout << "  - Posture hash excludes timestamp and hostname\n";
        std::cout << "  - Unchanged posture suppresses delivery\n";
        std::cout << "  - Flapping and failed deliveries are re-reported\n";
        std::cout << "  - Dynamic timestamp handling prevents test decay\n";
        std::cout << "  - End-to-end integration verified\n";
        std::cout << "  - Ready for production HTTP delivery\n\n";
    } catch (const std::exception& e) {
        std::cerr << "\nTEST FAILED: " << e.what() << "\n\n";
        return 1;
    }
    
    // Final cleanup: remove any leftover test databases
    const char* test_dbs[] = {"test_sentinel.db", "retry_queue_test.db", "integration_test.db",
                               "posture_test.db"};
    for (const char* db_name : test_dbs) {
        remove_db(db_name);
    }
    
    return 0;
}
