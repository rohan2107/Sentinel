// src/retry_queue.cpp
#include "retry_queue.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

RetryQueue::RetryQueue(DB& db, std::unique_ptr<DeliveryClient> client, int max_retries)
    : db_(db), client_(std::move(client)), max_retries_(max_retries) {
    // Seed random for jitter
    std::srand(static_cast<unsigned>(std::time(nullptr)));
}

void RetryQueue::enqueue(int run_id, const std::string& report_json, const std::string& report_hash) {
    db_.enqueue_report(run_id, report_json, report_hash);
}

std::vector<QueuedReport> RetryQueue::load_pending() {
    return db_.load_pending_reports();
}

std::string RetryQueue::iso8601_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    std::time_t t = system_clock::to_time_t(secs);
    auto ms = duration_cast<milliseconds>(now - secs).count();

    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << (ms % 1000) << 'Z';
    return oss.str();
}

std::string RetryQueue::compute_next_retry(int attempts) {
    // Exponential backoff: 1s, 2s, 4s, 8s, ... cap at 300s (5 minutes)
    int backoff_s = (std::min)(300, 1 << attempts);
    
    // Add jitter: ±25%
    int jitter_range = backoff_s / 4;
    int jitter_s = (std::rand() % (jitter_range * 2 + 1)) - jitter_range;
    
    int total_delay = (std::max)(1, backoff_s + jitter_s);
    
    // Compute future timestamp
    using namespace std::chrono;
    auto now = system_clock::now();
    auto future = now + seconds(total_delay);
    auto future_time_t = system_clock::to_time_t(future);
    
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &future_time_t);
#else
    gmtime_r(&tm, &future_time_t);
#endif
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

int RetryQueue::process_pending() {
    auto pending = db_.load_pending_reports();
    int delivered_count = 0;
    
    for (const auto& queued : pending) {
        // Attempt delivery
        DeliveryResult result = client_->send(queued.report_json, queued.report_hash);
        
        if (result.success) {
            // Mark as delivered
            db_.mark_delivered(queued.run_id, iso8601_now());
            delivered_count++;
        } else {
            // Check if max retries exceeded
            int new_attempts = queued.attempts + 1;
            
            if (new_attempts >= max_retries_) {
                // Terminal failure
                db_.mark_failed(queued.run_id, iso8601_now(), result.error_message);
            } else {
                // Schedule retry with backoff
                std::string next_retry = compute_next_retry(new_attempts);
                db_.update_retry(queued.run_id, new_attempts, next_retry, result.error_message);
            }
        }
    }
    
    return delivered_count;
}
