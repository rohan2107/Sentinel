// src/lua_evaluator.cpp
#include "lua_evaluator.h"

#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

// Recursively copy JSON into a sol::table (for objects/arrays) or push primitive values.
// - For arrays, uses 1-based indices (Lua style).
// - For objects, uses string keys.
static sol::object json_to_sol(const json& j, sol::state_view lua) {
    if (j.is_null()) {
        return sol::make_object(lua, sol::nil);
    } else if (j.is_boolean()) {
        return sol::make_object(lua, j.get<bool>());
    } else if (j.is_number_integer()) {
        // use 64-bit integer; sol will convert to Lua number or integer as available
        return sol::make_object(lua, j.get<long long>());
    } else if (j.is_number_unsigned()) {
        return sol::make_object(lua, j.get<unsigned long long>());
    } else if (j.is_number_float()) {
        return sol::make_object(lua, j.get<double>());
    } else if (j.is_string()) {
        return sol::make_object(lua, j.get<std::string>());
    } else if (j.is_array()) {
        sol::table t = lua.create_table(static_cast<int>(j.size()), 0);
        int idx = 1;
        for (const auto& el : j) {
            t[idx++] = json_to_sol(el, lua);
        }
        return sol::make_object(lua, t);
    } else if (j.is_object()) {
        sol::table t = lua.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string key = it.key();
            t.set(key, json_to_sol(it.value(), lua));
        }
        return sol::make_object(lua, t);
    }

    // fallback
    return sol::make_object(lua, sol::nil);
}

bool eval_lua_against_json(const std::string& luaCode, const json& resultsJson) {
    try {
        sol::state lua;
        // open a small, safe-ish set of libraries; add more if you need them
        lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);

        sol::state_view sv(lua);

        // Provide both `results` and `rows` globals for compatibility.
        // Expectation: resultsJson is an array of row objects (each row is an object mapping column->value).
        sol::object rows_obj = json_to_sol(resultsJson, sv);
        lua["results"] = rows_obj;
        lua["rows"] = rows_obj;

        // Wrap user code in a function to capture returns and avoid polluting globals accidentally.
        // We name it __USER_EVAL__ to keep it deterministic.
        std::string wrapped = "function __USER_EVAL__()\n" + luaCode + "\nend";

        sol::protected_function_result load_res = lua.safe_script(wrapped, &sol::script_pass_on_error);
        if (!load_res.valid()) {
            sol::error err = load_res;
            spdlog::error("Lua load error: {}", err.what());
            return false;
        }

        sol::protected_function user_fn = lua["__USER_EVAL__"];
        if (!user_fn.valid()) {
            spdlog::error("Lua function __USER_EVAL__ not found after loading code.");
            return false;
        }

        sol::protected_function_result call_res = user_fn();
        if (!call_res.valid()) {
            sol::error err = call_res;
            spdlog::error("Lua runtime error: {}", err.what());
            return false;
        }

        // If the function returned a boolean, prefer that.
        if (call_res.return_count() >= 1) {
            // try to get boolean from the last return value
            sol::optional<bool> b = call_res;
            if (b.has_value()) {
                return b.value();
            }

            // If returned non-boolean, try to interpret truthiness: nil -> false, else true
            // (sol doesn't provide direct "truthiness" query so examine top of stack)
            sol::object ret = call_res.get<sol::object>();
            if (ret.is<sol::nil_t>()) return false;
            // any other non-false value -> true
            if (ret.is<bool>()) return ret.as<bool>();
            return true;
        }

        // No return values: fallback to checking global `pass`
        sol::object gpass = lua["pass"];
        if (gpass.is<bool>()) {
            return gpass.as<bool>();
        }

        // Also allow number 0/1 style
        if (gpass.is<int>() || gpass.is<long long>() || gpass.is<unsigned long long>()) {
            long long v = 0;
            if (gpass.is<int>()) v = gpass.as<int>();
            else if (gpass.is<long long>()) v = gpass.as<long long>();
            else if (gpass.is<unsigned long long>()) v = static_cast<long long>(gpass.as<unsigned long long>());
            return v != 0;
        }

        // Nothing decisive: consider this a fail (safer default)
        spdlog::warn("Lua code produced no boolean return and no global 'pass' — treating as FAIL");
        return false;
    } catch (const std::exception& ex) {
        spdlog::error("Exception in eval_lua_against_json: {}", ex.what());
        return false;
    } catch (...) {
        spdlog::error("Unknown exception in eval_lua_against_json");
        return false;
    }
}
