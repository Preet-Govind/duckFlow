cmake_minimum_required(VERSION 3.15)
project(Teal LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile options: Enable Thread-Safe SQLite3 and global OpenSSL for httplib
add_compile_options(-Wall -Wextra -O2 -DSQLITE_THREADSAFE=1 -DCPPHTTPLIB_OPENSSL_SUPPORT=1)

# Include paths
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3
)

# Sources list
set(SOURCES
    src/main.cpp
    src/catalog.cpp
    src/duckdb_engine.cpp
    src/scheduler.cpp
    src/sproc.cpp
    src/stat_activity.cpp
    src/http_server.cpp
    src/raft.cpp
    src/garbage_collector.cpp
    src/flow_engine.cpp
    third_party/sqlite3/sqlite3.c
)

add_executable(teal ${SOURCES})

# Link Libraries
# We link dl for dlopen/dlsym, pthread for multi-threading, and ssl/crypto
target_link_libraries(teal
    PRIVATE
    pthread
    dl
    ssl
    crypto
)

# Install rules
add_executable(teal-cli src/cli.cpp)
target_link_libraries(teal-cli PRIVATE pthread ssl crypto)

# Test targets
add_executable(test-flow src/test_flow.cpp src/catalog.cpp src/duckdb_engine.cpp src/scheduler.cpp src/sproc.cpp src/stat_activity.cpp src/http_server.cpp src/raft.cpp src/garbage_collector.cpp src/flow_engine.cpp third_party/sqlite3/sqlite3.c)
target_link_libraries(test-flow PRIVATE pthread dl ssl crypto)

install(TARGETS teal teal-cli DESTINATION bin)

