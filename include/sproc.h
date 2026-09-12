#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "engine.h"

class SQLRouter {
public:
    static bool routeAndExecute(const std::string& raw_sql, 
                                 ExecutionEngine* engine,
                                 std::vector<std::string>& out_col_names, 
                                 std::vector<std::vector<std::string>>& out_rows, 
                                 std::string& out_error_msg,
                                 std::unordered_map<std::string, std::string>* vars = nullptr);

private:
    static bool handleCreateProject(const std::string& sql, std::string& out_msg);
    static bool handleCreateSchema(const std::string& sql, std::string& out_msg);
    static bool handleCreateRoutine(const std::string& sql, bool is_procedure, std::string& out_msg);
    static bool handleCreateJob(const std::string& sql, std::string& out_msg);
    static bool handleCreateFlow(const std::string& sql, std::string& out_msg);
    static bool handleCreateModel(const std::string& sql, std::string& out_msg);
    static bool handleRunFlow(const std::string& sql, ExecutionEngine* engine, std::string& out_msg);
    static bool handleCreateConnection(const std::string& sql, std::string& out_msg);
    static bool handleCreateSecret(const std::string& sql, std::string& out_msg);
    static bool handleAlterTenant(const std::string& sql, std::string& out_msg);
    
    // Procedure execution
    static bool handleCall(const std::string& sql, 
                           ExecutionEngine* engine, 
                           std::vector<std::string>& out_col_names, 
                           std::vector<std::vector<std::string>>& out_rows, 
                           std::string& out_error_msg,
                           std::unordered_map<std::string, std::string>* parent_vars = nullptr);
    
    // System views interception
    static bool handleSystemQuery(const std::string& sql, 
                                  ExecutionEngine* engine,
                                  std::vector<std::string>& out_col_names, 
                                  std::vector<std::vector<std::string>>& out_rows, 
                                  std::string& out_error_msg);

    // Helpers
    static std::string trim(const std::string& str);
    static bool startsWithIgnoreCase(const std::string& str, const std::string& prefix);
    static std::vector<std::string> splitStatements(const std::string& body);
    static std::string replaceAll(std::string str, const std::string& from, const std::string& to);
    static std::string stripComments(const std::string& sql);
    
    // Block execution
    static bool executeBlock(const std::vector<std::string>& stmts,
                             ExecutionEngine* engine,
                             std::vector<std::string>& out_col_names, 
                             std::vector<std::vector<std::string>>& out_rows, 
                             std::string& out_error_msg,
                             std::unordered_map<std::string, std::string>& vars,
                             bool& is_returned);
};
