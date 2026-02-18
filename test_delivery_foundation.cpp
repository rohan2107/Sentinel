// Integration test for delivery foundation
#include <iostream>
#include <cassert>
#include <nlohmann/json.hpp>
#include "src/db.h"
#include "src/report_hasher.h"
#include "src/delivery_client.h"

using json = nlohmann::json;

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
    
    // Simulate retry with backoff
    db.update_retry(run_id2, 1, "2026-02-18T12:10:00.000Z", "Network timeout");
    pending = db.load_pending_reports();
    assert(pending.size() == 0); // Not ready yet (next_retry_at is in future)
    std::cout << "[PASS] Updated retry metadata, not yet ready\n";
    
    // Simulate max retries exceeded
    db.mark_failed(run_id2, "2026-02-18T12:15:00.000Z", "Max retries exceeded");
    pending = db.load_pending_reports();
    assert(pending.size() == 0); // Failed, no longer pending
    std::cout << "[PASS] Marked as failed after max retries\n";
    
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
        test_integration();
        
        std::cout << "===================================================\n";
        std::cout << "  ALL TESTS PASSED\n";
        std::cout << "===================================================\n\n";
        
        std::cout << "Summary:\n";
        std::cout << "  - SHA-256 hashing is deterministic\n";
        std::cout << "  - MockDeliveryClient works correctly\n";
        std::cout << "  - Retry queue operations functional\n";
        std::cout << "  - End-to-end integration verified\n";
        std::cout << "  - Ready for HTTP/MQTT delivery clients\n\n";
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTEST FAILED: " << e.what() << "\n\n";
        return 1;
    }
}
