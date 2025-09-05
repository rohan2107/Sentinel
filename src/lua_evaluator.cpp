#include "lua_evaluator.h"
#include <sol/sol.hpp>

using json = nlohmann::json;

static void json_row_to_lua(sol::table& t, const json& j, sol::state_view lua) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string key = it.key();
        const json& val = it.value();
        if (val.is_string()) t[key] = val.get<std::string>();
        else if (val.is_boolean()) t[key] = val.get<bool>();
        else if (val.is_number_integer()) t[key] = (long long)val.get<long long>();
        else if (val.is_number_float()) t[key] = val.get<double>();
    }
}

bool eval_lua_against_json(const std::string& luaCode, const json& resultsJson) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::table results = lua.create_table();
    if (resultsJson.is_array()) {
        sol::state_view sv(lua);
        for (size_t i = 0; i < resultsJson.size(); ++i) {
            sol::table row = lua.create_table();
            json_row_to_lua(row, resultsJson[i], sv);
            results[i+1] = row;
        }
    }
    lua["results"] = results;

    std::string wrapped = "function __USER_EVAL__() " + luaCode + " end";
    sol::protected_function_result ok = lua.safe_script(wrapped, &sol::script_pass_on_error);
    if (!ok.valid()) return false;

    sol::protected_function f = lua["__USER_EVAL__"];
    sol::protected_function_result r = f();
    if (!r.valid()) return false;

    if (r.get_type() == sol::type::boolean) return r.get<bool>();
    return !!r;
}
