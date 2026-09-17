// src/retry_queue.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "db.h"
#include "delivery_client.h"

// Retry queue manager with exponential backoff
// Coordinates between DB persistence and delivery client
class RetryQueue {
public:
    RetryQueue(DB& db, std::unique_ptr<DeliveryClient> client, int max_retries = 10);
    
    // Enqueue new report for delivery (persists to DB).
    // Returns false if this report_hash was already queued; see
    // DB::enqueue_report for why a duplicate is not an error.
    bool enqueue(int run_id,
                 const std::string& report_json,
                 const std::string& report_hash,
                 const std::string& posture_hash);

    // Posture hash of the most recently reported run, or "" if none.
    std::string last_reported_posture();
    
    // Process all pending reports ready for delivery
    // Returns number of reports successfully delivered
    int process_pending();
    
    // Load pending reports on startup (crash recovery)
    std::vector<QueuedReport> load_pending();
    
private:
    DB& db_;
    std::unique_ptr<DeliveryClient> client_;
    int max_retries_;
    
    // Compute next retry time with exponential backoff + jitter
    // Returns ISO-8601 timestamp string
    std::string compute_next_retry(int attempts);
    
    // Helper to get current ISO-8601 timestamp
    std::string iso8601_now();
};
