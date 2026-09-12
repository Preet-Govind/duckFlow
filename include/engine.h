#pragma once

#include <string>
#include <vector>

class ExecutionEngine {
public:
    virtual ~ExecutionEngine() = default;

    virtual bool connect(const std::string& connection_str) = 0;
    virtual void disconnect() = 0;

    virtual bool executeQuery(const std::string& sql, 
                               std::vector<std::string>& out_col_names, 
                               std::vector<std::vector<std::string>>& out_rows, 
                               std::string& out_error_msg) = 0;
};
