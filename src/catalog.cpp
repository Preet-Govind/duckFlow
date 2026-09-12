#include "catalog.h"
#include <iostream>
#include <sstream>

bool Catalog::init(const std::string& db_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Enable Foreign Keys
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

    // Create Tables
    std::vector<std::string> init_sqls = {
        "CREATE TABLE IF NOT EXISTS projects ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT UNIQUE NOT NULL,"
        "  config TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");",

        "CREATE TABLE IF NOT EXISTS schemas ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(project_name, name)"
        ");",

        "CREATE TABLE IF NOT EXISTS routines ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  schema_name TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  type TEXT NOT NULL," // 'PROCEDURE' or 'FUNCTION'
        "  args TEXT,"          // JSON or comma-separated list
        "  return_type TEXT,"   // return type for function, empty for procedure
        "  body TEXT NOT NULL,"
        "  language TEXT DEFAULT 'SQL',"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(project_name, schema_name, name)"
        ");",

        "CREATE TABLE IF NOT EXISTS jobs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  name TEXT UNIQUE NOT NULL,"
        "  cron_schedule TEXT NOT NULL,"
        "  command TEXT NOT NULL,"
        "  status TEXT DEFAULT 'ENABLED',"
        "  last_run TEXT,"
        "  next_run TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");",

        "CREATE TABLE IF NOT EXISTS job_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  job_id INTEGER NOT NULL,"
        "  status TEXT NOT NULL," // 'RUNNING', 'SUCCESS', 'FAILED'
        "  started_at DATETIME NOT NULL,"
        "  finished_at DATETIME,"
        "  duration_ms INTEGER,"
        "  error_message TEXT,"
        "  logs TEXT"
        ");",

        "CREATE TABLE IF NOT EXISTS connections ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  type TEXT NOT NULL,"
        "  config TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(project_name, name)"
        ");",

        "CREATE TABLE IF NOT EXISTS secrets ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  value TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(project_name, name)"
        ");",

        "CREATE TABLE IF NOT EXISTS tenants ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  api_key TEXT NOT NULL UNIQUE,"
        "  db_path TEXT NOT NULL,"
        "  memory_limit TEXT NOT NULL DEFAULT '1GB',"
        "  thread_limit INTEGER NOT NULL DEFAULT 1,"
        "  qps_limit INTEGER NOT NULL DEFAULT 5,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");",

        "CREATE TABLE IF NOT EXISTS flows ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_name TEXT NOT NULL,"
        "  name TEXT UNIQUE NOT NULL,"
        "  definition_json TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");",

        "CREATE TABLE IF NOT EXISTS flow_runs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  flow_name TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  started_at DATETIME NOT NULL,"
        "  finished_at DATETIME,"
        "  duration_ms INTEGER,"
        "  error_message TEXT"
        ");",

        "CREATE TABLE IF NOT EXISTS flow_step_runs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  run_id INTEGER NOT NULL,"
        "  step_name TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  started_at DATETIME NOT NULL,"
        "  finished_at DATETIME,"
        "  duration_ms INTEGER,"
        "  logs TEXT,"
        "  error_message TEXT"
        ");",

        "CREATE TABLE IF NOT EXISTS duckflow_logs ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  log_time TEXT DEFAULT CURRENT_TIMESTAMP,"
        "  message TEXT,"
        "  routine_context TEXT"
        ");",

        "CREATE TABLE IF NOT EXISTS duckflow_models ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT UNIQUE NOT NULL,"
        "  type TEXT NOT NULL,"
        "  endpoint TEXT,"
        "  api_key TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
    };

    for (const auto& sql : init_sqls) {
        char* err_msg = nullptr;
        rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "SQL error during table initialization: " << err_msg << std::endl;
            sqlite3_free(err_msg);
            return false;
        }
    }

    // [CRASH RECOVERY]
    // If the server was killed forcefully (std::exit) during execution, jobs will be stuck in 'RUNNING'
    // This sweeps the history log on boot and correctly marks them as FAILED.
    const char* recovery_sql = "UPDATE job_history SET status = 'FAILED (Server Shutdown)', "
                               "error_message = 'Gateway process was terminated abruptly.' "
                               "WHERE status = 'RUNNING';";
    sqlite3_exec(db_, recovery_sql, nullptr, nullptr, nullptr);

    const char* recovery_flow_sql = "UPDATE flow_runs SET status = 'FAILED (Server Shutdown)', "
                                    "error_message = 'Gateway process was terminated abruptly.' "
                                    "WHERE status = 'RUNNING';";
    sqlite3_exec(db_, recovery_flow_sql, nullptr, nullptr, nullptr);

    // Create default project & schemas
    createProject("default");
    createSchema("default", "public");

    loadCache();

    return true;
}

void Catalog::close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Catalog::loadCache() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    routines_cache_.clear();
    auto rows = query("SELECT project_name, schema_name, name, type, args, return_type, body, language, created_at FROM routines;");
    for (const auto& r : rows) {
        CatalogRoutine routine;
        routine.project_name = r.at("project_name");
        routine.schema_name = r.at("schema_name");
        routine.name = r.at("name");
        routine.type = r.at("type");
        routine.args = r.at("args");
        routine.return_type = r.at("return_type");
        routine.body = r.at("body");
        routine.language = r.at("language");
        routine.created_at = r.at("created_at");
        
        std::string key = routine.project_name + "." + routine.schema_name + "." + routine.name;
        routines_cache_[key] = routine;
    }
}



bool Catalog::execute(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

std::vector<std::unordered_map<std::string, std::string>> Catalog::query(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::unordered_map<std::string, std::string>> results;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return results;
    }

    int cols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::unordered_map<std::string, std::string> row;
        for (int i = 0; i < cols; ++i) {
            const char* col_name = sqlite3_column_name(stmt, i);
            const unsigned char* col_val = sqlite3_column_text(stmt, i);
            row[col_name] = col_val ? reinterpret_cast<const char*>(col_val) : "NULL";
        }
        results.push_back(row);
    }

    sqlite3_finalize(stmt);
    return results;
}

bool Catalog::createProject(const std::string& name, const std::string& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR IGNORE INTO projects (name, config) VALUES (?, ?);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, config.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_OK;
}

bool Catalog::createSchema(const std::string& project_name, const std::string& schema_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR IGNORE INTO schemas (project_name, name) VALUES (?, ?);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, schema_name.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_OK;
}

bool Catalog::registerRoutine(const std::string& project_name, 
                              const std::string& schema_name, 
                              const std::string& name, 
                              const std::string& type, 
                              const std::string& args, 
                              const std::string& return_type, 
                              const std::string& body, 
                              const std::string& language) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO routines (project_name, schema_name, name, type, args, return_type, body, language) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(project_name, schema_name, name) DO UPDATE SET "
                      "type=excluded.type, args=excluded.args, return_type=excluded.return_type, body=excluded.body, language=excluded.language;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare registerRoutine: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, schema_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, args.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, return_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, language.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        CatalogRoutine routine;
        routine.project_name = project_name;
        routine.schema_name = schema_name;
        routine.name = name;
        routine.type = type;
        routine.args = args;
        routine.return_type = return_type;
        routine.body = body;
        routine.language = language;
        routine.created_at = ""; // Could fetch it, but fine to leave empty in cache after insert
        std::string key = project_name + "." + schema_name + "." + name;
        routines_cache_[key] = routine;
        return true;
    }
    return false;
}

bool Catalog::getRoutine(const std::string& project_name,
                        const std::string& schema_name,
                        const std::string& name,
                        std::string& type,
                        std::string& args,
                        std::string& return_type,
                        std::string& body,
                        std::string& language) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string key = project_name + "." + schema_name + "." + name;
    auto it = routines_cache_.find(key);
    if (it != routines_cache_.end()) {
        type = it->second.type;
        args = it->second.args;
        return_type = it->second.return_type;
        body = it->second.body;
        language = it->second.language;
        return true;
    }
    return false;
}

bool Catalog::registerModel(const std::string& name, const std::string& type, const std::string& endpoint, const std::string& api_key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "INSERT OR REPLACE INTO duckflow_models (name, type, endpoint, api_key) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, api_key.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<CatalogModel> Catalog::getModels() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<CatalogModel> models;
    const char* sql = "SELECT id, name, type, endpoint, api_key, created_at FROM duckflow_models ORDER BY created_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return models;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CatalogModel m;
        m.id = sqlite3_column_int(stmt, 0);
        m.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        m.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.endpoint = sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";
        m.api_key = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
        m.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        models.push_back(m);
    }
    sqlite3_finalize(stmt);
    return models;
}

bool Catalog::getModel(const std::string& name, CatalogModel& out_model) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const char* sql = "SELECT id, name, type, endpoint, api_key, created_at FROM duckflow_models WHERE name = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_model.id = sqlite3_column_int(stmt, 0);
        out_model.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out_model.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out_model.endpoint = sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";
        out_model.api_key = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
        out_model.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Catalog::registerJob(const std::string& project_name, 
                         const std::string& name, 
                         const std::string& cron_expr, 
                         const std::string& command) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO jobs (project_name, name, cron_schedule, command) "
                      "VALUES (?, ?, ?, ?) "
                      "ON CONFLICT(name) DO UPDATE SET "
                      "cron_schedule=excluded.cron_schedule, command=excluded.command;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cron_expr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, command.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<CatalogJob> Catalog::getJobs() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<CatalogJob> jobs;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, project_name, name, cron_schedule, command, status, last_run, next_run FROM jobs;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return jobs;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CatalogJob job;
        job.id = sqlite3_column_int(stmt, 0);
        job.project_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        job.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        job.cron_schedule = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        job.command = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        job.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        
        const unsigned char* lr = sqlite3_column_text(stmt, 6);
        job.last_run = lr ? reinterpret_cast<const char*>(lr) : "";

        const unsigned char* nr = sqlite3_column_text(stmt, 7);
        job.next_run = nr ? reinterpret_cast<const char*>(nr) : "";

        jobs.push_back(job);
    }
    sqlite3_finalize(stmt);
    return jobs;
}

bool Catalog::toggleJobStatus(int job_id, const std::string& new_status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE jobs SET status = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, job_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::logJobStart(int job_id, int64_t& out_history_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO job_history (job_id, status, started_at) VALUES (?, 'RUNNING', datetime('now', 'localtime'));";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, job_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        out_history_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);

    // Also update last run in jobs
    sqlite3_stmt* stmt2 = nullptr;
    const char* sql2 = "UPDATE jobs SET last_run = datetime('now', 'localtime') WHERE id = ?;";
    sqlite3_prepare_v2(db_, sql2, -1, &stmt2, nullptr);
    sqlite3_bind_int(stmt2, 1, job_id);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);

    return rc == SQLITE_DONE;
}

bool Catalog::logJobEnd(int64_t history_id, const std::string& status, int64_t duration_ms, const std::string& error_msg, const std::string& logs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE job_history SET status = ?, finished_at = datetime('now', 'localtime'), duration_ms = ?, error_message = ?, logs = COALESCE(logs, '') || ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, duration_ms);
    sqlite3_bind_text(stmt, 3, error_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, logs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, history_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::appendJobLog(int64_t history_id, const std::string& new_logs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE job_history SET logs = COALESCE(logs, '') || ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, new_logs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, history_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::getJobLog(int64_t history_id, std::string& out_logs, std::string& out_status) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(logs, ''), status FROM job_history WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, history_id);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_logs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Catalog::updateJobNextRun(int job_id, const std::string& next_run_time) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE jobs SET next_run = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, next_run_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, job_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::createConnection(const std::string& project_name, const std::string& name, const std::string& type, const std::string& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO connections (project_name, name, type, config) VALUES (?, ?, ?, ?);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, config.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::createSecret(const std::string& project_name, const std::string& name, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO secrets (project_name, name, value) VALUES (?, ?, ?);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, value.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::string Catalog::getSecret(const std::string& project_name, const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM secrets WHERE project_name = ? AND name = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return "";

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

    std::string secret = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        secret = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return secret;
}

bool Catalog::getTenantDbPath(const std::string& api_key, std::string& db_path, std::string& mem_limit, int& threads, int& qps) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT db_path, memory_limit, thread_limit, qps_limit FROM tenants WHERE api_key = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, api_key.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        db_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        mem_limit = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        threads = sqlite3_column_int(stmt, 2);
        qps = sqlite3_column_int(stmt, 3);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<CatalogTenant> Catalog::getTenants() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<CatalogTenant> tenants;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, api_key, db_path, memory_limit, thread_limit, qps_limit, created_at FROM tenants ORDER BY id DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CatalogTenant t;
            t.id = sqlite3_column_int(stmt, 0);
            t.api_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            t.db_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            t.memory_limit = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            t.thread_limit = sqlite3_column_int(stmt, 4);
            t.qps_limit = sqlite3_column_int(stmt, 5);
            const char* created_at_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            t.created_at = created_at_ptr ? created_at_ptr : "";
            tenants.push_back(t);
        }
    }
    sqlite3_finalize(stmt);
    return tenants;
}

bool Catalog::updateTenantLimits(const std::string& api_key, const std::string& memory, int threads, int qps) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE tenants SET memory_limit = ?, thread_limit = ?, qps_limit = ? WHERE api_key = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, memory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, threads);
    sqlite3_bind_int(stmt, 3, qps);
    sqlite3_bind_text(stmt, 4, api_key.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::deleteTenant(const std::string& api_key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // First, lookup the db_path so we can physically shred the files!
    sqlite3_stmt* lookup_stmt = nullptr;
    const char* lookup_sql = "SELECT db_path FROM tenants WHERE api_key = ?;";
    std::string db_path = "";
    if (sqlite3_prepare_v2(db_, lookup_sql, -1, &lookup_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(lookup_stmt, 1, api_key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(lookup_stmt) == SQLITE_ROW) {
            const char* path_ptr = reinterpret_cast<const char*>(sqlite3_column_text(lookup_stmt, 0));
            if (path_ptr) db_path = path_ptr;
        }
    }
    sqlite3_finalize(lookup_stmt);

    // Physically delete the DuckDB database sandbox files
    if (!db_path.empty()) {
        std::remove(db_path.c_str());
        std::remove((db_path + ".wal").c_str());
        std::remove((db_path + ".tmp").c_str());
        std::cout << "Garbage Collection: Shredded orphaned tenant sandbox at " << db_path << std::endl;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM tenants WHERE api_key = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, api_key.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool Catalog::createTenant(const std::string& api_key, const std::string& db_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO tenants (api_key, db_path) VALUES (?, ?);";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, api_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, db_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::registerFlow(const std::string& project_name, const std::string& name, const std::string& definition_json) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO flows (project_name, name, definition_json) "
                      "VALUES (?, ?, ?) "
                      "ON CONFLICT(name) DO UPDATE SET "
                      "definition_json=excluded.definition_json;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, definition_json.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::getFlow(const std::string& project_name, const std::string& name, std::string& out_definition_json) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT definition_json FROM flows WHERE project_name = ? AND name = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, project_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text) out_definition_json = reinterpret_cast<const char*>(text);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<std::unordered_map<std::string, std::string>> Catalog::getFlows() {
    return query("SELECT id, project_name, name, definition_json, created_at FROM flows ORDER BY name ASC;");
}

bool Catalog::logFlowRunStart(const std::string& flow_name, int64_t& out_run_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO flow_runs (flow_name, status, started_at) VALUES (?, 'RUNNING', datetime('now', 'localtime'));";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, flow_name.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        out_run_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::logFlowRunEnd(int64_t run_id, const std::string& status, int64_t duration_ms, const std::string& error_msg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE flow_runs SET status = ?, finished_at = datetime('now', 'localtime'), duration_ms = ?, error_message = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, duration_ms);
    sqlite3_bind_text(stmt, 3, error_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, run_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::logFlowStepStart(int64_t run_id, const std::string& step_name, int64_t& out_step_run_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO flow_step_runs (run_id, step_name, status, started_at) VALUES (?, ?, 'RUNNING', datetime('now', 'localtime'));";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, run_id);
    sqlite3_bind_text(stmt, 2, step_name.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        out_step_run_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool Catalog::logFlowStepEnd(int64_t step_run_id, const std::string& status, int64_t duration_ms, const std::string& logs, const std::string& error_msg) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE flow_step_runs SET status = ?, finished_at = datetime('now', 'localtime'), duration_ms = ?, logs = ?, error_message = ? WHERE id = ?;";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, duration_ms);
    sqlite3_bind_text(stmt, 3, logs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, error_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, step_run_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::vector<std::unordered_map<std::string, std::string>> Catalog::getFlowRuns(int limit) {
    std::string sql = "SELECT id, flow_name, status, started_at, finished_at, duration_ms, COALESCE(error_message, '') as error_message FROM flow_runs ORDER BY id DESC LIMIT " + std::to_string(limit) + ";";
    return query(sql);
}

std::vector<std::unordered_map<std::string, std::string>> Catalog::getFlowStepRuns(int64_t run_id) {
    std::string sql = "SELECT id, run_id, step_name, status, started_at, finished_at, duration_ms, COALESCE(logs, '') as logs, COALESCE(error_message, '') as error_message FROM flow_step_runs WHERE run_id = " + std::to_string(run_id) + " ORDER BY id ASC;";
    return query(sql);
}

