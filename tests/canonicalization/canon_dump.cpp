// Emits the canonical encoding and SHA-256 of every fixture, one JSON object
// per line, for tests/canonicalization/compare.py to diff against the Python
// and Go emitters.
//
// Calls canonicalize_json/compute_report_hash from src/report_hasher.cpp
// directly, so this exercises the agent's real encoder rather than a copy.
//
// Usage: canon_dump <fixtures.json>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include "report_hasher.h"

using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: canon_dump <fixtures.json>\n";
        return 2;
    }

    std::ifstream ifs(argv[1]);
    if (!ifs) {
        std::cerr << "cannot open fixtures: " << argv[1] << "\n";
        return 1;
    }

    json fixtures;
    try {
        ifs >> fixtures;
    } catch (const std::exception& e) {
        std::cerr << "invalid fixtures JSON: " << e.what() << "\n";
        return 1;
    }
    if (!fixtures.is_array()) {
        std::cerr << "fixtures must be a JSON array\n";
        return 1;
    }

    for (const auto& f : fixtures) {
        const std::string id = f.at("id").get<std::string>();
        const json& report = f.at("report");

        std::string canonical;
        std::string hash;
        std::string error;
        try {
            canonical = canonicalize_json(report);
            hash = compute_report_hash(report);
        } catch (const std::exception& e) {
            error = e.what();
        }

        // Build the output line as JSON so the driver never has to parse
        // delimiters out of canonical forms that contain arbitrary bytes.
        json line = {
            {"id", id},
            {"impl", "cpp"},
        };
        if (error.empty()) {
            line["canonical"] = canonical;
            line["hash"] = hash;
        } else {
            line["error"] = error;
        }
        // ensure_ascii=true here: this is the transport for the comparison, not
        // the artifact being compared, and ASCII output keeps it terminal-safe.
        std::cout << line.dump() << "\n";
    }
    return 0;
}
