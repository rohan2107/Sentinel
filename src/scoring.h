#pragma once
#include <nlohmann/json.hpp>

int compute_score(const nlohmann::json& policy, const nlohmann::json& outcomes);
