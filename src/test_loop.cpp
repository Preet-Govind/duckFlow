#include "duckdb_engine.h"
#include "sproc.h"
#include "catalog.h"
#include <iostream>

int main() {
    DuckDbEngine engine;
    if (!engine.connect("test.db")) {
        std::cerr << "failed to connect" << std::endl;
        return 1;
    }

    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::string err;

    std::string proc = R"(
    CREATE OR REPLACE PROCEDURE public.refresh_sales() AS 
    BEGIN
        CREATE TEMP TABLE qwe AS SELECT 1 AS col;
        DROP TABLE IF EXISTS test_sp;     
        CREATE TABLE test_sp AS SELECT *, current_timestamp AS ts FROM qwe;
        
        FOR i IN (SELECT unnest(generate_series(1, 3)) AS val) LOOP
            INSERT INTO test_sp SELECT i.val, current_timestamp;
        END LOOP;
    END;
    )";
    SQLRouter::routeAndExecute(proc, &engine, cols, rows, err);

    std::string call_proc = "CALL public.refresh_sales();";
    SQLRouter::routeAndExecute(call_proc, &engine, cols, rows, err);
    std::cout << "Call err: " << err << "\n";

    std::string q = "SELECT * FROM test_sp;";
    engine.executeQuery(q, cols, rows, err);
    
    std::cout << "Results of test_sp:\n";
    for(auto c : cols) std::cout << c << "\t";
    std::cout << "\n";
    for (auto r : rows) {
        for (auto v : r) std::cout << v << "\t";
        std::cout << "\n";
    }

    return 0;
}
