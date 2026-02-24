#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct QueuedReport {
    int run_id;
    std::string report_hash;
    std::string report_json;
    int attempts;
    std::string state;  // PENDING, DELIVERED, FAILED
    std::string next_retry_at;  // ISO-8601 timestamp, empty for immediate
    std::string created_at;
    std::string last_error;
};

struct DB {
    DB(const std::string& path);
    ~DB();

    // create schema if not exists
    void init_schema();

    // persist a run: store timestamp (ISO), hostname, policy, score, details (json)
    void persist_run(const std::string& iso_ts,
                     const std::string& hostname,
                     const std::string& policy,
                     int score,
                     const nlohmann::json& details);

    // get last inserted run_id (call after persist_run)
    int get_last_run_id();

    // convenience: export runs to JSON array
    nlohmann::json all_runs_json();

    // --- Retry Queue Operations ---
    
    // enqueue a report for delivery
    void enqueue_report(int run_id, const std::string& report_json, const std::string& report_hash);

    // load all pending reports (PENDING state, next_retry_at <= now or empty)
    std::vector<QueuedReport> load_pending_reports();

    // mark report as delivered
    void mark_delivered(int run_id, const std::string& delivered_at);

    // mark report as failed (max retries exceeded)
    void mark_failed(int run_id, const std::string& failed_at, const std::string& last_error);

    // increment retry attempt and set next_retry_at
    void update_retry(int run_id, int attempts, const std::string& next_retry_at, const std::string& error);

    // query a queue entry's current state (for testing/diagnostics)
    std::string get_queue_state(int run_id);

private:
    struct Impl;
    Impl* p;
};
