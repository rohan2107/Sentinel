#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct QueuedReport {
    int run_id;
    std::string report_hash;   // event hash: whole report, timestamp included
    std::string posture_hash;  // posture only: policy/score/details
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
    
    // Enqueue a report for delivery.
    //
    // Idempotent: returns true if a new row was inserted, false if this
    // report_hash was already queued. A duplicate is an expected, benign
    // condition in an at-least-once system, not an error, so it does not throw.
    bool enqueue_report(int run_id,
                        const std::string& report_json,
                        const std::string& report_hash,
                        const std::string& posture_hash);

    // Posture hash of the most recently reported run, or "" if nothing has been
    // reported yet.
    //
    // FAILED entries are excluded deliberately. A report whose delivery was
    // permanently abandoned was never actually reported, so it must not suppress
    // the next attempt: excluding it here means the next evaluation re-reports
    // that posture automatically, with no reset bookkeeping anywhere.
    std::string last_reported_posture_hash();

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
