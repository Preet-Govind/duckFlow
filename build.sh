#!/bin/bash
set -e

echo "=== Configuring and building Teal Database Gateway ==="

# Check for duckdb base library
if [ ! -f "third_party/duckdb/libduckdb.so" ]; then
    echo "Downloading DuckDB base library (v1.4.3)..."
    mkdir -p third_party/duckdb/v1.4.3
    cd third_party/duckdb/v1.4.3
    wget -q https://github.com/duckdb/duckdb/releases/download/v1.4.3/libduckdb-linux-amd64.zip
    unzip -o libduckdb-linux-amd64.zip
    rm libduckdb-linux-amd64.zip
    cd ../
    ln -s v1.4.3/libduckdb.so libduckdb.so
    cd ../../
fi

# Initialize build directory if not present
if [ ! -d "build" ]; then
    echo "Running CMake configuration..."
    cmake -B build -S .
fi

# Compile using nproc cores
echo "Building binary..."
cmake --build build -j$(nproc)

echo "=== Build Complete! Executable is located at ./build/teal ==="
