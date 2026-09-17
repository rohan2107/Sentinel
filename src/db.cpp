#include "db.h"
#include <sqlite3.h>
#include <stdexcept>
#include <vector>
#include <sstream>

struct DB::Impl {
    sqlite3* db = nullptr;

    // The runs.id from the most recent persist_run() call, captured
    // immediately after that specific INSERT.
    //
    // get_last_run_id() used to call sqlite3_last_insert_rowid(db) itself,
    // which returns the rowid of the most recent insert on the *connection*,
    // not specifically from `runs`. That was silently correct only because
    // the old features table's run_id column was declared INTEGER PRIMARY
    // KEY, which SQLite aliases directly to the rowid, so inserting into it
    // with our own run_id value left last_insert_rowid() equal to that same
    // value by coincidence. rule_results has a composite primary key
    // (run_id, rule_id), which is not rowid-aliased, so its own internal
    // rowid counter leaked through instead once persist_run started
    // inserting into it -- get_last_run_id() returned the count of
    // rule_results rows ever inserted, not the run id. Caching the value
    // at the one point it is actually known removes the fragility instead
    // of relying on no later insert ever running before get_last_run_id()
    // is called.
    sqlite3_int64 last_run_id = 0;

    // Whether table already has the named column, via PRAGMA table_info.
    // Used to make the additive schema migrations idempotent.
    bool column_exists(const char* table, const char* column) const {
        const std::string sql = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite prepare table_info for ") + table);
        }
        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            // Column 1 of table_info output is the column name.
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == column) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
        return found;
    }
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

    -- Per-rule outcomes, one row per (run, rule) rather than one column per
    -- rule name. Replaces an earlier "features" table that hardcoded
    -- firewall_enabled/av_installed as literal SQL columns -- which silently
    -- dropped every other rule's outcome, and assumed rule ids are shared
    -- across platforms when policies are actually per-device (see
    -- policies/macos_policy.json vs policies/sample_policy.json). This shape
    -- needs no schema change when any future policy on any platform
    -- introduces a new rule id.
    --
    -- weight is stored as applied to THIS run, not looked up later from a
    -- policy file that may since have changed.
    CREATE TABLE IF NOT EXISTS rule_results (
      run_id INTEGER NOT NULL,
      rule_id TEXT NOT NULL,
      passed INTEGER NOT NULL,
      weight INTEGER NOT NULL,
      PRIMARY KEY (run_id, rule_id),
      FOREIGN KEY(run_id) REFERENCES runs(id)
    );

    CREATE INDEX IF NOT EXISTS idx_rule_results_rule ON rule_results(rule_id);

    -- retry queue for at-least-once delivery semantics
    CREATE TABLE IF NOT EXISTS retry_queue (
      run_id INTEGER PRIMARY KEY,
      -- report_hash is the EVENT hash: covers the whole report including its
      -- timestamp, so it identifies one specific report. This is what goes on
      -- the wire and what receivers recompute to verify.
      report_hash TEXT UNIQUE NOT NULL,
      -- posture_hash covers policy/score/details only, excluding timestamp.
      -- Answers "has the state changed?" rather than "is this the same
      -- report?". Agent-local; never transmitted.
      posture_hash TEXT,
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

    // Additive migration: CREATE TABLE IF NOT EXISTS leaves an existing
    // retry_queue untouched, so a database created before posture_hash existed
    // needs the column added explicitly. Guarded by a column check so this is
    // idempotent. There is no migration framework here yet; when one arrives
    // this becomes an ordered, versioned step.
    if (!p->column_exists("retry_queue", "posture_hash")) {
        const char* alter = "ALTER TABLE retry_queue ADD COLUMN posture_hash TEXT;";
        if (sqlite3_exec(p->db, alter, nullptr, nullptr, &errmsg) != SQLITE_OK) {
            std::string e = errmsg ? errmsg : "unknown";
            sqlite3_free(errmsg);
            throw std::runtime_error("sqlite add posture_hash column: " + e);
        }
    }

    // Deliberate exception to "additive only" migrations: the old features
    // table did not just go unused, it actively returned wrong data (0/absent
    // for any rule outside the two hardcoded columns), so wrong-and-present is
    // worse than dropped. DROP TABLE IF EXISTS never errors on a database that
    // never had the table, so this is safe to run unconditionally on every
    // startup rather than needing an existence guard like the ALTER above.
    // Nothing reads "features" -- confirmed against backend/, go-aggregator/
    // and the docs -- so there is no data migration path to preserve here.
    if (sqlite3_exec(p->db, "DROP TABLE IF EXISTS features;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string e = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        throw std::runtime_error("sqlite drop features table: " + e);
    }
}

void DB::persist_run(const std::string& iso_ts,
                     const std::string& hostname,
                     const std::string& policy,
                     int score,
                     const nlohmann::json& details,
                     const nlohmann::json& rule_results) {
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

    // Get the id of the run just inserted, and cache it for get_last_run_id()
    // *before* any further inserts on this connection (rule_results, below)
    // can change what sqlite3_last_insert_rowid() reports. See the comment on
    // Impl::last_run_id for why this is not just belt-and-braces.
    sqlite3_int64 run_id = sqlite3_last_insert_rowid(p->db);
    p->last_run_id = run_id;

    // Insert one row per rule result. rule_results is a flat array of
    // {rule_id, passed, weight} built by the caller during rule evaluation,
    // where the policy's rule definitions (and their weights) are already in
    // scope -- db.cpp deliberately knows nothing about policy/report JSON
    // shape beyond this already-flattened form.
    if (!rule_results.empty()) {
        const char* insert_rule_result =
            "INSERT OR REPLACE INTO rule_results (run_id, rule_id, passed, weight) VALUES (?, ?, ?, ?);";
        sqlite3_stmt* rstmt = nullptr;
        if (sqlite3_prepare_v2(p->db, insert_rule_result, -1, &rstmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("sqlite prepare insert_rule_result");
        }
        for (const auto& rr : rule_results) {
            const std::string rule_id = rr.value("rule_id", std::string());
            if (rule_id.empty()) continue; // defensive: skip malformed entries

            sqlite3_reset(rstmt);
            sqlite3_clear_bindings(rstmt);
            sqlite3_bind_int64(rstmt, 1, run_id);
            sqlite3_bind_text(rstmt, 2, rule_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(rstmt, 3, rr.value("passed", false) ? 1 : 0);
            sqlite3_bind_int(rstmt, 4, rr.value("weight", 0));

            if (sqlite3_step(rstmt) != SQLITE_DONE) {
                sqlite3_finalize(rstmt);
                throw std::runtime_error("sqlite insert_rule_result step failed");
            }
        }
        sqlite3_finalize(rstmt);
    }

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

nlohmann::json DB::rule_results_for_run(int run_id) {
    nlohmann::json arr = nlohmann::json::array();
    const char* q = "SELECT rule_id, passed, weight FROM rule_results WHERE run_id = ? ORDER BY rule_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, q, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("sqlite prepare rule_results_for_run");
    }
    sqlite3_bind_int(stmt, 1, run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* rule_id = sqlite3_column_text(stmt, 0);
        nlohmann::json j;
        j["rule_id"] = rule_id ? reinterpret_cast<const char*>(rule_id) : "";
        j["passed"] = sqlite3_column_int(stmt, 1) != 0;
        j["weight"] = sqlite3_column_int(stmt, 2);
        arr.push_back(j);
    }
    sqlite3_finalize(stmt);
    return arr;
}

int DB::get_last_run_id() {
    // Cached by persist_run() at the moment runs.id is actually known, not
    // re-derived from connection-wide state here. See Impl::last_run_id.
    return static_cast<int>(p->last_run_id);
}

bool DB::enqueue_report(int run_id,
                        const std::string& report_json,
                        const std::string& report_hash,
                        const std::string& posture_hash) {
    // ON CONFLICT DO NOTHING rather than letting the UNIQUE constraint raise:
    // re-queuing a report that is already queued is exactly what an
    // at-least-once producer does when it is unsure whether the first attempt
    // landed. Treating it as an error turns a benign retry into a failure log.
    const char* sql = R"sql(
        INSERT INTO retry_queue
            (run_id, report_hash, posture_hash, report_json, attempts, state, created_at)
        VALUES (?, ?, ?, ?, 0, 'PENDING', datetime('now'))
        ON CONFLICT(report_hash) DO NOTHING;
    )sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite prepare enqueue_report: ")
                                 + sqlite3_errmsg(p->db));
    }

    sqlite3_bind_int(stmt, 1, run_id);
    sqlite3_bind_text(stmt, 2, report_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, posture_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, report_json.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::string e = sqlite3_errmsg(p->db);
        sqlite3_finalize(stmt);
        throw std::runtime_error("sqlite enqueue_report step failed: " + e);
    }
    sqlite3_finalize(stmt);

    // DO NOTHING suppresses the insert silently, so changes() is what
    // distinguishes "queued" from "already queued".
    return sqlite3_changes(p->db) > 0;
}

std::string DB::last_reported_posture_hash() {
    // Ordered by run_id, not created_at: created_at has one-second resolution
    // via datetime('now'), so two runs in the same second would be ambiguous.
    // run_id comes from runs.id AUTOINCREMENT and is monotonic.
    const char* sql = R"sql(
        SELECT posture_hash FROM retry_queue
        WHERE state IN ('PENDING', 'DELIVERED')
          AND posture_hash IS NOT NULL
        ORDER BY run_id DESC
        LIMIT 1;
    )sql";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(p->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite prepare last_reported_posture_hash: ")
                                 + sqlite3_errmsg(p->db));
    }

    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text) result = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<QueuedReport> DB::load_pending_reports() {
    // Load PENDING reports that are ready (next_retry_at is NULL or <= now)
    const char* sql = R"sql(
        SELECT run_id, report_hash, COALESCE(posture_hash, ''), report_json,
               attempts, state, COALESCE(next_retry_at, ''), created_at,
               COALESCE(last_error, '')
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
        
        // Indices must track the SELECT list above exactly:
        //   0 run_id, 1 report_hash, 2 posture_hash, 3 report_json,
        //   4 attempts, 5 state, 6 next_retry_at, 7 created_at, 8 last_error
        // Defensive NULL checks (even though COALESCE should prevent NULLs)
        const char* hash_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* posture_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* json_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* state_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* next_retry_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        const char* created_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        const char* error_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));

        r.report_hash = hash_ptr ? hash_ptr : "";
        r.posture_hash = posture_ptr ? posture_ptr : "";
        r.report_json = json_ptr ? json_ptr : "";
        r.state = state_ptr ? state_ptr : "";
        r.next_retry_at = next_retry_ptr ? next_retry_ptr : "";
        r.created_at = created_ptr ? created_ptr : "";
        r.last_error = error_ptr ? error_ptr : "";
        r.attempts = sqlite3_column_int(stmt, 4);
        
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
