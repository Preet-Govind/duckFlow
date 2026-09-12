#include "duckdb_engine.h"
#include "duckdb_loader.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "catalog.h"
#include "json.hpp"
#include "stat_activity.h"
#include <iostream>
#include <sstream>
#include <thread>

bool DuckDbEngine::connect(const std::string& connection_str) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Parse connection string: "db_path[?access_mode=READ_ONLY];so_path"
    db_path_ = "";
    so_path_ = "third_party/duckdb/libduckdb.so"; // Default path
    std::string access_mode = "READ_WRITE";

    size_t semicolon = connection_str.find(';');
    if (semicolon != std::string::npos) {
        db_path_ = connection_str.substr(0, semicolon);
        std::string custom_so = connection_str.substr(semicolon + 1);
        if (!custom_so.empty()) {
            so_path_ = custom_so;
        }
    } else {
        db_path_ = connection_str;
    }

    // Extract ACL access_mode from db_path_
    size_t qmark = db_path_.find('?');
    if (qmark != std::string::npos) {
        std::string query = db_path_.substr(qmark + 1);
        db_path_ = db_path_.substr(0, qmark);
        if (query.find("access_mode=READ_ONLY") != std::string::npos) {
            access_mode = "READ_ONLY";
        }
    }

    // Load the dynamic library
    auto& loader = DuckDbLoader::getInstance();
    if (!loader.load(so_path_)) {
        std::cerr << "Engine: Failed to load DuckDB library: " << so_path_ << std::endl;
        return false;
    }

    // Open database with ACL Policy
    const char* path_cstr = db_path_.empty() ? nullptr : db_path_.c_str();
    
    duckdb_state open_status;
    if (access_mode == "READ_ONLY" && loader.duckdb_create_config_fn && loader.duckdb_open_ext_fn) {
        duckdb_config config;
        loader.duckdb_create_config_fn(&config);
        loader.duckdb_set_config_fn(config, "access_mode", "READ_ONLY");
        
        char* err = nullptr;
        open_status = loader.duckdb_open_ext_fn(path_cstr, &db_, config, &err);
        loader.duckdb_destroy_config_fn(&config);
        
        if (err) {
            std::cerr << "Engine: ACL Boot Error: " << err << std::endl;
            loader.duckdb_free_fn(err);
        }
    } else {
        open_status = loader.duckdb_open_fn(path_cstr, &db_);
    }

    if (open_status == DuckDBError) {
        std::cerr << "Engine: Failed to open DuckDB file: " << db_path_ << std::endl;
        db_ = nullptr;
        return false;
    }

    // Create client connection
    if (loader.duckdb_connect_fn(db_, &conn_) == DuckDBError) {
        std::cerr << "Engine: Failed to connect to DuckDB database." << std::endl;
        loader.duckdb_close_fn(&db_);
        db_ = nullptr;
        conn_ = nullptr;
        return false;
    }

    // Register duckflow_http_get scalar function
    if (loader.duckdb_create_scalar_function_fn) {
        // Boot Extension Loaders for Cloud Data Lake capabilities
        duckdb_result res;
        if (loader.duckdb_query_fn(conn_, "INSTALL httpfs;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to INSTALL httpfs extension." << std::endl;
        }
        loader.duckdb_destroy_result_fn(&res);
        
        if (loader.duckdb_query_fn(conn_, "LOAD httpfs;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to LOAD httpfs extension." << std::endl;
        } else {
            std::cout << "DuckDbEngine: Cloud Data Lake extensions (httpfs) loaded successfully." << std::endl;
        }
        loader.duckdb_destroy_result_fn(&res);

        // Load ICU for timezone and TIMESTAMP WITH TIME ZONE parsing support
        if (loader.duckdb_query_fn(conn_, "INSTALL icu;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to INSTALL icu extension." << std::endl;
        }
        loader.duckdb_destroy_result_fn(&res);
        if (loader.duckdb_query_fn(conn_, "LOAD icu;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to LOAD icu extension." << std::endl;
        } else {
            std::cout << "DuckDbEngine: ICU extension loaded successfully." << std::endl;
        }
        loader.duckdb_destroy_result_fn(&res);

        // Boot SQLite integration for Catalog sharing
        if (loader.duckdb_query_fn(conn_, "INSTALL sqlite;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to INSTALL sqlite extension." << std::endl;
        }
        loader.duckdb_destroy_result_fn(&res);
        
        if (loader.duckdb_query_fn(conn_, "LOAD sqlite;", &res) != DuckDBSuccess) {
            std::cerr << "DuckDbEngine Warning: Failed to LOAD sqlite extension." << std::endl;
        } else {
            std::cout << "DuckDbEngine: SQLite extension loaded successfully." << std::endl;
            // Attach DuckFlow Catalog (sqlite metadata) so IDEs/users can query it seamlessly
            if (loader.duckdb_query_fn(conn_, "ATTACH 'teal_catalog.db' AS duckflow_catalog (TYPE SQLITE);", &res) != DuckDBSuccess) {
                std::cerr << "DuckDbEngine Warning: Failed to ATTACH teal_catalog.db" << std::endl;
            } else {
                std::cout << "DuckDbEngine: Attached duckflow_catalog successfully." << std::endl;
            }
            loader.duckdb_destroy_result_fn(&res);
        }

        duckdb_scalar_function func = loader.duckdb_create_scalar_function_fn();
        loader.duckdb_scalar_function_set_name_fn(func, "duckflow_http_get");
        
        duckdb_logical_type varchar_type = loader.duckdb_create_logical_type_fn(DUCKDB_TYPE_VARCHAR);
        loader.duckdb_scalar_function_add_parameter_fn(func, varchar_type);
        loader.duckdb_scalar_function_set_return_type_fn(func, varchar_type);
        
        loader.duckdb_scalar_function_set_function_fn(func, [](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
            auto& ldr = DuckDbLoader::getInstance();
            idx_t size = ldr.duckdb_data_chunk_get_size_fn(input);
            duckdb_vector url_vector = ldr.duckdb_data_chunk_get_vector_fn(input, 0);
            
            duckdb_string_t* url_data = (duckdb_string_t*)ldr.duckdb_vector_get_data_fn(url_vector);
            uint64_t* url_validity = ldr.duckdb_vector_get_validity_fn(url_vector);
            
            for (idx_t i = 0; i < size; i++) {
                if (!ldr.duckdb_validity_row_is_valid_fn(url_validity, i)) continue;
                
                duckdb_string_t str = url_data[i];
                std::string url_str(ldr.duckdb_string_t_data_fn(&str), ldr.duckdb_string_t_length_fn(str));
                
                std::string host = url_str;
                std::string path = "/";
                size_t proto = url_str.find("://");
                if (proto != std::string::npos) {
                    size_t slash = url_str.find("/", proto + 3);
                    if (slash != std::string::npos) {
                        host = url_str.substr(0, slash);
                        path = url_str.substr(slash);
                    }
                }
                
                // Spawn isolated HTTP client per row
                httplib::Client cli(host);
                cli.set_follow_location(true);
                cli.set_connection_timeout(5, 0);
                cli.set_read_timeout(10, 0);
                
                if (auto res = cli.Get(path)) {
                    ldr.duckdb_vector_assign_string_element_fn(output, i, res->body.c_str());
                } else {
                    ldr.duckdb_vector_assign_string_element_fn(output, i, "{\"error\": \"HTTP Timeout or Bad Host\"}");
                }
            }
        });
        
        loader.duckdb_register_scalar_function_fn(conn_, func);
        loader.duckdb_destroy_logical_type_fn(&varchar_type);
        loader.duckdb_destroy_scalar_function_fn(&func);
        std::cout << "Engine: Registered native 'duckflow_http_get(url)' network scalar." << std::endl;
        
        // --- Register ai_generate(prompt, model_name) ---
        duckdb_scalar_function ai_func = loader.duckdb_create_scalar_function_fn();
        loader.duckdb_scalar_function_set_name_fn(ai_func, "ai_generate");
        
        duckdb_logical_type varchar_type1 = loader.duckdb_create_logical_type_fn(DUCKDB_TYPE_VARCHAR);
        duckdb_logical_type varchar_type2 = loader.duckdb_create_logical_type_fn(DUCKDB_TYPE_VARCHAR);
        loader.duckdb_scalar_function_add_parameter_fn(ai_func, varchar_type1);
        loader.duckdb_scalar_function_add_parameter_fn(ai_func, varchar_type2);
        loader.duckdb_scalar_function_set_return_type_fn(ai_func, varchar_type1);
        
        loader.duckdb_scalar_function_set_function_fn(ai_func, [](duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
            auto& ldr = DuckDbLoader::getInstance();
            idx_t size = ldr.duckdb_data_chunk_get_size_fn(input);
            duckdb_vector prompt_vector = ldr.duckdb_data_chunk_get_vector_fn(input, 0);
            duckdb_vector model_vector = ldr.duckdb_data_chunk_get_vector_fn(input, 1);
            
            duckdb_string_t* prompt_data = (duckdb_string_t*)ldr.duckdb_vector_get_data_fn(prompt_vector);
            duckdb_string_t* model_data = (duckdb_string_t*)ldr.duckdb_vector_get_data_fn(model_vector);
            uint64_t* prompt_validity = ldr.duckdb_vector_get_validity_fn(prompt_vector);
            uint64_t* model_validity = ldr.duckdb_vector_get_validity_fn(model_vector);
            
            for (idx_t i = 0; i < size; i++) {
                if (!ldr.duckdb_validity_row_is_valid_fn(prompt_validity, i) || 
                    !ldr.duckdb_validity_row_is_valid_fn(model_validity, i)) continue;
                
                std::string prompt(ldr.duckdb_string_t_data_fn(&prompt_data[i]), ldr.duckdb_string_t_length_fn(prompt_data[i]));
                std::string model_name(ldr.duckdb_string_t_data_fn(&model_data[i]), ldr.duckdb_string_t_length_fn(model_data[i]));
                
                CatalogModel model;
                if (!Catalog::getInstance().getModel(model_name, model)) {
                    ldr.duckdb_vector_assign_string_element_fn(output, i, "{\"error\": \"Model not found\"}");
                    continue;
                }
                
                std::string host = model.endpoint;
                std::string path = "/";
                size_t proto = host.find("://");
                if (proto != std::string::npos) {
                    size_t slash = host.find("/", proto + 3);
                    if (slash != std::string::npos) {
                        path = host.substr(slash);
                        host = host.substr(0, slash);
                    }
                }
                
                httplib::Client cli(host);
                cli.set_follow_location(true);
                cli.set_connection_timeout(10, 0);
                cli.set_read_timeout(60, 0);
                
                if (!model.api_key.empty()) {
                    cli.set_bearer_token_auth(model.api_key);
                }
                
                nlohmann::json payload;
                if (model.type == "OLLAMA") {
                    if (path.back() != '/') path += "/";
                    if (path.find("/api/generate") == std::string::npos) {
                        path += "api/generate";
                    }
                    payload["model"] = "llama3"; // A sensible default if not parameterized further
                    payload["prompt"] = prompt;
                    payload["stream"] = false;
                } else if (model.type == "OPENAI") {
                    if (path.back() != '/') path += "/";
                    if (path.find("/chat/completions") == std::string::npos) {
                        path += "chat/completions";
                    }
                    payload["model"] = "gpt-3.5-turbo";
                    payload["messages"] = nlohmann::json::array({
                        {{"role", "user"}, {"content", prompt}}
                    });
                }
                
                std::string body = payload.dump();
                if (auto res = cli.Post(path, body, "application/json")) {
                    try {
                        auto resp_json = nlohmann::json::parse(res->body);
                        std::string ans = res->body;
                        if (model.type == "OLLAMA" && resp_json.contains("response")) {
                            ans = resp_json["response"].get<std::string>();
                        } else if (model.type == "OPENAI" && resp_json.contains("choices") && !resp_json["choices"].empty()) {
                            ans = resp_json["choices"][0]["message"]["content"].get<std::string>();
                        }
                        ldr.duckdb_vector_assign_string_element_fn(output, i, ans.c_str());
                    } catch (...) {
                        ldr.duckdb_vector_assign_string_element_fn(output, i, res->body.c_str());
                    }
                } else {
                    ldr.duckdb_vector_assign_string_element_fn(output, i, "{\"error\": \"HTTP POST Failed\"}");
                }
            }
        });
        
        loader.duckdb_register_scalar_function_fn(conn_, ai_func);
        loader.duckdb_destroy_logical_type_fn(&varchar_type1);
        loader.duckdb_destroy_logical_type_fn(&varchar_type2);
        loader.duckdb_destroy_scalar_function_fn(&ai_func);
        std::cout << "Engine: Registered native 'ai_generate(prompt, model)' network scalar." << std::endl;
    }

    std::cout << "Engine: Successfully connected to DuckDB (" << (db_path_.empty() ? ":memory:" : db_path_) 
              << ") using library version " << so_path_ << std::endl;
    return true;
}

void DuckDbEngine::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& loader = DuckDbLoader::getInstance();
    if (loader.isLoaded()) {
        if (conn_) {
            loader.duckdb_disconnect_fn(&conn_);
            conn_ = nullptr;
        }
        if (db_) {
            loader.duckdb_close_fn(&db_);
            db_ = nullptr;
        }
    }
}

bool DuckDbEngine::executeQuery(const std::string& sql, 
                               std::vector<std::string>& out_col_names, 
                               std::vector<std::vector<std::string>>& out_rows, 
                               std::string& out_error_msg) {
    // [ARCHITECTURE: LOCK-FREE CONCURRENCY]
    // We strictly do NOT use a global mutex lock here.
    // Instead, Teal utilizes a thread-local connection pool.
    // Every Web API worker thread and the background Job Scheduler thread get 
    // their own persistent DuckDB connection. This completely eradicates blocking 
    // and leverages DuckDB's native multi-version concurrency control (MVCC).
    
    auto& loader = DuckDbLoader::getInstance();
    if (!loader.isLoaded() || !db_) {
        out_error_msg = "DuckDB engine is not loaded or connected.";
        return false;
    }

    // Thread-local connection ensures thread safety and state isolation.
    thread_local duckdb_connection tl_conn = nullptr;
    if (!tl_conn) {
        if (loader.duckdb_connect_fn(db_, &tl_conn) == DuckDBError) {
            out_error_msg = "Failed to establish thread-local DuckDB connection.";
            return false;
        }
    }

    int session_fd = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto& tracker = StatActivityTracker::getInstance();
    tracker.addSession(session_fd, "Web UI / Internal");
    tracker.updateSession(session_fd, sql, "ACTIVE", tl_conn);

    duckdb_result result;
    if (loader.duckdb_query_fn(tl_conn, sql.c_str(), &result) == DuckDBError) {
        const char* err = loader.duckdb_result_error_fn(&result);
        out_error_msg = err ? err : "Unknown DuckDB error.";
        loader.duckdb_destroy_result_fn(&result);
        tracker.recordQuery(false);
        tracker.removeSession(session_fd);
        return false;
    }

    tracker.recordQuery(true);
    tracker.removeSession(session_fd);

    idx_t col_count = loader.duckdb_column_count_fn(&result);
    idx_t row_count = loader.duckdb_row_count_fn(&result);

    // Columns
    out_col_names.reserve(col_count);
    for (idx_t c = 0; c < col_count; ++c) {
        const char* name = loader.duckdb_column_name_fn(&result, c);
        out_col_names.push_back(name ? name : "");
    }

    // Rows
    out_rows.reserve(row_count);
    for (idx_t r = 0; r < row_count; ++r) {
        std::vector<std::string> row;
        row.reserve(col_count);
        for (idx_t c = 0; c < col_count; ++c) {
            char* val_cstr = loader.duckdb_value_varchar_fn(&result, c, r);
            if (val_cstr == nullptr) {
                row.push_back("NULL");
            } else {
                row.push_back(val_cstr);
                loader.duckdb_free_fn(val_cstr); // C API requires freeing returned varchars
            }
        }
        out_rows.push_back(row);
    }

    loader.duckdb_destroy_result_fn(&result);
    return true;
}
