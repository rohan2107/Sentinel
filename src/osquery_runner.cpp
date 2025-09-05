#include "osquery_runner.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using json = nlohmann::json;

static std::string popen_read_all(const std::string& cmd) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed");
    std::array<char, 256> buf{};
    std::string out;
    while (fgets(buf.data(), (int)buf.size(), pipe)) out += buf.data();
    _pclose(pipe);
#else
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed");
    std::array<char, 256> buf{};
    std::string out;
    while (fgets(buf.data(), buf.size(), pipe)) out += buf.data();
    pclose(pipe);
#endif
    return out;
}

static json simulate_query(const std::string& sql) {
    if (sql.find("firewall_rules") != std::string::npos) {
        return json::array({ {{"name","DemoRule"},{"active",1}} });
    }
    if (sql.find("anti_virus") != std::string::npos) {
        return json::array({ {{"name","Windows Defender"}} });
    }
    return json::array();
}

json run_osquery_json(const std::string& sql, bool simulate) {
    if (simulate) return simulate_query(sql);
    std::string cmd = "osqueryi --json \"" + sql + "\"";
    try {
        std::string out = popen_read_all(cmd);
        if (out.empty()) return json::array();
        return json::parse(out);
    } catch (...) {
        return json::array();
    }
}
