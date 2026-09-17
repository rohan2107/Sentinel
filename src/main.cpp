// src/main.cpp
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "osquery_runner.h"
#include "lua_evaluator.h"
#include "db.h"
#include "scoring.h"
#include "report_hasher.h"
#include "http_delivery_client.h"
#include "retry_queue.h"

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <unistd.h>
#endif

using json = nlohmann::json;

static bool validate_policy(const json& policy, std::vector<std::string>& errors) {
    if (!policy.is_object()) {
        errors.push_back("Policy root must be an object");
        return false;
    }
    if (!policy.contains("rules") || !policy["rules"].is_array()) {
        errors.push_back("Policy missing 'rules' array");
        return false;
    }
    std::unordered_set<std::string> ids;
    for (const auto& rule : policy["rules"]) {
        if (!rule.is_object()) {
            errors.push_back("Rule is not an object");
            continue;
        }
        if (!rule.contains("id") || !rule["id"].is_string()) {
            errors.push_back("Rule missing string 'id'");
            continue;
        }
        const std::string id = rule["id"].get<std::string>();
        if (id.empty()) errors.push_back("Rule id is empty");
        if (ids.count(id)) errors.push_back("Duplicate rule id: " + id);
        ids.insert(id);

        if (!rule.contains("query") || !rule["query"].is_string()) {
            errors.push_back("Rule " + id + " missing string 'query'");
        } else if (rule["query"].get<std::string>().size() > 4096) {
            errors.push_back("Rule " + id + " query too long (>4096 chars)");
        }

        if (!rule.contains("lua") || !rule["lua"].is_string()) {
            errors.push_back("Rule " + id + " missing string 'lua'");
        }

        if (rule.contains("weight") && !rule["weight"].is_number_integer()) {
            errors.push_back("Rule " + id + " weight must be integer");
        } else if (rule.contains("weight")) {
            int w = rule["weight"].get<int>();
            if (w < 0 || w > 100) {
                errors.push_back("Rule " + id + " weight out of range 0-100");
            }
        }
    }
    return errors.empty();
}

// Return current UTC time in ISO-8601 with milliseconds, e.g. 2025-09-20T12:34:56.789Z
static std::string iso8601_utc_now() {
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

// Best-effort hostname
static std::string get_hostname() {
#ifdef _WIN32
    CHAR buffer[256];
    DWORD size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) return std::string(buffer, buffer + size);
    return "unknown-host";
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) return std::string(buf);
    return "unknown-host";
#endif
}

struct CliOptions {
    std::string policy_path = "policies/sample_policy.json";
    std::string report_path = "reports/latest_report.json";
    std::string backend_url = "http://localhost:8000";
    bool enable_delivery = false;
};

static CliOptions parse_cli(int argc, char** argv) {
    CliOptions opts;
    std::string positional_arg;
    
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--policy" && i + 1 < argc) {
            opts.policy_path = argv[++i];
        } else if (a == "--report-path" && i + 1 < argc) {
            opts.report_path = argv[++i];
        } else if (a == "--backend-url" && i + 1 < argc) {
            opts.backend_url = argv[++i];
        } else if (a == "--enable-delivery") {
            opts.enable_delivery = true;
        } else if (!a.empty() && a[0] != '-') {
            // Positional argument (not a flag)
            if (positional_arg.empty()) {
                positional_arg = a;
            } else {
                spdlog::warn("Multiple positional arguments, ignoring: {}", a);
            }
        } else {
            spdlog::warn("Unknown argument: {}", a);
        }
    }
    
    // If positional arg provided and --policy was not set explicitly, use positional
    if (!positional_arg.empty() && opts.policy_path == "policies/sample_policy.json") {
        opts.policy_path = positional_arg;
    }
    
    return opts;
}

int main(int argc, char** argv) {
    auto opts = parse_cli(argc, argv);

    spdlog::info("Sentinel starting");
    spdlog::info("Policy: {}", opts.policy_path);
    
    // Crash recovery: retry pending deliveries from previous runs
    if (opts.enable_delivery) {
        try {
            DB db("sentinel_data.sqlite3");
            db.init_schema();
            
            auto pending = db.load_pending_reports();
            if (!pending.empty()) {
                spdlog::info("Found {} pending report(s) from previous run(s)", pending.size());
                
                auto client = std::make_unique<HttpDeliveryClient>(opts.backend_url, 30);
                RetryQueue queue(db, std::move(client), 10);
                
                int delivered = queue.process_pending();
                spdlog::info("Crash recovery: delivered {}/{} pending reports", delivered, pending.size());
            }
        } catch (const std::exception& e) {
            spdlog::error("Crash recovery failed: {}", e.what());
        }
    }

    // Load policy JSON
    json policy;
    {
        std::ifstream ifs(opts.policy_path);
        if (!ifs.is_open()) {
            spdlog::error("Cannot open policy: {}", opts.policy_path);
            std::cerr << "Cannot open policy: " << opts.policy_path << std::endl;
            return 1;
        }

        try {
            ifs >> policy;
        } catch (const std::exception& e) {
            spdlog::error("Failed to parse policy JSON: {}", e.what());
            std::cerr << "Failed to parse policy JSON: " << e.what() << std::endl;
            return 1;
        }
    }

    // Basic policy validation
    std::vector<std::string> validation_errors;
    if (!validate_policy(policy, validation_errors)) {
        for (const auto& e : validation_errors) {
            spdlog::error("Policy validation error: {}", e);
            std::cerr << "Policy validation error: " << e << std::endl;
        }
        return 1;
    }

    json outcomes = json::object();

    // Flat {rule_id, passed, weight} triples for persistence, one per rule
    // actually evaluated -- not a fixed set of named fields, so db.cpp needs
    // no schema change when any policy on any platform introduces a new rule
    // id. Weight is captured here, at evaluation time, because it is the
    // value that was actually applied to this run; the policy file it came
    // from may since have changed.
    json rule_results = json::array();

    // Evaluate each rule
    for (const auto& rule : policy["rules"]) {
        try {
            if (!rule.contains("id") || !rule.contains("query") || !rule.contains("lua")) {
                spdlog::warn("Skipping rule with missing fields: {}", rule.dump(0));
                continue;
            }

            const std::string id = rule.at("id").get<std::string>();
            const std::string query = rule.at("query").get<std::string>();
            const std::string luacode = rule.at("lua").get<std::string>();
            const int weight = rule.value("weight", 0);

            spdlog::info("Running osquery for rule {}: {}", id, query);
            // run_osquery_json throws on internal errors; returns JSON array on success
            json results;
            try {
                results = run_osquery_json(query);
            } catch (const std::exception& e) {
                spdlog::error("osquery error for rule {}: {}", id, e.what());
                outcomes[id] = false;
                rule_results.push_back({{"rule_id", id}, {"passed", false}, {"weight", weight}});
                continue; // move to next rule
            }

            bool pass = eval_lua_against_json(luacode, results);
            outcomes[id] = pass;
            rule_results.push_back({{"rule_id", id}, {"passed", pass}, {"weight", weight}});

            spdlog::info("{} -> {}", id, (pass ? "PASS" : "FAIL"));
        } catch (const std::exception& e) {
            spdlog::error("Exception while evaluating a rule: {}", e.what());
        } catch (...) {
            spdlog::error("Unknown exception while evaluating a rule");
        }
    }

    // Compute score and assemble report
    int score = 0;
    try {
        score = compute_score(policy, outcomes);
    } catch (const std::exception& e) {
        spdlog::error("Failed to compute score: {}", e.what());
        score = 0;
    }

    json report = {
        {"policy", policy.value("policy_name", std::string("unnamed"))},
        {"score", score},
        {"details", outcomes},
        {"timestamp", iso8601_utc_now()},
        {"hostname", get_hostname()}
    };

    // Print report to stdout
    std::cout << report.dump(2) << std::endl;

    // Persist report file (best-effort)
    try {
        // reports/ is gitignored, so it is absent on a fresh clone and ofstream
        // would fail to open the file. Create the parent directory first.
        const auto parent = std::filesystem::path(opts.report_path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                spdlog::warn("Could not create report directory {}: {}",
                             parent.string(), ec.message());
            }
        }

        std::ofstream ofs(opts.report_path);
        if (ofs) {
            ofs << report.dump(2);
            ofs.close();
            spdlog::info("Wrote report to {}", opts.report_path);
        } else {
            spdlog::warn("Could not open report file for writing: {}", opts.report_path);
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to write report file: {}", e.what());
    }

    // Persist to SQLite DB (best-effort)
    int run_id = -1;
    try {
        DB db("sentinel_data.sqlite3");
        db.init_schema();

        json details_flat = json::object();
        for (auto it = outcomes.begin(); it != outcomes.end(); ++it) {
            details_flat[it.key()] = it.value();
        }

        db.persist_run(report.value("timestamp", std::string("")),
                       report.value("hostname", std::string("unknown-host")),
                       report.value("policy", std::string("")),
                       report.value("score", 0),
                       details_flat,
                       rule_results);

        run_id = db.get_last_run_id();
        spdlog::info("Persisted run to sentinel_data.sqlite3 (run_id={})", run_id);
        
        // Delivery layer integration (if enabled)
        if (opts.enable_delivery && run_id > 0) {
            try {
                // Two distinct hashes, answering two distinct questions:
                //   event_hash   - "which report is this?" Covers the whole
                //                  report including timestamp. Goes on the wire;
                //                  receivers recompute it to verify.
                //   posture_hash - "has the state changed?" Covers policy, score
                //                  and details only. Agent-local, never sent.
                const std::string event_hash = compute_report_hash(report);
                const std::string posture_hash = compute_posture_hash(report);
                spdlog::info("Event hash:   {}...", event_hash.substr(0, 16));
                spdlog::info("Posture hash: {}...", posture_hash.substr(0, 16));

                auto client = std::make_unique<HttpDeliveryClient>(opts.backend_url, 30);
                RetryQueue queue(db, std::move(client), 10);

                // State-change-triggered reporting: send only when posture
                // differs from the last posture we committed to reporting.
                // Unconditional reporting sends an identical payload on every
                // evaluation, which is the bulk of the traffic on a fleet whose
                // posture is almost always static.
                const std::string last_posture = queue.last_reported_posture();
                if (!last_posture.empty() && last_posture == posture_hash) {
                    spdlog::info("Posture unchanged since last report "
                                 "(posture_hash={}...), skipping delivery",
                                 posture_hash.substr(0, 16));
                    // Still process the queue: an earlier report may be waiting
                    // on backoff, and suppressing a new one must not stall it.
                    int delivered = queue.process_pending();
                    if (delivered > 0) {
                        spdlog::info("Delivered {} backlogged report(s)", delivered);
                    }
                } else {
                    if (last_posture.empty()) {
                        spdlog::info("No previous report; reporting initial posture");
                    } else {
                        spdlog::info("Posture changed ({}... -> {}...), reporting",
                                     last_posture.substr(0, 16),
                                     posture_hash.substr(0, 16));
                    }

                    if (queue.enqueue(run_id, report.dump(), event_hash, posture_hash)) {
                        spdlog::info("Enqueued report for delivery");
                    } else {
                        spdlog::info("Report already queued (event_hash seen), not re-queued");
                    }

                    int delivered = queue.process_pending();
                    if (delivered > 0) {
                        spdlog::info("Successfully delivered {} report(s)", delivered);
                    } else {
                        spdlog::warn("Delivery deferred, will retry with backoff");
                    }
                }

            } catch (const std::exception& e) {
                spdlog::error("Delivery failed: {}", e.what());
            }
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to persist run to DB: {}", e.what());
    }

    spdlog::info("Sentinel finished, score={}", score);
    return 0;
}
