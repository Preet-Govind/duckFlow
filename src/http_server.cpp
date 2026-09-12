#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "http_server.h"
#include "sproc.h"
#include "catalog.h"
#include "stat_activity.h"
#include "duckdb_engine.h"
#include "flow_engine.h"
#include "raft.h"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

// Fallback HTML page embedded in the binary
const char* FALLBACK_DASHBOARD_HTML = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Teal: Control Plane Console</title>
    <style>
        body { font-family: sans-serif; background-color: #0d0e12; color: #fff; text-align: center; padding-top: 100px; }
        h1 { color: #00e5ff; }
        p { color: #a0aec0; }
    </style>
</head>
<body>
    <h1>Teal Web Dashboard Fallback</h1>
    <p>Please load the proper index.html file in the web/ directory.</p>
</body>
</html>
)rawhtml";

void HttpServer::start(int port, const std::string& web_dir, ExecutionEngine* engine) {
    port_ = port;
    web_dir_ = web_dir;
    engine_ = engine;
    running_ = true;

    thread_ = std::thread(&HttpServer::runLoop, this);
    std::cout << "HTTP Dashboard Server listening on port " << port_ << "..." << std::endl;
}

void HttpServer::stop() {
    running_ = false;
    if (svr_ptr_) {
        static_cast<httplib::Server*>(svr_ptr_)->stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HttpServer::runLoop() {
    httplib::Server svr;
    svr_ptr_ = &svr;

    // Static files or fallback index.html
    svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        std::string html_path = web_dir_ + "/index.html";
        std::ifstream file(html_path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            res.set_content(buffer.str(), "text/html");
        } else {
            res.set_content(FALLBACK_DASHBOARD_HTML, "text/html");
        }
    });

    svr.Get("/index.html", [this](const httplib::Request&, httplib::Response& res) {
        std::string html_path = web_dir_ + "/index.html";
        std::ifstream file(html_path);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            res.set_content(buffer.str(), "text/html");
        } else {
            res.set_content(FALLBACK_DASHBOARD_HTML, "text/html");
        }
    });

    svr.Get("/duckflow_logo.png", [this](const httplib::Request&, httplib::Response& res) {
        std::string path = web_dir_ + "/duckflow_logo.png";
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            res.set_content(buffer.str(), "image/png");
        } else {
            res.status = 404;
        }
    });

    // POST /api/query
    svr.Post("/api/query", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            if (!body.contains("query")) {
                res.status = 400;
                res.set_content("{\"status\":\"error\",\"error\":\"Missing query field\"}", "application/json");
                return;
            }

            std::string sql = body["query"];
            std::vector<std::string> cols;
            std::vector<std::vector<std::string>> rows;
            std::string err_msg;

            // [MULTI-TENANT ISOLATION]
            // If the request contains an Authorization Bearer token, we map the user 
            // strictly to their private tenant database file. Otherwise, we default to the public admin DB.
            ExecutionEngine* target_engine = engine_;
            DuckDbEngine tenant_engine;
            
            std::string auth_header = req.has_header("Authorization") ? req.get_header_value("Authorization") : "";
            if (auth_header.find("Bearer ") == 0) {
                std::string api_key = auth_header.substr(7);
                std::string tenant_db;
                std::string mem_limit;
                int threads, qps_limit;
                
                if (Catalog::getInstance().getTenantDbPath(api_key, tenant_db, mem_limit, threads, qps_limit)) {
                    // === RESOURCE GOVERNOR: QPS RATE LIMITING ===
                    {
                        auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> rl_lock(rate_limit_mutex_);
                        auto& tracker = rate_limits_[api_key];
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - tracker.last_reset).count() >= 1) {
                            tracker.last_reset = now;
                            tracker.queries_this_second = 0;
                        }
                        if (tracker.queries_this_second >= qps_limit) {
                            res.status = 429;
                            res.set_content("{\"status\":\"error\",\"error\":\"Governor Exception: 429 Too Many Requests. Tenant exceeded QPS Limit (" + std::to_string(qps_limit) + ").\"}", "application/json");
                            return;
                        }
                        tracker.queries_this_second++;
                    }

                    if (tenant_engine.connect(tenant_db)) {
                        target_engine = &tenant_engine;
                        
                        // === RESOURCE GOVERNOR: MEMORY & THREAD SANDBOXING ===
                        std::vector<std::string> dummy_c;
                        std::vector<std::vector<std::string>> dummy_r;
                        std::string dummy_e;
                        SQLRouter::routeAndExecute("PRAGMA memory_limit='" + mem_limit + "';", target_engine, dummy_c, dummy_r, dummy_e);
                        SQLRouter::routeAndExecute("PRAGMA threads=" + std::to_string(threads) + ";", target_engine, dummy_c, dummy_r, dummy_e);
                    }
                } else {
                    res.status = 401;
                    res.set_content("{\"status\":\"error\",\"error\":\"Unauthorized: Invalid Tenant API Key\"}", "application/json");
                    return;
                }
            }

            bool ok = SQLRouter::routeAndExecute(sql, target_engine, cols, rows, err_msg);

            // [CLUSTER REPLICATION]
            // If this node is the Leader, and the SQL command modifies Global State 
            // (e.g. creating Jobs or Routines), broadcast it to the Follower mesh!
            if (ok && RaftEngine::getInstance().getState() == RaftState::LEADER) {
                std::string upper_sql = sql;
                std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);
                
                if (upper_sql.find("CREATE ") == 0 || upper_sql.find("DROP ") == 0 || upper_sql.find("ALTER ") == 0) {
                    json payload;
                    payload["sql"] = sql;
                    RaftEngine::getInstance().propose("execute_sql", payload.dump());
                }
            }

            json response;
            if (ok) {
                response["status"] = "success";
                response["columns"] = cols;
                response["rows"] = rows;
            } else {
                response["status"] = "error";
                response["error"] = err_msg;
            }
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            json response;
            response["status"] = "error";
            response["error"] = std::string("Internal Server Error: ") + e.what();
            res.set_content(response.dump(), "application/json");
        }
    });

    // GET /api/jobs
    svr.Get("/api/jobs", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto jobs = Catalog::getInstance().getJobs();
        json arr = json::array();
        for (const auto& j : jobs) {
            json item;
            item["id"] = j.id;
            item["project"] = j.project_name;
            item["name"] = j.name;
            item["cron_schedule"] = j.cron_schedule;
            item["command"] = j.command;
            item["status"] = j.status;
            item["last_run"] = j.last_run;
            item["next_run"] = j.next_run;
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // POST /api/jobs/toggle
    svr.Post("/api/jobs/toggle", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            int job_id = body.value("id", 0);
            std::string status = body.value("status", "ENABLED");
            
            if (Catalog::getInstance().toggleJobStatus(job_id, status)) {
                // Replicate to the cluster!
                RaftEngine::getInstance().propose("toggle_job", req.body);
                res.set_content("{\"status\":\"success\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"status\":\"error\",\"error\":\"Failed to update database\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
        }
    });

    // GET /api/tenants
    svr.Get("/api/tenants", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto tenants = Catalog::getInstance().getTenants();
        json arr = json::array();
        for (const auto& t : tenants) {
            json item;
            item["id"] = t.id;
            item["api_key"] = t.api_key;
            item["db_path"] = t.db_path;
            item["memory_limit"] = t.memory_limit;
            item["thread_limit"] = t.thread_limit;
            item["qps_limit"] = t.qps_limit;
            item["created_at"] = t.created_at;
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // POST /api/tenants
    svr.Post("/api/tenants", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            std::string prefix = body.value("prefix", "usr");
            std::string db_path = body.value("db_path", "");
            std::string role = body.value("role", "read_write");

            if (db_path.empty()) {
                res.status = 400;
                res.set_content("{\"status\":\"error\",\"error\":\"db_path required\"}", "application/json");
                return;
            }

            if (role == "read_only") {
                db_path += "?access_mode=READ_ONLY";
            }

            // Generate secure token
            srand(time(NULL) + clock());
            std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            std::string token = "sk_" + prefix + "_";
            for (int i = 0; i < 32; ++i) token += chars[rand() % chars.length()];

            if (Catalog::getInstance().createTenant(token, db_path)) {
                // Eagerly instantiate the physical database on SSD
                try {
                    std::filesystem::path p(db_path);
                    if (p.has_parent_path()) {
                        std::filesystem::create_directories(p.parent_path());
                    }
                    DuckDbEngine eager_engine;
                    eager_engine.connect(db_path);
                    eager_engine.disconnect();
                } catch (...) {
                    std::cout << "Warning: Failed to eagerly instantiate physical sandbox." << std::endl;
                }

                // Replicate!
                json payload = {{"token", token}, {"db_path", db_path}};
                RaftEngine::getInstance().propose("create_tenant", payload.dump());
                res.set_content("{\"status\":\"success\",\"api_key\":\"" + token + "\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"status\":\"error\",\"error\":\"Failed to create tenant\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
        }
    });

    // DELETE /api/tenants
    svr.Delete("/api/tenants", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            std::string api_key = body.value("api_key", "");
            if (Catalog::getInstance().deleteTenant(api_key)) {
                // Replicate!
                json payload = {{"api_key", api_key}};
                RaftEngine::getInstance().propose("delete_tenant", payload.dump());
                res.set_content("{\"status\":\"success\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"status\":\"error\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
        }
    });

    // GET /api/logs/stream
    svr.Get("/api/logs/stream", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("history_id")) {
            res.status = 400;
            return;
        }
        int64_t history_id = std::stoll(req.get_param_value("history_id"));

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");

        auto last_pos = std::make_shared<size_t>(0);
        res.set_chunked_content_provider(
            "text/event-stream",
            [history_id, last_pos](size_t /*offset*/, httplib::DataSink &sink) {
                std::string logs, status;
                if (!Catalog::getInstance().getJobLog(history_id, logs, status)) {
                    sink.write("data: {\"status\":\"ERROR\", \"log\":\"Job history not found.\"}\n\n", 57);
                    sink.done();
                    return false; // Close stream
                }

                bool is_running = status.find("RUNNING") != std::string::npos;
                
                // If new logs were appended or job just finished, stream the payload!
                if (logs.length() > *last_pos || !is_running) {
                    std::string new_chunk = logs.substr(*last_pos);
                    *last_pos = logs.length();
                    
                    json payload;
                    payload["status"] = status;
                    payload["log"] = new_chunk;
                    payload["eof"] = !is_running;
                    
                    std::string msg = "data: " + payload.dump() + "\n\n";
                    if (!sink.write(msg.c_str(), msg.length())) return false;
                    
                    if (!is_running) {
                        // Allow a tiny delay for network buffer to flush before gracefully closing
                        sink.done();
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        return false; 
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                return true; // Keep streaming!
            }
        );
    });

    // GET /api/job-history
    svr.Get("/api/job-history", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto db_rows = Catalog::getInstance().query(
            "SELECT jh.id, j.name, jh.status, jh.started_at, jh.finished_at, jh.duration_ms, jh.error_message, jh.logs "
            "FROM job_history jh JOIN jobs j ON jh.job_id = j.id "
            "ORDER BY jh.started_at DESC LIMIT 50;"
        );

        json arr = json::array();
        for (const auto& r : db_rows) {
            json item;
            item["id"] = std::stoi(r.at("id"));
            item["job_name"] = r.at("name");
            item["status"] = r.at("status");
            item["started_at"] = r.at("started_at");
            item["finished_at"] = r.at("finished_at");
            item["duration_ms"] = r.at("duration_ms") == "NULL" ? 0 : std::stoi(r.at("duration_ms"));
            item["error_message"] = r.at("error_message") == "NULL" ? "" : r.at("error_message");
            item["logs"] = r.at("logs") == "NULL" ? "" : r.at("logs");
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // GET /api/stat-activity
    svr.Get("/api/stat-activity", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto& tracker = StatActivityTracker::getInstance();
        auto sessions = tracker.getActiveSessions();
        
        json arr = json::array();
        for (const auto& s : sessions) {
            json item;
            item["session_id"] = s.session_id;
            item["client_ip"] = s.client_ip;
            item["status"] = s.status;
            
            auto time_t_val = std::chrono::system_clock::to_time_t(s.query_start_time);
            char time_str[64];
            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t_val));
            item["query_start_time"] = time_str;
            item["current_query"] = s.current_query;
            arr.push_back(item);
        }
        
        json root;
        root["sessions"] = arr;
        root["totals"]["queries"] = tracker.getTotalQueries();
        root["totals"]["failed"] = tracker.getTotalFailed();
        
        res.set_content(root.dump(), "application/json");
    });
    
    // POST /api/stat-activity/kill
    svr.Post("/api/stat-activity/kill", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            if (!body.contains("session_id")) {
                res.status = 400;
                res.set_content("{\"status\":\"error\",\"error\":\"Missing session_id\"}", "application/json");
                return;
            }
            int session_id = body["session_id"];
            bool killed = StatActivityTracker::getInstance().killSession(session_id);
            if (killed) {
                res.set_content("{\"status\":\"success\"}", "application/json");
            } else {
                res.status = 404;
                res.set_content("{\"status\":\"error\",\"error\":\"Session not found or already idle\"}", "application/json");
            }
        } catch (...) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"Invalid JSON\"}", "application/json");
        }
    });

    // GET /api/routines
    svr.Get("/api/routines", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto db_rows = Catalog::getInstance().query(
            "SELECT project_name, schema_name, name, type, args, return_type, language, body, created_at FROM routines;"
        );
        json arr = json::array();
        for (const auto& r : db_rows) {
            json item;
            item["project"] = r.at("project_name");
            item["schema"] = r.at("schema_name");
            item["name"] = r.at("name");
            item["type"] = r.at("type");
            item["arguments"] = r.at("args");
            item["return_type"] = r.at("return_type");
            item["language"] = r.at("language");
            item["body"] = r.at("body");
            item["created_at"] = r.at("created_at");
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // GET /api/flows
    svr.Get("/api/flows", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto flows = Catalog::getInstance().getFlows();
        json arr = json::array();
        for (const auto& f : flows) {
            json item;
            item["id"] = f.at("id");
            item["project"] = f.at("project_name");
            item["name"] = f.at("name");
            item["definition_json"] = f.at("definition_json");
            item["created_at"] = f.at("created_at");
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // POST /api/flows/run
    svr.Post("/api/flows/run", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto body = json::parse(req.body);
            std::string flow_name = body.value("name", "");
            std::string project_name = body.value("project", "default");

            if (flow_name.empty()) {
                res.status = 400;
                res.set_content("{\"status\":\"error\",\"error\":\"Flow name is required\"}", "application/json");
                return;
            }

            // Spawn execution in background thread
            std::thread([this, project_name, flow_name]() {
                int64_t run_id = 0;
                std::string err;
                FlowEngine::getInstance().executeFlow(project_name, flow_name, engine_, run_id, err);
            }).detach();

            res.set_content("{\"status\":\"success\",\"message\":\"Flow execution triggered asynchronously\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"Invalid JSON body\"}", "application/json");
        }
    });

    // GET /api/flows/runs
    svr.Get("/api/flows/runs", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto runs = Catalog::getInstance().getFlowRuns(50);
        json arr = json::array();
        for (const auto& r : runs) {
            json item;
            item["id"] = std::stoi(r.at("id"));
            item["flow_name"] = r.at("flow_name");
            item["status"] = r.at("status");
            item["started_at"] = r.at("started_at");
            item["finished_at"] = r.at("finished_at");
            item["duration_ms"] = r.at("duration_ms") == "NULL" ? 0 : std::stoi(r.at("duration_ms"));
            item["error_message"] = r.at("error_message");
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // GET /api/flows/step-runs
    svr.Get("/api/flows/step-runs", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!req.has_param("run_id")) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"run_id parameter missing\"}", "application/json");
            return;
        }

        int64_t run_id = std::stoll(req.get_param_value("run_id"));
        auto steps = Catalog::getInstance().getFlowStepRuns(run_id);
        json arr = json::array();
        for (const auto& s : steps) {
            json item;
            item["id"] = std::stoi(s.at("id"));
            item["run_id"] = std::stoi(s.at("run_id"));
            item["step_name"] = s.at("step_name");
            item["status"] = s.at("status");
            item["started_at"] = s.at("started_at");
            item["finished_at"] = s.at("finished_at");
            item["duration_ms"] = s.at("duration_ms") == "NULL" ? 0 : std::stoi(s.at("duration_ms"));
            item["logs"] = s.at("logs");
            item["error_message"] = s.at("error_message");
            arr.push_back(item);
        }
        res.set_content(arr.dump(), "application/json");
    });

    // Raft HTTP API for Cluster Consensus
    svr.Post("/api/raft/append_entries", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string action = body.value("action", "");
            
            if (action == "toggle_job") {
                auto data = body["data"];
                int job_id = data.value("id", 0);
                std::string status = data.value("status", "ENABLED");
                Catalog::getInstance().toggleJobStatus(job_id, status);
                std::cout << "Raft Follower: Replicated toggle_job for job_id " << job_id << " to " << status << std::endl;
            } else if (action == "execute_sql") {
                std::string sql = body["data"].value("sql", "");
                std::vector<std::string> cols;
                std::vector<std::vector<std::string>> rows;
                std::string err;
                
                // Deterministically replay the same SQL payload locally!
                SQLRouter::routeAndExecute(sql, engine_, cols, rows, err);
                std::cout << "Raft Follower: Synchronized SQL payload from Leader: " << sql << std::endl;
            } else if (action == "create_tenant") {
                std::string token = body["data"].value("token", "");
                std::string db_path = body["data"].value("db_path", "");
                Catalog::getInstance().createTenant(token, db_path);
                std::cout << "Raft Follower: Replicated create_tenant for " << token << std::endl;
            } else if (action == "delete_tenant") {
                std::string api_key = body["data"].value("api_key", "");
                Catalog::getInstance().deleteTenant(api_key);
                std::cout << "Raft Follower: Replicated delete_tenant for " << api_key << std::endl;
            } else if (action == "alter_tenant") {
                std::string api_key = body["data"].value("api_key", "");
                std::string memory = body["data"].value("memory", "");
                int threads = body["data"].value("threads", 1);
                int qps = body["data"].value("qps", 5);
                Catalog::getInstance().updateTenantLimits(api_key, memory, threads, qps);
                std::cout << "Raft Follower: Replicated alter_tenant limits for " << api_key << std::endl;
            }
            
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (...) {
            res.status = 500;
        }
    });

    svr.Post("/api/raft/join", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string peer = body.value("peer", "");
            if (!peer.empty()) {
                RaftEngine::getInstance().addPeer(peer);
            }
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (...) {
            res.status = 500;
        }
    });

    svr.Post("/api/raft/vote", [](const httplib::Request& req, httplib::Response& res) {
        // Handle election vote requests
        res.set_content("{\"vote_granted\":true}", "application/json");
    });
    
    svr.Get("/api/models", [](const httplib::Request&, httplib::Response& res) {
        auto models = Catalog::getInstance().getModels();
        auto arr = json::array();
        for (const auto& m : models) {
            arr.push_back({
                {"id", m.id},
                {"name", m.name},
                {"type", m.type},
                {"endpoint", m.endpoint},
                {"has_key", !m.api_key.empty()},
                {"created_at", m.created_at}
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    // Start listening
    svr.listen("0.0.0.0", port_);
}
