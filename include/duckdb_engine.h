#pragma once

#include "engine.h"
#include "duckdb.h"
#include <string>
#include <mutex>

class DuckDbEngine : public ExecutionEngine {
public:
    DuckDbEngine() = default;
    ~DuckDbEngine() override { disconnect(); }

    // connection_str format: "db_file_path;libduckdb_so_path"
    bool connect(const std::string& connection_str) override;
    void disconnect() override;

    bool executeQuery(const std::string& sql, 
                       std::vector<std::string>& out_col_names, 
                       std::vector<std::vector<std::string>>& out_rows, 
                       std::string& out_error_msg) override;

private:
    duckdb_database db_ = nullptr;
    duckdb_connection conn_ = nullptr;
    std::string db_path_;
    std::string so_path_;
    std::mutex mutex_;
};
