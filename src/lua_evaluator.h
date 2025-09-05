#pragma once
#include <string>
#include <nlohmann/json.hpp>

bool eval_lua_against_json(const std::string& luaCode, const nlohmann::json& resultsJson);
