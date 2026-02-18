#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Compute SHA-256 hash of canonical JSON report
// Canonicalization: sorted keys, compact format (no whitespace)
std::string compute_report_hash(const nlohmann::json& report);

// Canonicalize JSON to deterministic string representation
std::string canonicalize_json(const nlohmann::json& j);
