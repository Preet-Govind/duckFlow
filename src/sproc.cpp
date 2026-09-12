#include "sproc.h"
#include "catalog.h"
#include "stat_activity.h"
#include "duckdb_engine.h"
#include "flow_engine.h"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <regex>
#include "raft.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

std::string SQLRouter::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool SQLRouter::startsWithIgnoreCase(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), str.begin(),
                      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

std::string SQLRouter::replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}
std::string SQLRouter::stripComments(const std::string& sql) {
    std::string result;
    bool in_quotes = false;
    char quote_char = 0;
    bool in_single_comment = false;
    bool in_multi_comment = false;

    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];
        
        if (in_single_comment) {
            if (c == '\n') {
                in_single_comment = false;
                result += c;
            }
            continue;
        }
        
        if (in_multi_comment) {
            if (c == '*' && i + 1 < sql.size() && sql[i+1] == '/') {
                in_multi_comment = false;
                i++; // skip '/'
            }
            continue;
        }

        if (!in_quotes) {
            // Check for multi-line comment start
            if (c == '/' && i + 1 < sql.size() && sql[i+1] == '*') {
                in_multi_comment = true;
                i++; // skip '*'
                continue;
            }
            // Check for single-line comment start
            if (c == '-' && i + 1 < sql.size() && sql[i+1] == '-') {
                in_single_comment = true;
                i++; // skip '-'
                continue;
            }
        }

        if ((c == '\'' || c == '"') && (i == 0 || sql[i-1] != '\\')) {
            if (!in_quotes) {
                in_quotes = true;
                quote_char = c;
            } else if (c == quote_char) {
                in_quotes = false;
            }
        }
        
        result += c;
    }
    
    return result;
}



std::vector<std::string> SQLRouter::splitStatements(const std::string& body) {
    std::vector<std::string> stmts;
    std::string current;
    bool in_quotes = false;
    char quote_char = 0;
    
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if ((c == '\'' || c == '"') && (i == 0 || body[i-1] != '\\')) {
            if (!in_quotes) {
                in_quotes = true;
                quote_char = c;
            } else if (c == quote_char) {
                in_quotes = false;
            }
        }
        
        if (c == ';' && !in_quotes) {
            current = trim(current);
            if (!current.empty()) {
                stmts.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    current = trim(current);
    if (!current.empty()) {
        stmts.push_back(current);
    }
    return stmts;
}

bool SQLRouter::routeAndExecute(const std::string& raw_sql, 
                                 ExecutionEngine* engine,
                                 std::vector<std::string>& out_col_names, 
                                 std::vector<std::vector<std::string>>& out_rows, 
                                 std::string& out_error_msg,
                                 std::unordered_map<std::string, std::string>* vars) {
    std::string sql = trim(stripComments(raw_sql));
    if (sql.empty()) {
        out_error_msg = "Empty query.";
        return false;
    }

    // Split into individual statements if there are semicolons outside quotes
    // (Except for CREATE ROUTINE/JOB which contain internal semicolons)
    if (!startsWithIgnoreCase(sql, "CREATE FUNCTION") && 
        !startsWithIgnoreCase(sql, "CREATE OR REPLACE FUNCTION") &&
        !startsWithIgnoreCase(sql, "CREATE PROCEDURE") && 
        !startsWithIgnoreCase(sql, "CREATE OR REPLACE PROCEDURE") &&
        !startsWithIgnoreCase(sql, "CREATE JOB")) {
        
        std::vector<std::string> stmts = splitStatements(sql);
        if (stmts.size() > 1) {
            std::vector<std::string> last_cols;
            std::vector<std::vector<std::string>> last_rows;
            for (const auto& stmt : stmts) {
                std::vector<std::string> tmp_cols;
                std::vector<std::vector<std::string>> tmp_rows;
                std::string err;
                if (!routeAndExecute(stmt, engine, tmp_cols, tmp_rows, err)) {
                    out_error_msg = err;
                    return false;
                }
                last_cols = tmp_cols;
                last_rows = tmp_rows;
            }
            out_col_names = last_cols;
            out_rows = last_rows;
            return true;
        }
    }

    // Intercept Custom DDL
    std::string msg;
    if (startsWithIgnoreCase(sql, "CREATE PROJECT ")) {
        bool ok = handleCreateProject(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE SCHEMA ")) {
        bool ok = handleCreateSchema(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE PROCEDURE ") || startsWithIgnoreCase(sql, "CREATE OR REPLACE PROCEDURE ")) {
        bool ok = handleCreateRoutine(sql, true, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE FUNCTION ") || startsWithIgnoreCase(sql, "CREATE OR REPLACE FUNCTION ")) {
        bool ok = handleCreateRoutine(sql, false, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE JOB ") || startsWithIgnoreCase(sql, "CREATE OR REPLACE JOB ")) {
        bool ok = handleCreateJob(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE FLOW ") || startsWithIgnoreCase(sql, "CREATE OR REPLACE FLOW ")) {
        bool ok = handleCreateFlow(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE MODEL ") || startsWithIgnoreCase(sql, "CREATE OR REPLACE MODEL ")) {
        bool ok = handleCreateModel(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "RUN FLOW ") || startsWithIgnoreCase(sql, "EXECUTE FLOW ")) {
        bool ok = handleRunFlow(sql, engine, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE CONNECTION ")) {
        bool ok = handleCreateConnection(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "CREATE SECRET ")) {
        bool ok = handleCreateSecret(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "ALTER TENANT ")) {
        bool ok = handleAlterTenant(sql, msg);
        out_col_names = {"status"};
        out_rows = {{ok ? "SUCCESS: " + msg : "ERROR: " + msg}};
        if (!ok) out_error_msg = msg;
        return ok;
    } else if (startsWithIgnoreCase(sql, "USE DATABASE ")) {
        // USE DATABASE 'db_path[;so_path]'
        std::regex re("USE\\s+DATABASE\\s+'([^']+)'", std::regex_constants::icase);
        std::smatch match;
        if (std::regex_search(sql, match, re) && match.size() > 1) {
            std::string conn_str = match[1].str();
            bool ok = engine->connect(conn_str);
            out_col_names = {"status"};
            if (ok) {
                out_rows = {{"SUCCESS: Switched database to '" + conn_str + "'."}};
                return true;
            } else {
                out_rows = {{"ERROR: Failed to switch database to '" + conn_str + "'."}};
                return false;
            }
        }
        out_error_msg = "Invalid USE DATABASE syntax. Required format: USE DATABASE 'db_path[;so_path]'";
        return false;
    } else if (startsWithIgnoreCase(sql, "USE TEMP DATABASE") || startsWithIgnoreCase(sql, "USE ISOLATED DATABASE")) {
        out_col_names = {"status"};
        out_rows = {{"SUCCESS: Initialized isolated in-memory execution sandbox."}};
        return true;
    } else if (startsWithIgnoreCase(sql, "CALL ")) {
        // [ARCHITECTURE] 
        // Intercepts CALL procedures and executes them statement-by-statement.
        // It provides MotherDuck-style execution isolation and garbage collection.
        return handleCall(sql, engine, out_col_names, out_rows, out_error_msg, vars);
    } else if (startsWithIgnoreCase(sql, "SELECT ") && 
               (sql.find("teal_stat_activity") != std::string::npos || 
                sql.find("pg_stat_activity") != std::string::npos ||
                sql.find("teal_jobs") != std::string::npos ||
                sql.find("teal_job_history") != std::string::npos ||
                sql.find("teal_routines") != std::string::npos ||
                sql.find("teal_logs") != std::string::npos ||
                sql.find("teal_models") != std::string::npos)) {
        return handleSystemQuery(sql, engine, out_col_names, out_rows, out_error_msg);
    }

    // [ARCHITECTURE] 
    // Default Fallback: Any standard analytical SQL query is directly passed to the 
    // underlying C++ DuckDB Engine for execution.
    return engine->executeQuery(sql, out_col_names, out_rows, out_error_msg);
}



bool SQLRouter::handleCreateProject(const std::string& sql, std::string& out_msg) {
    // CREATE PROJECT <name>
    std::regex re("CREATE\\s+PROJECT\\s+(\\w+)", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 1) {
        std::string project_name = match[1].str();
        if (Catalog::getInstance().createProject(project_name)) {
            out_msg = "Project '" + project_name + "' created successfully.";
            return true;
        }
    }
    out_msg = "Failed to parse or create project.";
    return false;
}

bool SQLRouter::handleCreateSchema(const std::string& sql, std::string& out_msg) {
    // CREATE SCHEMA [project.]<name>
    std::regex re("CREATE\\s+SCHEMA\\s+([\\w\\.]+)", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 1) {
        std::string target = match[1].str();
        std::string proj = "default";
        std::string schema = target;
        
        size_t dot = target.find('.');
        if (dot != std::string::npos) {
            proj = target.substr(0, dot);
            schema = target.substr(dot + 1);
        }
        
        if (Catalog::getInstance().createSchema(proj, schema)) {
            out_msg = "Schema '" + schema + "' created in project '" + proj + "' successfully.";
            return true;
        }
    }
    out_msg = "Failed to parse or create schema.";
    return false;
}

bool SQLRouter::handleCreateRoutine(const std::string& sql, bool is_procedure, std::string& out_msg) {
    // CREATE [OR REPLACE] PROCEDURE [project.][schema.]<name>([args]) AS BEGIN <body> END;
    std::string type_str = is_procedure ? "PROCEDURE" : "FUNCTION";
    
    std::string sql_upper = sql;
    std::transform(sql_upper.begin(), sql_upper.end(), sql_upper.begin(), ::toupper);
    
    size_t start_pos = sql_upper.find(type_str);
    if (start_pos == std::string::npos) {
        out_msg = "Invalid CREATE [OR REPLACE] " + type_str + " syntax.";
        return false;
    }
    
    start_pos += type_str.size();
    size_t paren_open = sql.find('(', start_pos);
    if (paren_open == std::string::npos) {
        out_msg = "Missing arguments list parentheses.";
        return false;
    }
    
    std::string full_name = trim(sql.substr(start_pos, paren_open - start_pos));
    
    size_t paren_close = sql.find(')', paren_open);
    if (paren_close == std::string::npos) {
        out_msg = "Unclosed arguments list.";
        return false;
    }
    
    std::string args = trim(sql.substr(paren_open + 1, paren_close - paren_open - 1));
    
    size_t begin_pos = sql_upper.find("BEGIN", paren_close);
    if (begin_pos == std::string::npos) {
        out_msg = "Missing 'BEGIN' block.";
        return false;
    }

    // Check if return type exists (for functions)
    std::string return_type = "";
    if (!is_procedure) {
        size_t returns_pos = sql_upper.find("RETURNS", paren_close);
        if (returns_pos != std::string::npos && returns_pos < begin_pos) {
            size_t as_pos = sql_upper.find("AS", returns_pos);
            if (as_pos != std::string::npos && as_pos < begin_pos) {
                return_type = trim(sql.substr(returns_pos + 7, as_pos - returns_pos - 7));
            } else {
                return_type = trim(sql.substr(returns_pos + 7, begin_pos - returns_pos - 7));
            }
        }
    }
    
    size_t end_pos = sql_upper.rfind("END");
    if (end_pos == std::string::npos || end_pos < begin_pos) {
        out_msg = "Missing 'END' keyword.";
        return false;
    }
    
    std::string body = trim(sql.substr(begin_pos + 5, end_pos - begin_pos - 5));
    
    // Resolve project/schema/name
    std::string proj = "default";
    std::string schema = "public";
    std::string name = full_name;
    
    std::vector<std::string> parts;
    std::stringstream ss(full_name);
    std::string part;
    while (std::getline(ss, part, '.')) {
        parts.push_back(part);
    }
    
    if (parts.size() == 3) {
        proj = parts[0];
        schema = parts[1];
        name = parts[2];
    } else if (parts.size() == 2) {
        schema = parts[0];
        name = parts[1];
    }
    
    if (Catalog::getInstance().registerRoutine(proj, schema, name, type_str, args, return_type, body, "SQL")) {
        out_msg = type_str + " '" + schema + "." + name + "' registered successfully.";
        return true;
    }
    
    out_msg = "Failed to register routine in Catalog database.";
    return false;
}

bool SQLRouter::handleCreateJob(const std::string& sql, std::string& out_msg) {
    // CREATE [OR REPLACE] JOB <name> SCHEDULE '<cron>' AS <command>;
    std::regex re("CREATE\\s+(?:OR\\s+REPLACE\\s+)?JOB\\s+(\\w+)\\s+SCHEDULE\\s+'([^']+)'\\s+AS\\s+(.+)", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 3) {
        std::string job_name = match[1].str();
        std::string cron_schedule = match[2].str();
        std::string command = trim(match[3].str());
        
        // Strip trailing semicolon from command if present
        if (!command.empty() && command.back() == ';') {
            command.pop_back();
        }
        
        if (Catalog::getInstance().registerJob("default", job_name, cron_schedule, command)) {
            out_msg = "Job '" + job_name + "' created with schedule '" + cron_schedule + "' successfully.";
            return true;
        }
    }
    out_msg = "Failed to parse or register Job. Required format: CREATE JOB <name> SCHEDULE '<cron>' AS <command>";
    return false;
}

bool SQLRouter::handleCreateFlow(const std::string& sql, std::string& out_msg) {
    FlowDefinition flow;
    std::string json_str;
    std::string err;
    if (!FlowEngine::getInstance().parseCreateFlowDDL(sql, flow, json_str, err)) {
        out_msg = "Failed to parse CREATE FLOW DDL: " + err;
        return false;
    }

    if (!Catalog::getInstance().registerFlow(flow.project_name, flow.name, json_str)) {
        out_msg = "Failed to register FLOW '" + flow.name + "' in catalog.";
        return false;
    }

    out_msg = "Flow '" + flow.name + "' registered successfully as a valid DAG with " + std::to_string(flow.steps.size()) + " steps.";
    return true;
}

bool SQLRouter::handleRunFlow(const std::string& sql, ExecutionEngine* engine, std::string& out_msg) {
    std::regex re("(?:RUN|EXECUTE)\\s+FLOW\\s+([\\w\\.]+)", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 1) {
        std::string target = match[1].str();
        std::string proj = "default";
        std::string flow_name = target;

        size_t dot = target.find('.');
        if (dot != std::string::npos) {
            proj = target.substr(0, dot);
            flow_name = target.substr(dot + 1);
        }

        int64_t run_id = 0;
        std::string err;
        if (FlowEngine::getInstance().executeFlow(proj, flow_name, engine, run_id, err)) {
            out_msg = "Flow '" + flow_name + "' (Run ID: " + std::to_string(run_id) + ") completed successfully.";
            return true;
        } else {
            out_msg = "Flow '" + flow_name + "' (Run ID: " + std::to_string(run_id) + ") failed: " + err;
            return false;
        }
    }
    out_msg = "Syntax error. Required format: RUN FLOW <flow_name> or EXECUTE FLOW <flow_name>";
    return false;
}

bool SQLRouter::handleCreateConnection(const std::string& sql, std::string& out_msg) {
    // CREATE CONNECTION <name> TYPE <type> CONFIG '<config>'
    std::regex re("CREATE\\s+CONNECTION\\s+(\\w+)\\s+TYPE\\s+(\\w+)\\s+CONFIG\\s+'([^']+)'", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 3) {
        std::string conn_name = match[1].str();
        std::string conn_type = match[2].str();
        std::string config = match[3].str();
        
        if (Catalog::getInstance().createConnection("default", conn_name, conn_type, config)) {
            out_msg = "Connection '" + conn_name + "' of type '" + conn_type + "' created successfully.";
            return true;
        }
    }
    out_msg = "Failed to parse or create connection.";
    return false;
}

bool SQLRouter::handleCreateSecret(const std::string& sql, std::string& out_msg) {
    // CREATE SECRET <name> VALUE '<value>'
    std::regex re("CREATE\\s+SECRET\\s+(\\w+)\\s+VALUE\\s+'([^']+)'", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re) && match.size() > 2) {
        std::string secret_name = match[1].str();
        std::string secret_val = match[2].str();
        
        if (Catalog::getInstance().createSecret("default", secret_name, secret_val)) {
            out_msg = "Secret '" + secret_name + "' created successfully.";
            return true;
        }
    }
    out_msg = "Failed to parse or create secret.";
    return false;
}

bool SQLRouter::handleCall(const std::string& sql, 
                           ExecutionEngine* engine, 
                           std::vector<std::string>& out_col_names, 
                           std::vector<std::vector<std::string>>& out_rows, 
                           std::string& out_error_msg,
                           std::unordered_map<std::string, std::string>* parent_vars) {
    // CALL [schema.]<name>([args])
    std::regex re("CALL\\s+([\\w\\.]+)\\s*\\((.*)\\)", std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(sql, match, re) || match.size() < 3) {
        out_error_msg = "Invalid CALL syntax. Format: CALL <procedure_name>(val1, val2)";
        return false;
    }
    
    std::string full_name = match[1].str();
    std::string raw_args = match[2].str();
    
    std::string proj = "default";
    std::string schema = "public";
    std::string name = full_name;
    
    std::vector<std::string> parts;
    std::stringstream ss(full_name);
    std::string part;
    while (std::getline(ss, part, '.')) {
        parts.push_back(part);
    }
    if (parts.size() == 3) {
        proj = parts[0];
        schema = parts[1];
        name = parts[2];
    } else if (parts.size() == 2) {
        schema = parts[0];
        name = parts[1];
    }
    
    // Look up routine
    std::string type, args_def, return_type, body, language;
    if (!Catalog::getInstance().getRoutine(proj, schema, name, type, args_def, return_type, body, language)) {
        out_error_msg = "Procedure or Function '" + schema + "." + name + "' not found in catalog.";
        return false;
    }
    
    // Check if the body requests an isolated temporary database context
    bool use_isolated = false;
    std::string temp_db_path = "";
    std::string engine_so_path = "";
    
    std::regex eng_re("USE\\s+ENGINE\\s+'([^']+)'(?:\\s+VERSION\\s+'([^']+)')?", std::regex_constants::icase);
    std::smatch eng_match;
    if (std::regex_search(body, eng_match, eng_re)) {
        use_isolated = true;
        std::string eng_name = eng_match[1].str();
        std::string version = (eng_match.size() > 2 && eng_match[2].matched) ? eng_match[2].str() : "latest";
        
        if (eng_name == "lancedb") {
            engine_so_path = "third_party/lancedb/v" + version + "/lance.so";
        } else {
            engine_so_path = "third_party/" + eng_name + "/v" + version + "/lib" + eng_name + ".so";
        }
    }

    std::regex temp_db_re("USE\\s+(?:TEMP|ISOLATED)\\s+DATABASE(?:\\s+'([^']+)')?", std::regex_constants::icase);
    std::smatch temp_db_match;
    if (std::regex_search(body, temp_db_match, temp_db_re)) {
        use_isolated = true;
        if (temp_db_match.size() > 1 && temp_db_match[1].matched) {
            std::string user_path = temp_db_match[1].str();
            // Force file-based temp databases into the resilient sandboxes directory
            if (user_path.find("sandboxes/") != 0 && user_path.find("/") == std::string::npos) {
                temp_db_path = "sandboxes/" + user_path;
            } else {
                temp_db_path = user_path;
            }
        }
    }

    // Set up engine pointer
    DuckDbEngine local_engine;
    ExecutionEngine* active_engine = engine;
    
    // [ARCHITECTURE: MULTI-ENGINE GC]
    // RAII Garbage Collector for File-Based Temporary Databases.
    // If the procedure logic runs `USE TEMP DATABASE 'path.db'`, we isolate it.
    // When this struct falls out of scope (procedure finishes), it cleanly unlinks the disk files.
    struct TempDbGC {
        std::string path;
        DuckDbEngine* eng;
        ~TempDbGC() {
            if (!path.empty() && eng) {
                eng->disconnect(); // Ensure file handles are closed
                std::remove(path.c_str());
                std::remove((path + ".wal").c_str());
                std::remove((path + ".tmp").c_str());
            }
        }
    } gc;

    if (use_isolated) {
        std::string conn_str = temp_db_path;
        if (!engine_so_path.empty()) {
            conn_str += ";" + engine_so_path;
        }
        if (!local_engine.connect(conn_str)) {
            out_error_msg = "Failed to create isolated DuckDB/LanceDB engine sandbox at: " + (temp_db_path.empty() ? "in-memory" : temp_db_path);
            return false;
        }
        active_engine = &local_engine;
        gc.path = temp_db_path;
        gc.eng = &local_engine;
    }
    
    // Parse call arguments
    std::vector<std::string> call_args;
    std::stringstream ss_args(raw_args);
    std::string arg;
    while (std::getline(ss_args, arg, ',')) {
        call_args.push_back(trim(arg));
    }
    
    // Parse definition arguments
    std::vector<std::string> def_arg_names;
    std::stringstream ss_def(args_def);
    std::string def_item;
    while (std::getline(ss_def, def_item, ',')) {
        def_item = trim(def_item);
        if (!def_item.empty()) {
            size_t space = def_item.find(' ');
            if (space != std::string::npos) {
                def_arg_names.push_back(def_item.substr(0, space));
            } else {
                def_arg_names.push_back(def_item);
            }
        }
    }
    
    if (call_args.size() != def_arg_names.size()) {
        out_error_msg = "Argument count mismatch. Expected " + std::to_string(def_arg_names.size()) + ", got " + std::to_string(call_args.size());
        return false;
    }
    
    // Split procedure body into statements
    std::vector<std::string> stmts = splitStatements(body);
    
    // Set up initial symbol table
    std::unordered_map<std::string, std::string> vars;
    if (parent_vars) {
        vars = *parent_vars;
    }
    
    // Bind arguments to variables
    for (size_t j = 0; j < def_arg_names.size(); ++j) {
        vars[def_arg_names[j]] = call_args[j];
    }
    
    bool is_returned = false;
    return executeBlock(stmts, active_engine, out_col_names, out_rows, out_error_msg, vars, is_returned);
}

bool SQLRouter::executeBlock(const std::vector<std::string>& stmts,
                             ExecutionEngine* engine,
                             std::vector<std::string>& out_col_names, 
                             std::vector<std::vector<std::string>>& out_rows, 
                             std::string& out_error_msg,
                             std::unordered_map<std::string, std::string>& vars,
                             bool& is_returned) {
    std::vector<std::string> last_cols;
    std::vector<std::vector<std::string>> last_rows;

    for (size_t i = 0; i < stmts.size(); ++i) {
        std::string stmt = stmts[i];
        
        // 1. FOR ... IN (query) LOOP block parsing
        std::regex for_re("^FOR\\s+([a-zA-Z0-9_]+)\\s+IN\\s+\\((.*?)\\)\\s*LOOP([\\s\\S]*)", std::regex::icase);
        std::smatch for_match;
        if (std::regex_search(stmt, for_match, for_re)) {
            std::string var_name = for_match[1].str();
            std::string driver_query = for_match[2].str();
            std::string trailing = trim(for_match[3].str());
            
            // Interpolate driver query variables
            for (const auto& kv : vars) {
                driver_query = replaceAll(driver_query, ":" + kv.first, kv.second);
                driver_query = replaceAll(driver_query, "$" + kv.first, kv.second);
                
                // Escape kv.first for regex
                std::string escaped_arg = std::regex_replace(kv.first, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
                driver_query = std::regex_replace(driver_query, std::regex("\\b" + escaped_arg + "\\b"), kv.second);
            }
            
            // Collect inner statements until END LOOP
            std::vector<std::string> inner_stmts;
            if (!trailing.empty()) {
                inner_stmts.push_back(trailing);
            }
            int depth = 1;
            while (++i < stmts.size()) {
                if (std::regex_search(stmts[i], std::regex("^FOR\\s+.*LOOP", std::regex::icase))) {
                    depth++;
                } else if (std::regex_search(stmts[i], std::regex("^END\\s+LOOP", std::regex::icase))) {
                    depth--;
                }
                
                if (depth == 0) {
                    break;
                }
                inner_stmts.push_back(stmts[i]);
            }
            
            // Execute driver query
            std::vector<std::string> drv_cols;
            std::vector<std::vector<std::string>> drv_rows;
            std::string drv_err;
            if (!engine->executeQuery(driver_query, drv_cols, drv_rows, drv_err)) {
                out_error_msg = "Error executing loop query: " + drv_err;
                return false;
            }
            
            // Iterate and execute block
            for (const auto& row : drv_rows) {
                for (size_t c = 0; c < drv_cols.size(); ++c) {
                    vars[var_name + "." + drv_cols[c]] = row[c];
                }
                
                std::string blk_err;
                std::vector<std::string> blk_cols;
                std::vector<std::vector<std::string>> blk_rows;
                if (!executeBlock(inner_stmts, engine, blk_cols, blk_rows, blk_err, vars, is_returned)) {
                    out_error_msg = blk_err;
                    return false;
                }
                if (!blk_cols.empty()) {
                    last_cols = blk_cols;
                    last_rows = blk_rows;
                }
                if (is_returned) {
                    out_col_names = last_cols;
                    out_rows = last_rows;
                    return true;
                }
            }
            continue; // Skip the rest of loop logic for this block
        }
        
        // 1.1 WHILE loops
        std::regex while_re("^WHILE\\s+(.*?)\\s+LOOP([\\s\\S]*)", std::regex::icase);
        std::smatch while_match;
        if (std::regex_search(stmt, while_match, while_re)) {
            std::string condition = while_match[1].str();
            std::string trailing = trim(while_match[2].str());
            
            std::vector<std::string> inner_stmts;
            if (!trailing.empty()) inner_stmts.push_back(trailing);
            
            int depth = 1;
            while (++i < stmts.size()) {
                if (std::regex_search(stmts[i], std::regex("^(FOR|WHILE)\\s+.*LOOP", std::regex::icase))) depth++;
                else if (std::regex_search(stmts[i], std::regex("^END\\s+LOOP", std::regex::icase))) depth--;
                if (depth == 0) break;
                inner_stmts.push_back(stmts[i]);
            }
            
            while (true) {
                std::string exec_cond = condition;
                for (const auto& kv : vars) {
                    exec_cond = replaceAll(exec_cond, ":" + kv.first, kv.second);
                    exec_cond = replaceAll(exec_cond, "$" + kv.first, kv.second);
                    std::string escaped_arg = std::regex_replace(kv.first, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
                    exec_cond = std::regex_replace(exec_cond, std::regex("\\b" + escaped_arg + "\\b"), kv.second);
                }
                
                std::vector<std::string> cond_cols;
                std::vector<std::vector<std::string>> cond_rows;
                std::string cond_err;
                if (!engine->executeQuery("SELECT (" + exec_cond + ")", cond_cols, cond_rows, cond_err)) {
                    out_error_msg = "Error executing WHILE condition: " + cond_err;
                    return false;
                }
                
                bool cond_is_true = false;
                if (!cond_rows.empty() && !cond_rows[0].empty()) {
                    std::string val = cond_rows[0][0];
                    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                    if (val == "true" || val == "1" || val == "t" || val == "y" || val == "yes") {
                        cond_is_true = true;
                    }
                }
                
                if (!cond_is_true) break;
                
                std::string blk_err;
                std::vector<std::string> blk_cols;
                std::vector<std::vector<std::string>> blk_rows;
                if (!executeBlock(inner_stmts, engine, blk_cols, blk_rows, blk_err, vars, is_returned)) {
                    out_error_msg = blk_err;
                    return false;
                }
                if (!blk_cols.empty()) {
                    last_cols = blk_cols;
                    last_rows = blk_rows;
                }
                if (is_returned) {
                    out_col_names = last_cols;
                    out_rows = last_rows;
                    return true;
                }
            }
            continue;
        }

        // 1.2 IF / THEN / ELSE / END IF
        std::regex if_re("^IF\\s+(.*?)\\s+THEN([\\s\\S]*)", std::regex::icase);
        std::smatch if_match;
        if (std::regex_search(stmt, if_match, if_re)) {
            std::string condition = if_match[1].str();
            std::string trailing = trim(if_match[2].str());
            
            std::vector<std::string> true_stmts;
            std::vector<std::string> false_stmts;
            std::vector<std::string>* current_stmts = &true_stmts;
            
            if (!trailing.empty()) {
                if (std::regex_search(trailing, std::regex("^ELSE", std::regex::icase))) {
                    current_stmts = &false_stmts;
                    std::string else_trail = trim(std::regex_replace(trailing, std::regex("^ELSE", std::regex::icase), ""));
                    if (!else_trail.empty()) current_stmts->push_back(else_trail);
                } else {
                    current_stmts->push_back(trailing);
                }
            }
            
            int depth = 1;
            while (++i < stmts.size()) {
                if (std::regex_search(stmts[i], std::regex("^IF\\s+.*THEN", std::regex::icase))) {
                    depth++;
                } else if (std::regex_search(stmts[i], std::regex("^END\\s+IF", std::regex::icase))) {
                    depth--;
                } else if (depth == 1 && std::regex_search(stmts[i], std::regex("^ELSE", std::regex::icase))) {
                    current_stmts = &false_stmts;
                    std::string else_trail = trim(std::regex_replace(stmts[i], std::regex("^ELSE", std::regex::icase), ""));
                    if (!else_trail.empty()) current_stmts->push_back(else_trail);
                    continue;
                }
                
                if (depth == 0) break;
                current_stmts->push_back(stmts[i]);
            }
            
            std::string exec_cond = condition;
            for (const auto& kv : vars) {
                exec_cond = replaceAll(exec_cond, ":" + kv.first, kv.second);
                exec_cond = replaceAll(exec_cond, "$" + kv.first, kv.second);
                std::string escaped_arg = std::regex_replace(kv.first, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
                exec_cond = std::regex_replace(exec_cond, std::regex("\\b" + escaped_arg + "\\b"), kv.second);
            }
            
            std::vector<std::string> cond_cols;
            std::vector<std::vector<std::string>> cond_rows;
            std::string cond_err;
            if (!engine->executeQuery("SELECT (" + exec_cond + ")", cond_cols, cond_rows, cond_err)) {
                out_error_msg = "Error executing IF condition: " + cond_err;
                return false;
            }
            
            bool cond_is_true = false;
            if (!cond_rows.empty() && !cond_rows[0].empty()) {
                std::string val = cond_rows[0][0];
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                if (val == "true" || val == "1" || val == "t" || val == "y" || val == "yes") {
                    cond_is_true = true;
                }
            }
            
            std::vector<std::string>& stmts_to_exec = cond_is_true ? true_stmts : false_stmts;
            if (!stmts_to_exec.empty()) {
                std::string blk_err;
                std::vector<std::string> blk_cols;
                std::vector<std::vector<std::string>> blk_rows;
                if (!executeBlock(stmts_to_exec, engine, blk_cols, blk_rows, blk_err, vars, is_returned)) {
                    out_error_msg = blk_err;
                    return false;
                }
                if (!blk_cols.empty()) {
                    last_cols = blk_cols;
                    last_rows = blk_rows;
                }
                if (is_returned) {
                    out_col_names = last_cols;
                    out_rows = last_rows;
                    return true;
                }
            }
            continue;
        }

        // 1.3 BEGIN ... EXCEPTION ... END
        std::regex begin_re("^BEGIN([\\s\\S]*)", std::regex::icase);
        std::smatch begin_match;
        if (std::regex_search(stmt, begin_match, begin_re)) {
            std::string trailing = trim(begin_match[1].str());
            std::vector<std::string> try_stmts;
            std::vector<std::string> catch_stmts;
            bool in_catch = false;
            
            if (!trailing.empty()) try_stmts.push_back(trailing);
            
            int depth = 1;
            while (++i < stmts.size()) {
                if (std::regex_search(stmts[i], std::regex("^BEGIN", std::regex::icase))) {
                    depth++;
                } else if (std::regex_search(stmts[i], std::regex("^END", std::regex::icase))) {
                    if (!std::regex_search(stmts[i], std::regex("^END\\s+(IF|LOOP)", std::regex::icase))) {
                        depth--;
                    }
                } else if (depth == 1 && std::regex_search(stmts[i], std::regex("^EXCEPTION\\s+WHEN", std::regex::icase))) {
                    in_catch = true;
                    std::smatch ex_match;
                    std::regex_search(stmts[i], ex_match, std::regex("^EXCEPTION\\s+WHEN\\s+(.*?)\\s+THEN([\\s\\S]*)", std::regex::icase));
                    std::string catch_trail = trim(ex_match[2].str());
                    if (!catch_trail.empty()) catch_stmts.push_back(catch_trail);
                    continue;
                }
                
                if (depth == 0) break;
                
                if (in_catch && depth == 1) {
                    catch_stmts.push_back(stmts[i]);
                } else {
                    try_stmts.push_back(stmts[i]);
                }
            }
            
            std::string blk_err;
            std::vector<std::string> blk_cols;
            std::vector<std::vector<std::string>> blk_rows;
            if (!executeBlock(try_stmts, engine, blk_cols, blk_rows, blk_err, vars, is_returned)) {
                if (in_catch) {
                    vars["SQLERRM"] = blk_err;
                    std::string catch_err;
                    if (!executeBlock(catch_stmts, engine, blk_cols, blk_rows, catch_err, vars, is_returned)) {
                        out_error_msg = catch_err;
                        return false;
                    }
                } else {
                    out_error_msg = blk_err;
                    return false;
                }
            }
            if (!blk_cols.empty()) {
                last_cols = blk_cols;
                last_rows = blk_rows;
            }
            if (is_returned) {
                out_col_names = last_cols;
                out_rows = last_rows;
                return true;
            }
            continue;
        }

        // 1.4 RETURN statement
        std::regex return_re("^RETURN\\s+(.*)", std::regex::icase);
        std::smatch return_match;
        if (std::regex_search(stmt, return_match, return_re)) {
            std::string expr = trim(return_match[1].str());
            if (!expr.empty() && expr.back() == ';') expr.pop_back();
            
            for (const auto& kv : vars) {
                expr = replaceAll(expr, ":" + kv.first, kv.second);
                expr = replaceAll(expr, "$" + kv.first, kv.second);
                std::string escaped_arg = std::regex_replace(kv.first, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
                expr = std::regex_replace(expr, std::regex("\\b" + escaped_arg + "\\b"), kv.second);
            }
            
            std::vector<std::string> ret_cols;
            std::vector<std::vector<std::string>> ret_rows;
            std::string ret_err;
            if (!engine->executeQuery("SELECT (" + expr + ") AS return_value", ret_cols, ret_rows, ret_err)) {
                out_error_msg = "Error evaluating RETURN expression: " + ret_err;
                return false;
            }
            
            out_col_names = ret_cols;
            out_rows = ret_rows;
            is_returned = true;
            return true;
        }

        // 1.5 RAISE NOTICE / PRINT logging
        std::regex raise_re("^(?:RAISE\\s+NOTICE|PRINT)\\s+'(.*)'", std::regex::icase);
        std::smatch raise_match;
        if (std::regex_search(stmt, raise_match, raise_re)) {
            std::string msg = raise_match[1].str();
            for (const auto& kv : vars) {
                msg = replaceAll(msg, ":" + kv.first, kv.second);
                msg = replaceAll(msg, "$" + kv.first, kv.second);
                std::string escaped_arg = std::regex_replace(kv.first, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
                msg = std::regex_replace(msg, std::regex("\\b" + escaped_arg + "\\b"), kv.second);
            }
            
            std::string escape_msg = replaceAll(msg, "'", "''");
            DuckDbEngine meta_engine;
            if (meta_engine.connect("teal_catalog.db")) {
                std::string dummy_err;
                std::vector<std::string> dummy_cols;
                std::vector<std::vector<std::string>> dummy_rows;
                meta_engine.executeQuery("INSERT INTO duckflow_logs(log_time, message, routine_context) VALUES (current_timestamp, '" + escape_msg + "', 'duckflow_proc')", dummy_cols, dummy_rows, dummy_err);
            }
            continue;
        }
        
        // 2. Variable interpolation for standard statements
        std::string executed_sql = stmt;
        for (const auto& kv : vars) {
            std::string arg_name = kv.first;
            std::string arg_val = kv.second;
            
            executed_sql = replaceAll(executed_sql, ":" + arg_name, arg_val);
            executed_sql = replaceAll(executed_sql, "$" + arg_name, arg_val);
            
            std::string escaped_arg = std::regex_replace(arg_name, std::regex(R"([-[\]{}()*+?.,\^$|#\s])"), R"(\$&)");
            executed_sql = std::regex_replace(executed_sql, std::regex("\\b" + escaped_arg + "\\b"), arg_val);
        }
        
        // 3. EXECUTE dynamic SQL support
        if (startsWithIgnoreCase(executed_sql, "EXECUTE ")) {
            std::string dyn = executed_sql.substr(8);
            dyn = trim(dyn);
            // If it's a quoted string literal, strip quotes
            if (dyn.size() >= 2 && dyn.front() == '\'' && dyn.back() == '\'') {
                dyn = dyn.substr(1, dyn.size() - 2);
            }
            // Execute the dynamic string
            std::vector<std::string> dyn_cols;
            std::vector<std::vector<std::string>> dyn_rows;
            std::string err;
            if (!routeAndExecute(dyn, engine, dyn_cols, dyn_rows, err, &vars)) {
                out_error_msg = "Dynamic SQL error: " + err;
                return false;
            }
            last_cols = dyn_cols;
            last_rows = dyn_rows;
            continue;
        }
        
        std::vector<std::string> tmp_cols;
        std::vector<std::vector<std::string>> tmp_rows;
        std::string err;
        
        std::cout << "Executing procedure step: " << executed_sql << std::endl;
        
        if (!routeAndExecute(executed_sql, engine, tmp_cols, tmp_rows, err, &vars)) {
            out_error_msg = "Error in statement '" + executed_sql + "': " + err;
            return false;
        }
        
        if (!tmp_cols.empty()) {
            last_cols = tmp_cols;
            last_rows = tmp_rows;
        }
    }
    
    if (!last_cols.empty()) {
        out_col_names = last_cols;
        out_rows = last_rows;
    } else {
        out_col_names = {"status"};
        out_rows = {{"SUCCESS"}};
    }
    return true;
}

bool SQLRouter::handleAlterTenant(const std::string& sql, std::string& out_msg) {
    // Expected syntax: ALTER TENANT 'sk_abc_123' SET MEMORY='4GB', THREADS=4, QPS=100;
    std::regex re(R"(ALTER\s+TENANT\s+'([^']+)'\s+SET\s+MEMORY\s*=\s*'([^']+)',\s*THREADS\s*=\s*(\d+),\s*QPS\s*=\s*(\d+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(sql, match, re)) {
        std::string api_key = match[1].str();
        std::string memory = match[2].str();
        int threads = std::stoi(match[3].str());
        int qps = std::stoi(match[4].str());
        
        if (Catalog::getInstance().updateTenantLimits(api_key, memory, threads, qps)) {
            // Replicate the Governor limit update across the Raft Mesh!
            json payload;
            payload["api_key"] = api_key;
            payload["memory"] = memory;
            payload["threads"] = threads;
            payload["qps"] = qps;
            
            if (RaftEngine::getInstance().getState() == RaftState::LEADER) {
                RaftEngine::getInstance().propose("alter_tenant", payload.dump());
            }
            
            out_msg = "Governor constraints dynamically updated for tenant: " + api_key;
            return true;
        } else {
            out_msg = "Failed to update tenant constraints. Check if API Key exists.";
            return false;
        }
    }
    out_msg = "Syntax error. Expected: ALTER TENANT 'api_key' SET MEMORY='4GB', THREADS=4, QPS=100;";
    return false;
}

bool SQLRouter::handleCreateModel(const std::string& sql, std::string& out_msg) {
    // CREATE MODEL <name> TYPE <type> [WITH (endpoint='...', api_key='...')]
    std::regex model_re(R"(CREATE\s+(?:OR\s+REPLACE\s+)?MODEL\s+([a-zA-Z0-9_]+)\s+TYPE\s+([a-zA-Z0-9_]+)(?:\s+WITH\s*\((.*?)\))?)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(sql, match, model_re)) {
        std::string name = match[1].str();
        std::string type = match[2].str();
        std::string with_clause = match[3].str();
        
        std::string endpoint = "";
        std::string api_key = "";
        
        if (!with_clause.empty()) {
            std::regex endpoint_re(R"(endpoint\s*=\s*'([^']+)')", std::regex::icase);
            std::regex key_re(R"(api_key\s*=\s*'([^']+)')", std::regex::icase);
            
            std::smatch ematch, kmatch;
            if (std::regex_search(with_clause, ematch, endpoint_re)) endpoint = ematch[1].str();
            if (std::regex_search(with_clause, kmatch, key_re)) api_key = kmatch[1].str();
        }
        
        if (Catalog::getInstance().registerModel(name, type, endpoint, api_key)) {
            out_msg = "Model successfully registered: " + name;
            return true;
        } else {
            out_msg = "Failed to register model.";
            return false;
        }
    }
    out_msg = "Syntax error. Expected: CREATE MODEL <name> TYPE <type> [WITH (endpoint='...', api_key='...')];";
    return false;
}

bool SQLRouter::handleSystemQuery(const std::string& sql,
                                  ExecutionEngine* engine,
                                  std::vector<std::string>& out_col_names, 
                                  std::vector<std::vector<std::string>>& out_rows, 
                                  std::string& out_error_msg) {
    if (sql.find("teal_stat_activity") != std::string::npos || sql.find("pg_stat_activity") != std::string::npos) {
        out_col_names = {"session_id", "client_ip", "status", "query_start_time", "current_query"};
        auto sessions = StatActivityTracker::getInstance().getActiveSessions();
        for (const auto& s : sessions) {
            // Format start time
            auto time_t_val = std::chrono::system_clock::to_time_t(s.query_start_time);
            char time_str[64];
            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t_val));
            
            out_rows.push_back({
                std::to_string(s.session_id),
                s.client_ip,
                s.status,
                time_str,
                s.current_query
            });
        }
        return true;
    }
    
    if (sql.find("teal_jobs") != std::string::npos) {
        auto jobs = Catalog::getInstance().getJobs();
        out_col_names = {"id", "project", "name", "cron_schedule", "command", "status", "last_run", "next_run"};
        for (const auto& j : jobs) {
            out_rows.push_back({
                std::to_string(j.id),
                j.project_name,
                j.name,
                j.cron_schedule,
                j.command,
                j.status,
                j.last_run.empty() ? "NEVER" : j.last_run,
                j.next_run.empty() ? "N/A" : j.next_run
            });
        }
        return true;
    }

    if (sql.find("teal_job_history") != std::string::npos) {
        auto db_rows = Catalog::getInstance().query("SELECT jh.id, j.name, jh.status, jh.started_at, jh.finished_at, jh.duration_ms, jh.error_message FROM job_history jh JOIN jobs j ON jh.job_id = j.id ORDER BY jh.started_at DESC LIMIT 50;");
        out_col_names = {"id", "job_name", "status", "started_at", "finished_at", "duration_ms", "error_message"};
        for (const auto& r : db_rows) {
            out_rows.push_back({
                r.at("id"),
                r.at("name"),
                r.at("status"),
                r.at("started_at"),
                r.at("finished_at"),
                r.at("duration_ms"),
                r.at("error_message")
            });
        }
        return true;
    }
    
    if (sql.find("teal_routines") != std::string::npos) {
        auto& cache = Catalog::getInstance().getRoutinesCache();
        out_col_names = {"project", "schema", "name", "type", "arguments", "return_type", "language", "created_at"};
        for (const auto& kv : cache) {
            const auto& r = kv.second;
            out_rows.push_back({
                r.project_name,
                r.schema_name,
                r.name,
                r.type,
                r.args,
                r.return_type,
                r.language,
                r.created_at
            });
        }
        return true;
    }
    
    if (sql.find("teal_logs") != std::string::npos) {
        std::string internal_sql = "SELECT * FROM duckflow_catalog.duckflow_logs";
        if (engine) {
            return engine->executeQuery(internal_sql, out_col_names, out_rows, out_error_msg);
        } else {
            out_error_msg = "No engine provided to execute teal_logs.";
            return false;
        }
    }
    
    if (sql.find("teal_models") != std::string::npos) {
        auto models = Catalog::getInstance().getModels();
        out_col_names = {"id", "name", "type", "endpoint", "api_key", "created_at"};
        for (const auto& m : models) {
            out_rows.push_back({
                std::to_string(m.id),
                m.name,
                m.type,
                m.endpoint,
                m.api_key.empty() ? "" : "********",
                m.created_at
            });
        }
        return true;
    }

    out_error_msg = "Unknown system query.";
    return false;
}
