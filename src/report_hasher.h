#pragma once
#include <string>
#include <nlohmann/json.hpp>

// Compute SHA-256 hash of canonical JSON report.
// Canonicalization: sorted keys, compact format (no whitespace).
//
// This is the *event* hash: it covers the whole report, timestamp included, so
// it identifies one specific report. It is what goes on the wire and what the
// receivers recompute to verify, so its input must stay the full report.
std::string compute_report_hash(const nlohmann::json& report);

// Compute SHA-256 over the posture-bearing fields only: policy, score, details.
//
// Deliberately excludes:
//   timestamp - fresh on every run, so including it makes every hash unique and
//               defeats the "has anything changed?" question entirely
//   hostname  - identity, not posture; it is constant for an agent, and a
//               machine rename would otherwise read as a posture change
//
// Agent-local: used only to decide whether to report at all. It is never sent,
// so this does not affect the wire format.
std::string compute_posture_hash(const nlohmann::json& report);

// Canonicalize JSON to deterministic string representation
std::string canonicalize_json(const nlohmann::json& j);
