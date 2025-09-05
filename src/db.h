#pragma once
#include <string>
#include <nlohmann/json.hpp>

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

    // convenience: export runs to JSON array
    nlohmann::json all_runs_json();

private:
    struct Impl;
    Impl* p;
};
