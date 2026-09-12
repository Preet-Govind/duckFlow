#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sqlite3.h>

struct CatalogRoutine {
    std::string project_name;
    std::string schema_name;
    std::string name;
    std::string type;
    std::string args;
    std::string return_type;
    std::string body;
    std::string language;
    std::string created_at;
};

struct CatalogJob {
    int id;
    std::string project_name;
    std::string name;
    std::string cron_schedule;
    std::string command;
    std::string status;
    std::string last_run;
    std::string next_run;
};

struct CatalogTenant {
    int id;
    std::string api_key;
    std::string db_path;
    std::string memory_limit;
    int thread_limit;
    int qps_limit;
    std::string created_at;
};

struct CatalogFlow {
    int id;
    std::string project_name;
    std::string name;
    std::string definition_json;
    std::string created_at;
};

struct CatalogModel {
    int id;
    std::string name;
    std::string type;
    std::string endpoint;
    std::string api_key;
    std::string created_at;
};

class Catalog {
public:
    static Catalog& getInstance() {
        static Catalog instance;
        return instance;
    }

    bool init(const std::string& db_path);
    void close();

    bool execute(const std::string& sql);
    std::vector<std::unordered_map<std::string, std::string>> query(const std::string& sql);

    void loadCache();
    const std::unordered_map<std::string, CatalogRoutine>& getRoutinesCache() const { return routines_cache_; }



    // Projects & Schemas
    bool createProject(const std::string& name, const std::string& config = "{}");
    bool createSchema(const std::string& project_name, const std::string& schema_name);

    // Models
    bool registerModel(const std::string& name, const std::string& type, const std::string& endpoint, const std::string& api_key);
    std::vector<CatalogModel> getModels();
    bool getModel(const std::string& name, CatalogModel& out_model);

    // System Views (Proxy functions)
    bool registerRoutine(const std::string& project_name, 
                          const std::string& schema_name, 
                          const std::string& name, 
                          const std::string& type, // "PROCEDURE" or "FUNCTION"
                          const std::string& args, 
                          const std::string& return_type, 
                          const std::string& body, 
                          const std::string& language = "SQL");

    bool getRoutine(const std::string& project_name,
                    const std::string& schema_name,
                    const std::string& name,
                    std::string& type,
                    std::string& args,
                    std::string& return_type,
                    std::string& body,
                    std::string& language);

    // Jobs
    bool registerJob(const std::string& project_name, 
                     const std::string& name, 
                     const std::string& cron_expr, 
                     const std::string& command);
    std::vector<CatalogJob> getJobs();
    bool toggleJobStatus(int job_id, const std::string& new_status);
    bool logJobStart(int job_id, int64_t& out_history_id);
    bool logJobEnd(int64_t history_id, const std::string& status, int64_t duration_ms, const std::string& error_msg, const std::string& logs);
    bool appendJobLog(int64_t history_id, const std::string& new_logs);
    bool getJobLog(int64_t history_id, std::string& out_logs, std::string& out_status);
    bool updateJobNextRun(int job_id, const std::string& next_run_time);

    // Flows & DAG Orchestration
    bool registerFlow(const std::string& project_name, const std::string& name, const std::string& definition_json);
    bool getFlow(const std::string& project_name, const std::string& name, std::string& out_definition_json);
    std::vector<std::unordered_map<std::string, std::string>> getFlows();
    bool logFlowRunStart(const std::string& flow_name, int64_t& out_run_id);
    bool logFlowRunEnd(int64_t run_id, const std::string& status, int64_t duration_ms, const std::string& error_msg);
    bool logFlowStepStart(int64_t run_id, const std::string& step_name, int64_t& out_step_run_id);
    bool logFlowStepEnd(int64_t step_run_id, const std::string& status, int64_t duration_ms, const std::string& logs, const std::string& error_msg);
    std::vector<std::unordered_map<std::string, std::string>> getFlowRuns(int limit = 50);
    std::vector<std::unordered_map<std::string, std::string>> getFlowStepRuns(int64_t run_id);

    // Connections & Secrets
    bool createConnection(const std::string& project_name, const std::string& name, const std::string& type, const std::string& config);
    bool createSecret(const std::string& project_name, const std::string& name, const std::string& value);
    std::string getSecret(const std::string& project_name, const std::string& name);
    
    // Auth & Tenants
    bool getTenantDbPath(const std::string& api_key, std::string& db_path, std::string& mem_limit, int& threads, int& qps);
    bool createTenant(const std::string& api_key, const std::string& db_path);
    bool updateTenantLimits(const std::string& api_key, const std::string& memory, int threads, int qps);
    std::vector<CatalogTenant> getTenants();
    bool deleteTenant(const std::string& api_key);

private:
    Catalog() = default;
    ~Catalog() { close(); }
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    sqlite3* db_ = nullptr;
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, CatalogRoutine> routines_cache_;
};
