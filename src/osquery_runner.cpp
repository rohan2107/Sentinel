// src/osquery_runner.cpp
#include "osquery_runner.h"
#include <stdexcept>
#include <sstream>
#include <array>
#include <memory>
#include <cstdio>

#ifdef _WIN32
#  define POPEN _popen
#  define PCLOSE _pclose
#else
#  define POPEN popen
#  define PCLOSE pclose
#endif

using json = nlohmann::json;

static std::string escape_shell_quotes(const std::string& s) {
    // Keep simple: on Windows double quotes are used; on Unix we wrap in single quotes later.
    // We will build a command that uses double quotes for osqueryi --json "<query>"
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\"') {
            out += '\\';
            out += '"';
        } else {
            out += c;
        }
    }
    return out;
}

json run_osquery_json(const std::string& sql_query) {
    // Construct command: osqueryi --json "<query>"
    std::ostringstream cmd;
#ifdef _WIN32
    // windows: use double quotes
    cmd << "osqueryi --json \"" << escape_shell_quotes(sql_query) << "\"";
#else
    // unix: wrap in single quotes to avoid shell interpolation; single quotes inside query are rare for SQL
    // if needed, users can escape single quotes in policy queries by doubling them.
    cmd << "osqueryi --json '" << sql_query << "'";
#endif

    std::array<char, 4096> buffer;
    std::string result;

    FILE* pipe = POPEN(cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to run osqueryi (is it installed and on PATH?)");
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }

    int rc = PCLOSE(pipe);
    (void)rc;

    try {
        if (result.empty()) {
            return json::array();
        }
        json parsed = json::parse(result);
        return parsed;
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "osqueryi returned invalid JSON: " << e.what() << "\nraw output:\n" << result;
        throw std::runtime_error(oss.str());
    }
}
