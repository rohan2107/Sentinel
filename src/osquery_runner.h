// src/osquery_runner.h
#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Run osqueryi with the given SQL query and return parsed JSON.
// Throws std::runtime_error on failure.
nlohmann::json run_osquery_json(const std::string& sql_query);
