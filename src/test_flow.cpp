#include <iostream>
#include <cassert>
#include "catalog.h"
#include "sproc.h"
#include "duckdb_engine.h"
#include "flow_engine.h"

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << " TESTING DUCKFLOW DAG ORCHESTRATION ENGINE " << std::endl;
    std::cout << "==========================================" << std::endl;

    // Initialize catalog
    Catalog::getInstance().init("test_flow_catalog.db");
    
    DuckDbEngine engine;
    if (!engine.connect("test_flow_data.db")) {
        std::cerr << "Failed to connect to DuckDB engine test instance" << std::endl;
        return 1;
    }

    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::string err;

    // 1. Test DDL Parsing and DAG Registration
    std::string ddl = "CREATE FLOW daily_pipeline "
                      "STEP extract DEPENDS ON () AS 'CREATE TABLE IF NOT EXISTS raw_data (id INT, val TEXT); INSERT INTO raw_data VALUES (1, ''item1''), (2, ''item2'');' "
                      "STEP transform DEPENDS ON (extract) AS 'CREATE TABLE IF NOT EXISTS summary AS SELECT COUNT(*) AS total_items FROM raw_data;' "
                      "STEP load DEPENDS ON (transform) AS 'SELECT * FROM summary;';";

    std::cout << "\n1. Registering valid DAG Flow via SQL DDL..." << std::endl;
    bool ok = SQLRouter::routeAndExecute(ddl, &engine, cols, rows, err);
    std::cout << "Result: " << (ok ? "SUCCESS" : "FAILED") << " | " << rows[0][0] << std::endl;
    assert(ok == true);

    // 2. Test Cycle Detection Validation
    std::string cyclic_ddl = "CREATE FLOW cyclic_flow "
                             "STEP node1 DEPENDS ON (node2) AS 'SELECT 1;' "
                             "STEP node2 DEPENDS ON (node1) AS 'SELECT 2;';";
    std::cout << "\n2. Testing Cyclic Flow Rejection..." << std::endl;
    bool cyclic_ok = SQLRouter::routeAndExecute(cyclic_ddl, &engine, cols, rows, err);
    std::cout << "Result (Expected Fail): " << (!cyclic_ok ? "PASSED (Rejected)" : "FAILED (Allowed Cycle)") << " | " << rows[0][0] << std::endl;
    assert(cyclic_ok == false);

    // 2.5 Test optional DEPENDS ON parsing
    std::string optional_deps_ddl = "CREATE FLOW test01\n"
                                    "STEP node1 AS 'SELECT 1;'\n"
                                    "STEP node2 DEPENDS ON (node1) AS 'SELECT 2;';";
    std::cout << "\n2.5. Testing Optional DEPENDS ON Parsing..." << std::endl;
    bool optional_ok = SQLRouter::routeAndExecute(optional_deps_ddl, &engine, cols, rows, err);
    std::cout << "Result: " << (optional_ok ? "SUCCESS" : "FAILED") << " | " << rows[0][0] << std::endl;
    assert(optional_ok == true);

    // 3. Test Flow Execution (Topological Parallel Execution)
    std::cout << "\n3. Executing Flow via RUN FLOW daily_pipeline..." << std::endl;
    bool run_ok = SQLRouter::routeAndExecute("RUN FLOW daily_pipeline;", &engine, cols, rows, err);
    std::cout << "Result: " << (run_ok ? "SUCCESS" : "FAILED") << " | " << rows[0][0] << std::endl;
    assert(run_ok == true);

    // 4. Verify Catalog Run Logs and Data Output
    std::cout << "\n4. Verifying created tables and step execution history..." << std::endl;
    cols.clear();
    rows.clear();
    SQLRouter::routeAndExecute("SELECT total_items FROM summary;", &engine, cols, rows, err);
    std::cout << "Summary Table Total Items: " << rows[0][0] << " (Expected: 2)" << std::endl;
    assert(rows[0][0] == "2");

    auto runs = Catalog::getInstance().getFlowRuns(10);
    std::cout << "Recorded Flow Runs in Catalog: " << runs.size() << std::endl;
    assert(!runs.empty());
    std::cout << "Flow Run Status: " << runs[0]["status"] << " | Duration: " << runs[0]["duration_ms"] << " ms" << std::endl;
    assert(runs[0]["status"] == "SUCCESS");

    int64_t run_id = std::stoll(runs[0]["id"]);
    auto step_runs = Catalog::getInstance().getFlowStepRuns(run_id);
    std::cout << "Recorded Step Runs for Run #" << run_id << ": " << step_runs.size() << " steps" << std::endl;
    for (const auto& s : step_runs) {
        std::cout << "  - Step: " << s.at("step_name") << " | Status: " << s.at("status") << " | Duration: " << s.at("duration_ms") << " ms" << std::endl;
    }
    assert(step_runs.size() == 3);

    std::cout << "\n==========================================" << std::endl;
    std::cout << " ALL DAG ENGINE TESTS PASSED SUCCESSFULLY! " << std::endl;
    std::cout << "==========================================" << std::endl;

    // Clean up test databases
    std::remove("test_flow_catalog.db");
    std::remove("test_flow_data.db");
    return 0;
}
