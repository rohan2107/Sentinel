// src/json_to_lua.h
#pragma once
#include <nlohmann/json.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Pushes a nlohmann::json value onto the given lua_State as a native Lua value.
// - objects -> table with string keys
// - arrays  -> table with integer keys starting at 1
// - string, boolean, number, null -> corresponding lua types
// The function pushes the value onto the stack. It does not pop anything.
void push_json_to_lua(lua_State* L, const nlohmann::json& j);
