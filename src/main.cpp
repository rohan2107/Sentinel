// src/main.cpp
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

            spdlog::info("Running osquery for rule {}: {}", id, query);
            // run_osquery_json throws on internal errors; returns JSON array on success
            json results;
            try {
                results = run_osquery_json(query);
            } catch (const std::exception& e) {
                spdlog::error("osquery error for rule {}: {}", id, e.what());
                outcomes[id] = false;
                std::cout << id << ": FAIL" << std::endl;
                continue; // move to next rule
            }

            bool pass = eval_lua_against_json(luacode, results);
            outcomes[id] = pass;

            spdlog::info("{} -> {}", id, (pass ? "PASS" : "FAIL"));
            std::cout << id << ": " << (pass ? "PASS" : "FAIL") << std::endl;
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
                       details_flat);

        spdlog::info("Persisted run to sentinel_data.sqlite3");
    } catch (const std::exception& e) {
        spdlog::error("Failed to persist run to DB: {}", e.what());
    }

    spdlog::info("Sentinel finished, score={}", score);
    return 0;
}
