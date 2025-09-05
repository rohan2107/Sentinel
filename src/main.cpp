#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "osquery_runner.h"
#include "lua_evaluator.h"
#include "scoring.h"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

int main(int argc, char** argv) {
    std::string policyPath = "policies/sample_policy.json";
    bool simulate = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--policy" && i + 1 < argc) policyPath = argv[++i];
        else if (a == "--simulate") simulate = true;
        else if (a == "--no-simulate") simulate = false;
    }

    std::ifstream in(policyPath);
    if (!in) {
        spdlog::error("Cannot open policy: {}", policyPath);
        return 1;
    }
    json policy; in >> policy;

    json outcomes = json::object();

    for (const auto& rule : policy.at("rules")) {
        const std::string id = rule.at("id").get<std::string>();
        const std::string query = rule.at("query").get<std::string>();
        const std::string luacode = rule.at("lua").get<std::string>();

        json results = run_osquery_json(query, simulate);
        bool pass = eval_lua_against_json(luacode, results);
        outcomes[id] = pass;
        spdlog::info("{} -> {}", id, (pass ? "PASS" : "FAIL"));
    }

    int score = compute_score(policy, outcomes);
    json report{{"policy", policy["policy_name"]}, {"score", score}, {"details", outcomes}};

    std::cout << report.dump(2) << std::endl;
    return 0;
}
