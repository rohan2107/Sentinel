// src/json_to_lua.cpp
#include "json_to_lua.h"
#include <string>

using json = nlohmann::json;

static void push_json_internal(lua_State* L, const json& j) {
    if (j.is_null()) {
        lua_pushnil(L);
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>());
    } else if (j.is_number_integer()) {
        lua_pushinteger(L, j.get<lua_Integer>()); // careful if values exceed lua_Integer
    } else if (j.is_number()) {
        lua_pushnumber(L, j.get<lua_Number>());
    } else if (j.is_string()) {
        const std::string& s = j.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
    } else if (j.is_array()) {
        lua_newtable(L);
        int idx = 1;
        for (const auto& el : j) {
            push_json_internal(L, el);   // pushes value
            lua_rawseti(L, -2, idx++);   // table[idx] = value; pops value
        }
    } else if (j.is_object()) {
        lua_newtable(L);
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string key = it.key();
            push_json_internal(L, it.value());    // pushes value
            lua_setfield(L, -2, key.c_str());     // table[key] = value; pops value
        }
    } else {
        lua_pushnil(L);
    }
}

void push_json_to_lua(lua_State* L, const json& j) {
    push_json_internal(L, j);
}
