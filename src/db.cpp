#include "db.h"
#include <sqlite3.h>
#include <stdexcept>
#include <vector>
#include <sstream>

struct DB::Impl {
    sqlite3* db = nullptr;
};

DB::DB(const std::string& path) : p(new Impl()) {
    if (sqlite3_open_v2(path.c_str(), &p->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string e = sqlite3_errmsg(p->db);
        sqlite3_close(p->db);
        throw std::runtime_error("sqlite open: " + e);
    }
    // enable WAL for concurrency and faster writes
    sqlite3_exec(p->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
}

DB::~DB() {
    if (p->db) sqlite3_close(p->db);
    delete p;
}

void DB::init_schema() {
    const char* sql = R"sql(
    CREATE TABLE IF NOT EXISTS runs (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      ts TEXT NOT NULL,
      hostname TEXT,
      policy TEXT,
      score INTEGER,
      details_json TEXT
    );

    -- flattened features table: one row per run, could be extended with more columns for ML
    CREATE TABLE IF NOT EXISTS features (
      run_id INTEGER PRIMARY KEY,
      firewall_enabled INTEGER,
      av_installed INTEGER,
      -- add more feature columns here
      FOREIGN KEY(run_id) REFERENCES runs(id)
    );
    )sql";

    char* errmsg = nullptr;
    if (sqlite3_exec(p->db, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string e = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        throw std::runtime_error("sqlite init_schema: " + e);
    }
}

void DB::persist_run(const std::string& iso_ts,
                     const std::string& hostname,
                     const std::string& policy,
                     int score,
                     const nlohmann::json& details) {
    // begin transaction
    char* errmsg = nullptr;
    sqlite3_exec(p->db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errmsg);
    if (errmsg) { sqlite3_free(errmsg); errmsg = nullptr; }

    const char* insert_run = "INSERT INTO runs (ts, hostname, policy, score, details_json) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, insert_run, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare insert_run");
    }
    sqlite3_bind_text(stmt, 1, iso_ts.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hostname.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, policy.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, score);
    std::string details_s = details.dump();
    sqlite3_bind_text(stmt, 5, details_s.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite insert_run step failed");
    }
    sqlite3_finalize(stmt);

    // get last inserted id
    sqlite3_int64 run_id = sqlite3_last_insert_rowid(p->db);

    // insert features (flattened)
    const char* insert_features = "INSERT OR REPLACE INTO features (run_id, firewall_enabled, av_installed) VALUES (?, ?, ?);";
    sqlite3_stmt* fstmt = nullptr;
    if (sqlite3_prepare_v2(p->db, insert_features, -1, &fstmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare insert_features");
    }
    sqlite3_bind_int64(fstmt, 1, run_id);
    int fw = 0;
    int av = 0;
    try {
        if (details.contains("firewall_enabled")) fw = details.at("firewall_enabled").get<bool>() ? 1 : 0;
        if (details.contains("av_installed")) av = details.at("av_installed").get<bool>() ? 1 : 0;
    } catch (...) { /* best-effort */ }
    sqlite3_bind_int(fstmt, 2, fw);
    sqlite3_bind_int(fstmt, 3, av);

    if (sqlite3_step(fstmt) != SQLITE_DONE) {
        sqlite3_finalize(fstmt);
        throw std::runtime_error("sqlite insert_features step failed");
    }
    sqlite3_finalize(fstmt);

    // commit
    if (sqlite3_exec(p->db, "COMMIT;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string e = errmsg ? errmsg : "commit failed";
        sqlite3_free(errmsg);
        throw std::runtime_error("sqlite commit: " + e);
    }
}

nlohmann::json DB::all_runs_json() {
    nlohmann::json arr = nlohmann::json::array();
    const char* q = "SELECT id, ts, hostname, policy, score, details_json FROM runs ORDER BY id DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, q, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare all_runs");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char* ts = sqlite3_column_text(stmt, 1);
        const unsigned char* hostname = sqlite3_column_text(stmt, 2);
        const unsigned char* policy = sqlite3_column_text(stmt, 3);
        int score = sqlite3_column_int(stmt, 4);
        const unsigned char* details = sqlite3_column_text(stmt, 5);
        nlohmann::json j;
        j["id"] = id;
        j["ts"] = ts ? reinterpret_cast<const char*>(ts) : "";
        j["hostname"] = hostname ? reinterpret_cast<const char*>(hostname) : "";
        j["policy"] = policy ? reinterpret_cast<const char*>(policy) : "";
        j["score"] = score;
        try {
            if (details) j["details"] = nlohmann::json::parse(reinterpret_cast<const char*>(details));
            else j["details"] = nullptr;
        } catch (...) {
            j["details"] = nlohmann::json::parse("{}");
        }
        arr.push_back(j);
    }
    sqlite3_finalize(stmt);
    return arr;
}
