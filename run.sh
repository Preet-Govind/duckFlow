#!/bin/bash

echo "=== Preparing Teal Gateway Environment ==="

# 1. Kill any existing running Teal process
echo "Stopping existing Teal instances..."
killall teal 2>/dev/null || kill -9 $(pgrep teal) 2>/dev/null || true

# 2. Release any process locking teal_data.db
if [ -f "teal_data.db" ]; then
    PID=$(fuser teal_data.db 2>/dev/null | awk '{print $1}')
    if [ ! -z "$PID" ]; then
        echo "Releasing lock on teal_data.db (PID: $PID)..."
        kill -9 $PID 2>/dev/null || true
    fi
fi

# 3. Start the gateway
MODE=${1:-single}

if [ "$MODE" == "cluster" ]; then
    echo "Starting Teal Web Console (Cluster Node) on port 9090..."
    # You can pass additional arguments after "cluster" (e.g. ./run.sh cluster --join http://other:8081)
    shift
    exec ./build/teal --http-port 9090 --catalog cluster_catalog2.db --db cluster_data2.db --join http://localhost:8081 "$@"
else
    echo "Starting Teal Web Console (Single/Leader Node) on port 8081..."
    # Note: If passing arguments to single node, just pass them directly (e.g. ./run.sh --help)
    exec ./build/teal --http-port 8081 "$@"
fi