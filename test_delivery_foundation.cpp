// Integration test for delivery foundation
#include <iostream>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include "src/db.h"
#include "src/report_hasher.h"
#include "src/delivery_client.h"
#include "src/retry_queue.h"

using json = nlohmann::json;

// Helper function to compute future timestamp in ISO8601 format
std::string get_future_timestamp(int hours_ahead) {
    auto now = std::chrono::system_clock::now();
    auto future = now + std::chrono::hours(hours_ahead);
    auto future_time_t = std::chrono::system_clock::to_time_t(future);
    
    std::tm tm_utc;
    gmtime_s(&tm_utc, &future_time_t);  // Windows-safe version
    
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
    remove(test_db);
    
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
    db.enqueue_report(run_id, report_json, report_hash);
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
    db.enqueue_report(run_id2, report2.dump(), hash2);
    
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
    
    try {
        db.enqueue_report(run_id3, report2.dump(), duplicate_hash);
        assert(false); // Should not reach here
    } catch (const std::runtime_error& e) {
        // Expected: UNIQUE constraint violation
        std::cout << "[PASS] Duplicate report_hash correctly rejected\n";
    }
    
    std::cout << "\n";
    remove(test_db);
}

void test_retry_queue_manager() {
    std::cout << "=== Testing RetryQueue Manager ===\n";
    
    const char* test_db = "retry_queue_test.db";
    remove(test_db);
    
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
        
        queue.enqueue(run_id, report.dump(), hash);
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
        
        queue.enqueue(run_id2, report2.dump(), hash2);
        
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
    remove(test_db);
}

void test_integration() {
    std::cout << "=== Testing End-to-End Integration ===\n";
    
    const char* test_db = "integration_test.db";
    remove(test_db);
    
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
    db.enqueue_report(run_id, report.dump(), report_hash);
    
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
    
    remove(test_db);
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
        std::cout << "  - UNIQUE constraint on report_hash enforced\n";
        std::cout << "  - Dynamic timestamp handling prevents test decay\n";
        std::cout << "  - End-to-end integration verified\n";
        std::cout << "  - Ready for production HTTP delivery\n\n";
    } catch (const std::exception& e) {
        std::cerr << "\nTEST FAILED: " << e.what() << "\n\n";
        return 1;
    }
    
    // Final cleanup: remove any leftover test databases
    const char* test_dbs[] = {"test_sentinel.db", "retry_queue_test.db", "integration_test.db"};
    for (const char* db_name : test_dbs) {
        remove(db_name);
    }
    
    return 0;
}
