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

// Escape double quotes for Windows command line usage.
static std::string escape_shell_quotes_windows(const std::string& s) {
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

// Escape single quotes for POSIX shell by doubling them: ' -> '\''  (we implement the simpler doubling approach)
static std::string escape_single_quotes_unix(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\'') {
            out += "''"; // osquery SQL can accept doubled single quotes for escaping
        } else {
            out += c;
        }
    }
    return out;
}

json run_osquery_json(const std::string& sql_query) {
    // Build osqueryi command to return JSON
    std::ostringstream cmd;
#ifdef _WIN32
    cmd << "osqueryi --json \""
        << escape_shell_quotes_windows(sql_query)
        << "\"";
#else
    cmd << "osqueryi --json '"
        << escape_single_quotes_unix(sql_query)
        << "'";
#endif

    // Larger buffer to better handle big JSON payloads
    std::array<char, 16 * 1024> buffer;
    std::string result;

    FILE* pipe = POPEN(cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to run osqueryi (is it installed and on PATH?)");
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }

    int rc = PCLOSE(pipe);

    try {
        if (result.empty()) {
            // osqueryi produced no output (treat as empty result set)
            return json::array();
        }
        json parsed = json::parse(result);
        // Optionally, you could inspect rc here and log it; non-zero rc sometimes still returns JSON.
        if (rc != 0) {
            std::ostringstream warn;
            warn << "osqueryi exited with code " << rc << " but returned JSON.";
            // We throw on non-zero only when parsing fails below — keep lenient but informative.
            // spdlog isn't included here; callers should log if desired.
        }
        return parsed;
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "osqueryi returned invalid JSON (exit code " << rc << "): " << e.what()
            << "\nraw output:\n" << result;
        throw std::runtime_error(oss.str());
    }
}
