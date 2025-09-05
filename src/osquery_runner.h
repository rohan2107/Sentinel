#pragma once
#include <nlohmann/json.hpp>
#include <string>

nlohmann::json run_osquery_json(const std::string& sql, bool simulate);
