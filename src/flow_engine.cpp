#include "flow_engine.h"
#include "catalog.h"
#include "sproc.h"
#include <iostream>
#include <sstream>
#include <queue>
#include <thread>
#include <chrono>
#include <algorithm>

std::string FlowEngine::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

bool FlowEngine::parseCreateFlowDDL(const std::string& ddl, FlowDefinition& out_flow, std::string& out_json_str, std::string& out_err) {
    std::string sql = trim(ddl);
    if (sql.back() == ';') sql.pop_back();

    // Syntax: CREATE FLOW <flow_name> STEP <step1> DEPENDS ON (<deps>) AS '<command>' ...
    std::stringstream ss(sql);
    std::string token;
    ss >> token; // CREATE
    ss >> token; // FLOW

    std::string flow_name;
    ss >> flow_name;
    if (flow_name.empty()) {
        out_err = "Missing flow name in CREATE FLOW";
        return false;
    }

    out_flow.name = flow_name;
    out_flow.project_name = "default";

    // Read remaining body
    std::string remaining;
    std::getline(ss, remaining, '\0');
    remaining = trim(remaining);

    json steps_json = json::array();

    // Parse step blocks iteratively
    size_t pos = 0;
    while (pos < remaining.size()) {
        size_t step_pos = remaining.find("STEP ", pos);
        if (step_pos == std::string::npos) {
            // Case insensitive check
            std::string upper_rem = remaining;
            std::transform(upper_rem.begin(), upper_rem.end(), upper_rem.begin(), ::toupper);
            step_pos = upper_rem.find("STEP ", pos);
            if (step_pos == std::string::npos) break;
        }

        pos = step_pos + 5; // skip "STEP "
        
        std::string upper_rem = remaining;
        std::transform(upper_rem.begin(), upper_rem.end(), upper_rem.begin(), ::toupper);
        
        // Find AS
        size_t as_pos = upper_rem.find(" AS ", pos);
        if (as_pos == std::string::npos) {
            as_pos = upper_rem.find("\tAS ", pos);
        }
        if (as_pos == std::string::npos) {
            as_pos = upper_rem.find("\nAS ", pos);
        }
        if (as_pos == std::string::npos) {
            out_err = "Missing ' AS ' clause for step near position " + std::to_string(pos);
            return false;
        }
        as_pos += 1; // point to 'A' (since we searched for " AS " or "\tAS " or "\nAS ")

        // Find DEPENDS ON
        size_t dep_pos = upper_rem.find(" DEPENDS ON ", pos);
        if (dep_pos == std::string::npos) dep_pos = upper_rem.find("\tDEPENDS ON ", pos);
        if (dep_pos == std::string::npos) dep_pos = upper_rem.find("\nDEPENDS ON ", pos);
        if (dep_pos != std::string::npos) dep_pos += 1; // point to 'D'

        std::string step_name;
        std::vector<std::string> deps;

        if (dep_pos != std::string::npos && dep_pos < as_pos) {
            // Has DEPENDS ON
            step_name = trim(remaining.substr(pos, dep_pos - pos));
            pos = dep_pos + 10; // skip "DEPENDS ON"
            
            // Dependencies inside (...)
            size_t open_paren = remaining.find("(", pos);
            size_t close_paren = remaining.find(")", open_paren);
            if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren > as_pos) {
                out_err = "Malformed DEPENDS ON: missing parenthesis for step " + step_name;
                return false;
            }

            std::string deps_str = trim(remaining.substr(open_paren + 1, close_paren - open_paren - 1));
            if (!deps_str.empty()) {
                std::stringstream dep_ss(deps_str);
                std::string dep_item;
                while (std::getline(dep_ss, dep_item, ',')) {
                    dep_item = trim(dep_item);
                    if (!dep_item.empty()) deps.push_back(dep_item);
                }
            }
            pos = close_paren + 1;
        } else {
            // No DEPENDS ON
            step_name = trim(remaining.substr(pos, as_pos - pos));
            pos = as_pos;
        }

        pos = as_pos + 2;
        while (pos < remaining.size() && (remaining[pos] == ' ' || remaining[pos] == '\t')) pos++;

        std::string command;
        if (pos < remaining.size() && remaining[pos] == '$') {
            // Dollar quoting: $$ command $$ or $tag$ command $tag$
            size_t next_dollar = remaining.find('$', pos + 1);
            if (next_dollar == std::string::npos) {
                out_err = "Malformed dollar quote tag for step " + step_name;
                return false;
            }
            std::string tag = remaining.substr(pos, next_dollar - pos + 1);
            pos = next_dollar + 1;
            size_t end_tag = remaining.find(tag, pos);
            if (end_tag == std::string::npos) {
                out_err = "Unterminated dollar quote tag '" + tag + "' for step " + step_name;
                return false;
            }
            command = remaining.substr(pos, end_tag - pos);
            pos = end_tag + tag.size();
        } else if (pos < remaining.size() && (remaining[pos] == '\'' || remaining[pos] == '"')) {
            char quote_char = remaining[pos];
            pos++; // skip opening quote
            while (pos < remaining.size()) {
                if (remaining[pos] == quote_char) {
                    if (pos + 1 < remaining.size() && remaining[pos + 1] == quote_char) {
                        // Escaped quote ('' or "")
                        command += quote_char;
                        pos += 2;
                    } else {
                        // Closing quote
                        pos++;
                        break;
                    }
                } else {
                    command += remaining[pos];
                    pos++;
                }
            }
        } else {
            out_err = "Expected quoted string command ('' or $$...$$) after AS for step " + step_name;
            return false;
        }

        FlowStep step;
        step.name = step_name;
        step.dependencies = deps;
        step.command = command;
        out_flow.steps.push_back(step);

        json step_j;
        step_j["name"] = step_name;
        step_j["dependencies"] = deps;
        step_j["command"] = command;
        steps_json.push_back(step_j);
    }

    if (out_flow.steps.empty()) {
        out_err = "No steps defined in CREATE FLOW";
        return false;
    }

    json root;
    root["name"] = out_flow.name;
    root["project_name"] = out_flow.project_name;
    root["steps"] = steps_json;
    out_json_str = root.dump(2);

    return validateDAG(out_flow, out_err);
}

bool FlowEngine::validateDAG(const FlowDefinition& flow, std::string& out_err) {
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> graph;
    std::set<std::string> all_steps;

    for (const auto& step : flow.steps) {
        all_steps.insert(step.name);
        if (in_degree.find(step.name) == in_degree.end()) {
            in_degree[step.name] = 0;
        }
    }

    for (const auto& step : flow.steps) {
        for (const auto& dep : step.dependencies) {
            if (all_steps.find(dep) == all_steps.end()) {
                out_err = "Step '" + step.name + "' depends on non-existent step '" + dep + "'";
                return false;
            }
            graph[dep].push_back(step.name);
            in_degree[step.name]++;
        }
    }

    std::queue<std::string> q;
    for (const auto& kv : in_degree) {
        if (kv.second == 0) q.push(kv.first);
    }

    int visited = 0;
    while (!q.empty()) {
        std::string current = q.front();
        q.pop();
        visited++;

        for (const auto& neighbor : graph[current]) {
            in_degree[neighbor]--;
            if (in_degree[neighbor] == 0) {
                q.push(neighbor);
            }
        }
    }

    if (visited != static_cast<int>(all_steps.size())) {
        out_err = "Cyclic dependency detected in FLOW '" + flow.name + "'! Not a valid DAG.";
        return false;
    }

    return true;
}

bool FlowEngine::executeFlow(const std::string& project_name, const std::string& flow_name, ExecutionEngine* engine, int64_t& out_run_id, std::string& out_err) {
    std::string definition_json;
    if (!Catalog::getInstance().getFlow(project_name, flow_name, definition_json)) {
        out_err = "Flow '" + flow_name + "' not found in catalog for project '" + project_name + "'";
        return false;
    }

    json root;
    try {
        root = json::parse(definition_json);
    } catch (const std::exception& e) {
        out_err = "Failed to parse flow definition JSON: " + std::string(e.what());
        return false;
    }

    FlowDefinition flow;
    flow.name = flow_name;
    flow.project_name = project_name;

    for (const auto& step_j : root["steps"]) {
        FlowStep s;
        s.name = step_j["name"].get<std::string>();
        s.command = step_j["command"].get<std::string>();
        for (const auto& d : step_j["dependencies"]) {
            s.dependencies.push_back(d.get<std::string>());
        }
        flow.steps.push_back(s);
    }

    // Start Flow Run Logging
    if (!Catalog::getInstance().logFlowRunStart(flow_name, out_run_id)) {
        out_err = "Failed to log flow run start in database catalog";
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();

    // Map step status & dependencies
    std::unordered_map<std::string, FlowStep> step_map;
    std::unordered_map<std::string, std::vector<std::string>> graph; // parent -> children
    std::unordered_map<std::string, std::set<std::string>> remaining_deps; // child -> set of pending parents
    std::unordered_map<std::string, std::string> step_status; // step_name -> status ("PENDING", "RUNNING", "SUCCESS", "FAILED", "SKIPPED")

    std::mutex flow_mutex;
    std::condition_variable cv;

    for (const auto& s : flow.steps) {
        step_map[s.name] = s;
        step_status[s.name] = "PENDING";
        remaining_deps[s.name] = std::set<std::string>(s.dependencies.begin(), s.dependencies.end());
        for (const auto& dep : s.dependencies) {
            graph[dep].push_back(s.name);
        }
    }

    std::atomic<bool> flow_failed{false};
    std::string flow_error_msg = "";

    auto execute_step = [&](const std::string& step_name) {
        int64_t step_run_id = 0;
        Catalog::getInstance().logFlowStepStart(out_run_id, step_name, step_run_id);

        auto step_start = std::chrono::steady_clock::now();
        std::vector<std::string> col_names;
        std::vector<std::vector<std::string>> rows;
        std::string err;

        std::string cmd = step_map[step_name].command;
        bool ok = SQLRouter::routeAndExecute(cmd, engine, col_names, rows, err);
        auto step_end = std::chrono::steady_clock::now();
        int64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(step_end - step_start).count();

        std::string logs = "Executed command: " + cmd + "\nRows returned: " + std::to_string(rows.size());

        std::lock_guard<std::mutex> lock(flow_mutex);
        if (ok) {
            step_status[step_name] = "SUCCESS";
            Catalog::getInstance().logFlowStepEnd(step_run_id, "SUCCESS", duration, logs, "");

            // Unlock downstream steps
            for (const auto& child : graph[step_name]) {
                remaining_deps[child].erase(step_name);
            }
        } else {
            step_status[step_name] = "FAILED";
            flow_failed = true;
            if (flow_error_msg.empty()) flow_error_msg = "Step '" + step_name + "' failed: " + err;
            Catalog::getInstance().logFlowStepEnd(step_run_id, "FAILED", duration, logs, err);

            // Mark all downstream dependent steps as SKIPPED
            std::queue<std::string> skip_q;
            for (const auto& child : graph[step_name]) skip_q.push(child);

            while (!skip_q.empty()) {
                std::string curr = skip_q.front();
                skip_q.pop();
                if (step_status[curr] == "PENDING") {
                    step_status[curr] = "SKIPPED";
                    int64_t skip_run_id = 0;
                    Catalog::getInstance().logFlowStepStart(out_run_id, curr, skip_run_id);
                    Catalog::getInstance().logFlowStepEnd(skip_run_id, "SKIPPED", 0, "Skipped due to upstream failure in step '" + step_name + "'", "Upstream step failed");
                    for (const auto& child_of_skip : graph[curr]) {
                        skip_q.push(child_of_skip);
                    }
                }
            }
        }
        cv.notify_all();
    };

    // Dispatch loop
    while (true) {
        std::vector<std::string> ready_steps;
        {
            std::unique_lock<std::mutex> lock(flow_mutex);
            
            // Check if all steps finished (SUCCESS, FAILED, or SKIPPED)
            bool all_finished = true;
            for (const auto& kv : step_status) {
                if (kv.second == "PENDING" || kv.second == "RUNNING") {
                    all_finished = false;
                }
                if (kv.second == "PENDING" && remaining_deps[kv.first].empty()) {
                    ready_steps.push_back(kv.first);
                    step_status[kv.first] = "RUNNING";
                }
            }

            if (all_finished) break;

            if (ready_steps.empty()) {
                cv.wait(lock);
                continue;
            }
        }

        // Spawn parallel threads for ready steps
        for (const auto& step_name : ready_steps) {
            std::thread(execute_step, step_name).detach();
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    int64_t total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::string final_status = flow_failed ? "FAILED" : "SUCCESS";
    Catalog::getInstance().logFlowRunEnd(out_run_id, final_status, total_duration, flow_error_msg);

    if (flow_failed) {
        out_err = flow_error_msg;
        return false;
    }

    return true;
}
