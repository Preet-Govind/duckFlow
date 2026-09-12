#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "engine.h"
#include "json.hpp"

using json = nlohmann::json;

struct FlowStep {
    std::string name;
    std::vector<std::string> dependencies;
    std::string command;
};

struct FlowDefinition {
    std::string name;
    std::string project_name;
    std::vector<FlowStep> steps;
};

class FlowEngine {
public:
    static FlowEngine& getInstance() {
        static FlowEngine instance;
        return instance;
    }

    // Parse CREATE FLOW DDL into FlowDefinition and JSON string
    bool parseCreateFlowDDL(const std::string& ddl, FlowDefinition& out_flow, std::string& out_json_str, std::string& out_err);

    // Validate DAG for cycles
    bool validateDAG(const FlowDefinition& flow, std::string& out_err);

    // Execute flow asynchronously or synchronously
    bool executeFlow(const std::string& project_name, const std::string& flow_name, ExecutionEngine* engine, int64_t& out_run_id, std::string& out_err);

private:
    FlowEngine() = default;
    ~FlowEngine() = default;
    FlowEngine(const FlowEngine&) = delete;
    FlowEngine& operator=(const FlowEngine&) = delete;

    static std::string trim(const std::string& str);
};
