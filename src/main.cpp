// src/main.cpp
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

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

// Return current time in ISO-8601 UTC with milliseconds
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

// Get system hostname (best-effort)
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
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--policy" && i + 1 < argc) {
            opts.policy_path = argv[++i];
        } else if (a == "--report-path" && i + 1 < argc) {
            opts.report_path = argv[++i];
        } else {
            spdlog::warn("Unknown argument: {}", a);
        }
    }
    return opts;
}

int main(int argc, char** argv) {
    auto opts = parse_cli(argc, argv);

    spdlog::info("SentinelAgent starting");
    spdlog::info("Policy: {}", opts.policy_path);

    // Load policy
    std::ifstream ifs(opts.policy_path);
    if (!ifs.is_open()) {
        spdlog::error("Cannot open policy: {}", opts.policy_path);
        std::cerr << "Cannot open policy: " << opts.policy_path << std::endl;
        return 1;
    }

    json policy;
    try {
        ifs >> policy;
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse policy JSON: {}", e.what());
        return 1;
    }

    // Evaluate rules
    json outcomes = json::object();
    try {
        for (const auto& rule : policy.at("rules")) {
            const std::string id = rule.at("id").get<std::string>();
            const std::string query = rule.at("query").get<std::string>();
            const std::string luacode = rule.at("lua").get<std::string>();

            spdlog::info("Running osquery for rule {}: {}", id, query);
            json results = run_osquery_json(query);

            bool pass = eval_lua_against_json(luacode, results);
            outcomes[id] = pass;

            spdlog::info("{} -> {}", id, (pass ? "PASS" : "FAIL"));
            std::cout << id << ": " << (pass ? "PASS" : "FAIL") << std::endl;
        }
    } catch (const std::exception& e) {
        spdlog::error("Error while evaluating rules: {}", e.what());
        return 1;
    }

    // Compute score and build report
    int score = compute_score(policy, outcomes);

    json report = {
        {"policy", policy.value("policy_name", std::string("unnamed"))},
        {"score", score},
        {"details", outcomes},
        {"timestamp", iso8601_utc_now()},
        {"hostname", get_hostname()}
    };

    // Print report to stdout (pretty)
    std::cout << report.dump(2) << std::endl;

    // Persist report to file if requested
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

        // Create a flat details object from outcomes (keeps booleans as-is)
        json details_flat = json::object();
        for (auto it = outcomes.begin(); it != outcomes.end(); ++it) {
            details_flat[it.key()] = it.value();
        }

        std::string ts = report.value("timestamp", std::string(""));
        std::string host = report.value("hostname", std::string("unknown-host"));
        std::string policy_name = report.value("policy", std::string(""));

        db.persist_run(ts, host, policy_name, report.value("score", 0), details_flat);
        spdlog::info("Persisted run to sentinel_data.sqlite3");
    } catch (const std::exception& e) {
        spdlog::error("Failed to persist run to DB: {}", e.what());
    }

    spdlog::info("SentinelAgent finished, score={}", score);
    return 0;
}
