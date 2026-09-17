// Test: drives eval_lua_against_json with the Lua snippets from
// policies/macos_policy.json against synthetic osquery rows, checking both the
// pass and fail path of each rule. Confirms the sol::lua_nil portability fix
// works at runtime, not just at compile time.
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include "lua_evaluator.h"

using json = nlohmann::json;

static int failures = 0;

static void check(const std::string& name, bool actual, bool expected) {
    const bool ok = (actual == expected);
    if (!ok) ++failures;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << name
              << " -> got " << (actual ? "true" : "false")
              << ", expected " << (expected ? "true" : "false") << "\n";
}

static std::string lua_for(const json& policy, const std::string& id) {
    for (const auto& r : policy.at("rules")) {
        if (r.at("id").get<std::string>() == id) return r.at("lua").get<std::string>();
    }
    throw std::runtime_error("no such rule: " + id);
}

int main(int argc, char** argv) {
    // ctest runs from the build directory, so the policy path is passed in.
    // Falls back to the repo-relative path for direct invocation.
    const std::string policy_path =
        (argc > 1) ? argv[1] : "policies/macos_policy.json";

    std::ifstream ifs(policy_path);
    if (!ifs) {
        std::cerr << "cannot open policy: " << policy_path << "\n";
        return 1;
    }
    json policy; ifs >> policy;

    std::cout << "=== firewall_enabled (alf.global_state) ===\n";
    const auto fw = lua_for(policy, "firewall_enabled");
    check("global_state=1 (on for specific services)", eval_lua_against_json(fw, json::parse(R"([{"global_state":"1","stealth_enabled":"0"}])")), true);
    check("global_state=2 (block all)",                eval_lua_against_json(fw, json::parse(R"([{"global_state":"2","stealth_enabled":"1"}])")), true);
    check("global_state=0 (off)",                      eval_lua_against_json(fw, json::parse(R"([{"global_state":"0","stealth_enabled":"0"}])")), false);
    check("no rows",                                   eval_lua_against_json(fw, json::array()), false);

    std::cout << "=== av_installed (gatekeeper.assessments_enabled) ===\n";
    const auto gk = lua_for(policy, "av_installed");
    check("assessments_enabled=1", eval_lua_against_json(gk, json::parse(R"([{"assessments_enabled":"1","dev_id_enabled":"1"}])")), true);
    check("assessments_enabled=0", eval_lua_against_json(gk, json::parse(R"([{"assessments_enabled":"0","dev_id_enabled":"1"}])")), false);
    check("no rows",               eval_lua_against_json(gk, json::array()), false);

    std::cout << "=== filevault_encrypted (disk_encryption.encrypted) ===\n";
    const auto fv = lua_for(policy, "filevault_encrypted");
    check("encrypted=1", eval_lua_against_json(fv, json::parse(R"([{"encrypted":"1"}])")), true);
    check("encrypted=0", eval_lua_against_json(fv, json::parse(R"([{"encrypted":"0"}])")), false);
    check("no rows",     eval_lua_against_json(fv, json::array()), false);

    std::cout << "=== sip_enabled (sip_config.enabled) ===\n";
    const auto sip = lua_for(policy, "sip_enabled");
    check("enabled=1", eval_lua_against_json(sip, json::parse(R"([{"enabled":"1","enabled_nvram":"1"}])")), true);
    check("enabled=0", eval_lua_against_json(sip, json::parse(R"([{"enabled":"0","enabled_nvram":"0"}])")), false);
    check("no rows",   eval_lua_against_json(sip, json::array()), false);

    std::cout << "=== null handling (exercises sol::lua_nil path) ===\n";
    check("JSON null column value", eval_lua_against_json(fw, json::parse(R"([{"global_state":null}])")), false);
    check("nested null in results", eval_lua_against_json("return results[1].a == nil", json::parse(R"([{"a":null}])")), true);

    std::cout << "\n" << (failures == 0 ? "ALL LUA CHECKS PASSED" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
