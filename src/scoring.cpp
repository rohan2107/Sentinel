#include "scoring.h"
using json = nlohmann::json;

int compute_score(const json& policy, const json& outcomes) {
    int base = policy.value("base_score", 100);
    int score = base;
    for (const auto& r : policy.at("rules")) {
        const std::string id = r.at("id").get<std::string>();
        int weight = r.value("weight", 0);
        bool pass = outcomes.value(id, false);
        if (!pass) score -= weight;
    }
    return std::max(0, std::min(100, score));
}
