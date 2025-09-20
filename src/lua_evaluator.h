// src/lua_evaluator.h
#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Evaluate the provided Lua code against osquery JSON results.
// - luaCode: Lua source provided in the policy (should return true/false or set global `pass`).
// - resultsJson: osquery results as nlohmann::json (expected to be an array of row objects).
//
// Returns true if policy passes, false otherwise. Any runtime error in Lua also returns false.
bool eval_lua_against_json(const std::string& luaCode, const nlohmann::json& resultsJson);
