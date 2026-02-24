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

    -- retry queue for at-least-once delivery semantics
    CREATE TABLE IF NOT EXISTS retry_queue (
      run_id INTEGER PRIMARY KEY,
      report_hash TEXT UNIQUE NOT NULL,
      report_json TEXT NOT NULL,
      attempts INTEGER DEFAULT 0,
      state TEXT NOT NULL CHECK (state IN ('PENDING', 'DELIVERED', 'FAILED')),
      next_retry_at TEXT,
      created_at TEXT NOT NULL,
      delivered_at TEXT,
      failed_at TEXT,
      last_error TEXT,
      FOREIGN KEY(run_id) REFERENCES runs(id)
    );

    CREATE INDEX IF NOT EXISTS idx_retry_state ON retry_queue(state, next_retry_at);
    CREATE INDEX IF NOT EXISTS idx_retry_hash ON retry_queue(report_hash);
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

int DB::get_last_run_id() {
    return static_cast<int>(sqlite3_last_insert_rowid(p->db));
}

void DB::enqueue_report(int run_id, const std::string& report_json, const std::string& report_hash) {
    const char* sql = R"sql(
        INSERT INTO retry_queue (run_id, report_hash, report_json, attempts, state, created_at)
        VALUES (?, ?, ?, 0, 'PENDING', datetime('now'));
    )sql";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare enqueue_report");
    }
    
    sqlite3_bind_int(stmt, 1, run_id);
    sqlite3_bind_text(stmt, 2, report_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, report_json.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite enqueue_report step failed");
    }
    sqlite3_finalize(stmt);
}

std::vector<QueuedReport> DB::load_pending_reports() {
    // Load PENDING reports that are ready (next_retry_at is NULL or <= now)
    const char* sql = R"sql(
        SELECT run_id, report_hash, report_json, attempts, state, 
               COALESCE(next_retry_at, ''), created_at, COALESCE(last_error, '')
        FROM retry_queue
        WHERE state = 'PENDING' 
          AND (next_retry_at IS NULL OR next_retry_at <= datetime('now'))
        ORDER BY created_at ASC;
    )sql";
    
    sqlite3_stmt* stmt = nullptr;
    std::vector<QueuedReport> reports;
    
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare load_pending_reports");
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueuedReport r;
        r.run_id = sqlite3_column_int(stmt, 0);
        
        // Defensive NULL checks (even though COALESCE should prevent NULLs)
        const char* hash_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* json_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* state_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* next_retry_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* created_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* error_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        
        r.report_hash = hash_ptr ? hash_ptr : "";
        r.report_json = json_ptr ? json_ptr : "";
        r.state = state_ptr ? state_ptr : "";
        r.next_retry_at = next_retry_ptr ? next_retry_ptr : "";
        r.created_at = created_ptr ? created_ptr : "";
        r.last_error = error_ptr ? error_ptr : "";
        r.attempts = sqlite3_column_int(stmt, 3);
        
        reports.push_back(r);
    }
    
    sqlite3_finalize(stmt);
    return reports;
}

void DB::mark_delivered(int run_id, const std::string& delivered_at) {
    const char* sql = R"sql(
        UPDATE retry_queue 
        SET state = 'DELIVERED', delivered_at = ?
        WHERE run_id = ?;
    )sql";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare mark_delivered");
    }
    
    sqlite3_bind_text(stmt, 1, delivered_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, run_id);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite mark_delivered step failed");
    }
    sqlite3_finalize(stmt);
}

void DB::mark_failed(int run_id, const std::string& failed_at, const std::string& last_error) {
    const char* sql = R"sql(
        UPDATE retry_queue 
        SET state = 'FAILED', failed_at = ?, last_error = ?
        WHERE run_id = ?;
    )sql";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare mark_failed");
    }
    
    sqlite3_bind_text(stmt, 1, failed_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, run_id);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite mark_failed step failed");
    }
    sqlite3_finalize(stmt);
}

void DB::update_retry(int run_id, int attempts, const std::string& next_retry_at, const std::string& error) {
    const char* sql = R"sql(
        UPDATE retry_queue 
        SET attempts = ?, next_retry_at = ?, last_error = ?
        WHERE run_id = ?;
    )sql";
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare update_retry");
    }
    
    sqlite3_bind_int(stmt, 1, attempts);
    sqlite3_bind_text(stmt, 2, next_retry_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, run_id);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite update_retry step failed");
    }
    sqlite3_finalize(stmt);
}

std::string DB::get_queue_state(int run_id) {
    const char* sql = "SELECT state FROM retry_queue WHERE run_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare get_queue_state");
    }
    sqlite3_bind_int(stmt, 1, run_id);

    std::string state;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        state = ptr ? ptr : "";
    }
    sqlite3_finalize(stmt);
    return state;
}
