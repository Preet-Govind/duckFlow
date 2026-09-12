#pragma once

#include <dlfcn.h>
#include <string>
#include <iostream>
#include "duckdb.h" // Include C API definitions

class DuckDbLoader {
public:
    static DuckDbLoader& getInstance() {
        static DuckDbLoader instance;
        return instance;
    }

    bool load(const std::string& so_path) {
        if (handle_) {
            // Already loaded. If path is identical, return true.
            if (current_path_ == so_path) return true;
            unload();
        }

        handle_ = dlopen(so_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (!handle_) {
            std::cerr << "Failed to dynamically load DuckDB from '" << so_path << "': " << dlerror() << std::endl;
            return false;
        }

        #define LOAD_SYM(name) \
            name##_fn = (decltype(&name))dlsym(handle_, #name); \
            if (!name##_fn) { \
                std::cerr << "Failed to find symbol: " << #name << " in " << so_path << std::endl; \
                unload(); \
                return false; \
            }

        LOAD_SYM(duckdb_open);
        LOAD_SYM(duckdb_close);
        LOAD_SYM(duckdb_connect);
        LOAD_SYM(duckdb_disconnect);
        LOAD_SYM(duckdb_query);
        LOAD_SYM(duckdb_destroy_result);
        LOAD_SYM(duckdb_column_count);
        LOAD_SYM(duckdb_column_name);
        LOAD_SYM(duckdb_row_count);
        LOAD_SYM(duckdb_value_varchar);
        LOAD_SYM(duckdb_free);
        LOAD_SYM(duckdb_result_error);
        
        LOAD_SYM(duckdb_create_config);
        LOAD_SYM(duckdb_set_config);
        LOAD_SYM(duckdb_open_ext);
        LOAD_SYM(duckdb_destroy_config);

        LOAD_SYM(duckdb_create_scalar_function);
        LOAD_SYM(duckdb_scalar_function_set_name);
        LOAD_SYM(duckdb_create_logical_type);
        LOAD_SYM(duckdb_scalar_function_add_parameter);
        LOAD_SYM(duckdb_scalar_function_set_return_type);
        LOAD_SYM(duckdb_scalar_function_set_function);
        LOAD_SYM(duckdb_register_scalar_function);
        LOAD_SYM(duckdb_destroy_logical_type);
        LOAD_SYM(duckdb_destroy_scalar_function);
        LOAD_SYM(duckdb_data_chunk_get_size);
        LOAD_SYM(duckdb_data_chunk_get_vector);
        LOAD_SYM(duckdb_vector_get_data);
        LOAD_SYM(duckdb_vector_get_validity);
        LOAD_SYM(duckdb_validity_row_is_valid);
        LOAD_SYM(duckdb_vector_assign_string_element);
        LOAD_SYM(duckdb_string_t_data);
        LOAD_SYM(duckdb_string_t_length);
        LOAD_SYM(duckdb_interrupt);

        #undef LOAD_SYM
        current_path_ = so_path;
        return true;
    }

    void unload() {
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
            current_path_ = "";
        }
        
        // Reset pointers
        duckdb_open_fn = nullptr;
        duckdb_close_fn = nullptr;
        duckdb_connect_fn = nullptr;
        duckdb_disconnect_fn = nullptr;
        duckdb_query_fn = nullptr;
        duckdb_destroy_result_fn = nullptr;
        duckdb_column_count_fn = nullptr;
        duckdb_column_name_fn = nullptr;
        duckdb_row_count_fn = nullptr;
        duckdb_value_varchar_fn = nullptr;
        duckdb_free_fn = nullptr;
        duckdb_result_error_fn = nullptr;
        
        duckdb_create_config_fn = nullptr;
        duckdb_set_config_fn = nullptr;
        duckdb_open_ext_fn = nullptr;
        duckdb_destroy_config_fn = nullptr;
    }

    bool isLoaded() const { return handle_ != nullptr; }
    std::string getLoadedPath() const { return current_path_; }

    // Function pointers
    decltype(&duckdb_open) duckdb_open_fn = nullptr;
    decltype(&duckdb_close) duckdb_close_fn = nullptr;
    decltype(&duckdb_connect) duckdb_connect_fn = nullptr;
    decltype(&duckdb_disconnect) duckdb_disconnect_fn = nullptr;
    decltype(&duckdb_query) duckdb_query_fn = nullptr;
    decltype(&duckdb_destroy_result) duckdb_destroy_result_fn = nullptr;
    decltype(&duckdb_column_count) duckdb_column_count_fn = nullptr;
    decltype(&duckdb_column_name) duckdb_column_name_fn = nullptr;
    decltype(&duckdb_row_count) duckdb_row_count_fn = nullptr;
    decltype(&duckdb_value_varchar) duckdb_value_varchar_fn = nullptr;
    decltype(&duckdb_free) duckdb_free_fn = nullptr;
    decltype(&duckdb_result_error) duckdb_result_error_fn = nullptr;
    
    decltype(&duckdb_create_config) duckdb_create_config_fn = nullptr;
    decltype(&duckdb_set_config) duckdb_set_config_fn = nullptr;
    decltype(&duckdb_open_ext) duckdb_open_ext_fn = nullptr;
    decltype(&duckdb_destroy_config) duckdb_destroy_config_fn = nullptr;

    decltype(&duckdb_create_scalar_function) duckdb_create_scalar_function_fn = nullptr;
    decltype(&duckdb_scalar_function_set_name) duckdb_scalar_function_set_name_fn = nullptr;
    decltype(&duckdb_create_logical_type) duckdb_create_logical_type_fn = nullptr;
    decltype(&duckdb_scalar_function_add_parameter) duckdb_scalar_function_add_parameter_fn = nullptr;
    decltype(&duckdb_scalar_function_set_return_type) duckdb_scalar_function_set_return_type_fn = nullptr;
    decltype(&duckdb_scalar_function_set_function) duckdb_scalar_function_set_function_fn = nullptr;
    decltype(&duckdb_register_scalar_function) duckdb_register_scalar_function_fn = nullptr;
    decltype(&duckdb_destroy_logical_type) duckdb_destroy_logical_type_fn = nullptr;
    decltype(&duckdb_destroy_scalar_function) duckdb_destroy_scalar_function_fn = nullptr;
    decltype(&duckdb_data_chunk_get_size) duckdb_data_chunk_get_size_fn = nullptr;
    decltype(&duckdb_data_chunk_get_vector) duckdb_data_chunk_get_vector_fn = nullptr;
    decltype(&duckdb_vector_get_data) duckdb_vector_get_data_fn = nullptr;
    decltype(&duckdb_vector_get_validity) duckdb_vector_get_validity_fn = nullptr;
    decltype(&duckdb_validity_row_is_valid) duckdb_validity_row_is_valid_fn = nullptr;
    decltype(&duckdb_vector_assign_string_element) duckdb_vector_assign_string_element_fn = nullptr;
    decltype(&duckdb_string_t_data) duckdb_string_t_data_fn = nullptr;
    decltype(&duckdb_interrupt) duckdb_interrupt_fn = nullptr;
    decltype(&duckdb_string_t_length) duckdb_string_t_length_fn = nullptr;

private:
    DuckDbLoader() = default;
    ~DuckDbLoader() { unload(); }
    DuckDbLoader(const DuckDbLoader&) = delete;
    DuckDbLoader& operator=(const DuckDbLoader&) = delete;

    void* handle_ = nullptr;
    std::string current_path_ = "";
};
