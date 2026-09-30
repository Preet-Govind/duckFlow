# DuckFlow Examples

This document outlines how to write Stored Procedures in DuckFlow, highlighting how our C++ parser interacts with the DuckDB execution engine.

## 1. The `FOR ... IN (query) LOOP` 
Because DuckFlow delegates data processing to DuckDB, our loops don't use the standard `1..3` range syntax. Instead, they drive the loop using a DuckDB **SQL Query**! 

You do **not** need a `DECLARE` block. The loop automatically scopes the variables returned by the query.

```sql
CREATE OR REPLACE PROCEDURE public.refresh_sales() AS
BEGIN
    -- 1. Create staging data
    CREATE TEMP TABLE qwe AS SELECT 0 AS col;
    
    DROP TABLE IF EXISTS test_sp;
    CREATE TABLE test_sp AS SELECT *,  cast(current_timestamp as timestamp )  AS ts FROM qwe;
    
    -- 2. Use DuckDB's generate_series to loop 3 times
    -- This executes the query in DuckDB, and then loops in C++ 3 times!
    FOR i IN (SELECT unnest(generate_series(1, 3)) AS val) LOOP
        
        -- You can reference 'i.val' if you need the number (1, 2, 3), 
        -- but here we just insert the timestamp 3 times.
        INSERT INTO test_sp SELECT i.val, cast(current_timestamp as timestamp ) ;
        
    END LOOP;
END;
```
*(Note: Be sure to use `END LOOP;` as two separate words!)*


## 2. Dynamic SQL inside a Loop
This is extremely powerful for dynamically generating strings and executing them.

```sql
CREATE OR REPLACE PROCEDURE public.backup_metadata() AS
BEGIN
    -- Query the internal DuckFlow catalog we attached earlier
    FOR t IN (SELECT name FROM duckflow_catalog.tables WHERE schema_name = 'public') LOOP
        
        -- The EXECUTE command concatenates the string and runs it dynamically
        EXECUTE 'COPY ' || t.name || ' TO ''s3://my-bucket/backups/' || t.name || '.csv'' (FORMAT CSV)';
        
    END LOOP;
END;
```

## 3. Passing Arguments to Procedures
You can pass arguments to procedures and reference them with either the `$arg` syntax or exact word match.

```sql
CREATE OR REPLACE PROCEDURE public.load_daily_data(date_str, source_path) AS
BEGIN
    /* 
      We use $date_str and $source_path. 
      The C++ symbol table will inject the values automatically.
    */
    CREATE TABLE IF NOT EXISTS daily_metrics (ds DATE, val DOUBLE);
    
    INSERT INTO daily_metrics 
    SELECT CAST($date_str AS DATE), val 
    FROM read_parquet($source_path);
    
END;

-- Invoking the procedure
CALL public.load_daily_data('2026-09-12', 's3://bucket/data/2026_09_12.parquet');
```

## 4. Conditionals (`IF / THEN / ELSE / END IF`)
You can use `IF/ELSE` blocks with conditions that evaluate dynamically using the DuckDB engine. You can also use variables in the condition via `:var` or `$var`.

```sql
CREATE OR REPLACE PROCEDURE public.conditional_test(x) AS
BEGIN
    IF :x > 10 THEN
        PRINT 'Value is greater than 10!';
    ELSE
        PRINT 'Value is 10 or less!';
    END IF;
END;

-- Invoking the procedure
CALL public.conditional_test(15);
```

## 5. `WHILE` Loops
While `FOR` loops use a query to generate iterations, `WHILE` loops repeatedly evaluate a condition query:

```sql
CREATE OR REPLACE PROCEDURE public.while_test() AS
BEGIN
    WHILE (SELECT COUNT(*) FROM my_table) < 5 LOOP
        INSERT INTO my_table VALUES (1);
    END LOOP;
END;
```

## 6. Exception Handling (`BEGIN ... EXCEPTION ... END`)
You can handle errors gracefully without aborting the entire routine by wrapping risky operations inside a nested `BEGIN/END` block with an `EXCEPTION WHEN OTHERS` handler:

```sql
CREATE OR REPLACE PROCEDURE public.exception_test() AS
BEGIN
    PRINT 'Starting operation...';
    
    BEGIN
        INSERT INTO table_with_unique_key VALUES (1);
    EXCEPTION WHEN OTHERS THEN
        PRINT 'Failed to insert: ' || :SQLERRM;
    END;
    
    PRINT 'Operation finished gracefully!';
END;
```

## 7. `RETURN` Values (Functions)
Routines created with `CREATE FUNCTION` can return scalar values that can be used directly or returned to the CLI output:

```sql
CREATE FUNCTION compute_value(x) RETURNS INT AS
BEGIN
    IF :x > 10 THEN
        RETURN :x * 2;
    END IF;
    RETURN :x;
END;

-- CLI invocation
CALL compute_value(15); -- Output: 30
```

## 8. System Logging & Auditing
The `PRINT` and `RAISE NOTICE` commands log directly into DuckFlow's catalog database (`teal_catalog.db`). You can inspect these logs by querying the built-in system view:

```sql
SELECT * FROM duckflow_catalog.duckflow_logs;
```

---

# DuckFlow Core Features (OS & Orchestration)

DuckFlow isn't just a database; it is a Distributed Analytical Database OS. Here are examples of its core data engineering and orchestration capabilities:

## 9. Creating Projects
Organize your routines, jobs, and flows logically by creating namespaces (Projects).

```sql
CREATE PROJECT data_engineering_prod;
-- Automatically creates schemas and scopes assets to this project
```

## 10. Background Jobs (Scheduler)
DuckFlow has a built-in cron scheduler. You can schedule SQL commands or procedures to run automatically in the background.

```sql
CREATE JOB daily_sales_aggregation 
SCHEDULE '0 2 * * *' 
AS 'CALL refresh_sales();';
```

## 11. Directed Acyclic Graphs (CREATE FLOW)
Build complex data pipelines directly in SQL! You can define multi-step flows where steps run sequentially or in parallel based on dependencies.

```sql
CREATE FLOW nightly_etl
    STEP extract AS 'CALL extract_s3_data();'
    STEP transform DEPENDS ON (extract) AS 'CALL transform_data();'
    STEP load DEPENDS ON (transform) AS 'CALL load_warehouse();'
    STEP notify DEPENDS ON (load) AS 'PRINT ''ETL Finished!'';';
```
*(The workflow engine will automatically topological-sort the DAG and execute it.)*

## 12. Tenant Resource Governor (Multi-tenancy)
DuckFlow acts as an OS, meaning you can dynamically sandbox memory, threads, and queries per second (QPS) for specific API tenants across the distributed cluster.

```sql
ALTER TENANT 'sk_analytics_team_123' SET MEMORY='16GB', THREADS=8, QPS=500;
```

## 13. Native Network Fetching (`duckflow_http_get`)
DuckFlow includes native HTTP scalar functions to fetch data directly during query execution (perfect for hitting REST APIs inside DuckDB).

```sql
SELECT duckflow_http_get('https://api.coindesk.com/v1/bpi/currentprice.json') AS btc_price;
```

## 14. System Auditing Views
DuckFlow tracks everything that happens inside the OS. You can query these internal states just like normal tables:

```sql
-- View all currently active queries and their execution times across the cluster
SELECT * FROM teal_stat_activity;

-- View all scheduled jobs
SELECT * FROM teal_jobs;

-- View historical runs of all background jobs
SELECT * FROM teal_job_history;

-- View all stored procedures and functions
SELECT * FROM duckflow_catalog.routines;

-- View execution logs for all DAG Flows
SELECT * FROM duckflow_catalog.flow_runs;
```

## 15. Native AI Integration (LLM Models)
DuckFlow has native integrations with AI engines (like OpenAI and local Ollama models) allowing you to execute AI prompts directly from SQL without writing external Python scripts.

```sql
-- Register a local Ollama/llama.cpp model with an OpenAI-compatible endpoint
CREATE MODEL test_model TYPE OLLAMA WITH (endpoint='http://localhost:8080/v1/');

-- Query the AI model directly in SQL
SELECT ai_generate('What is DuckDB?', 'test_model');

-- View all registered models
SELECT * FROM teal_models;
```

