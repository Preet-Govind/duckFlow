#include "catalog.h"
#include "duckdb_engine.h"
#include "scheduler.h"
#include "http_server.h"
#include "raft.h"
#include "garbage_collector.h"
#include <filesystem>
#include <iostream>
#include <csignal>
#include <chrono>

std::atomic<bool> keep_running{true};

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ". Forcing instant shutdown..." << std::endl;
    std::exit(0);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "==========================================================" << std::endl;
    std::cout << "   DuckFlow( Teal ): analytical database OS Gateway starting...       " << std::endl;
    std::cout << "==========================================================" << std::endl;

    // [CRASH RESILIENCE] 
    // Securely wipe the temporary sandboxes directory to prevent disk bloat
    // from massive orphaned databases left by violent server crashes.
    try {
        if (std::filesystem::exists("sandboxes")) {
            std::filesystem::remove_all("sandboxes");
        }
        std::filesystem::create_directory("sandboxes");
        std::cout << "Sandbox storage wiped and initialized." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to initialize sandboxes directory: " << e.what() << std::endl;
    }

    // Config defaults
    int http_port = 8080;
    std::string catalog_db = "teal_catalog.db";
    std::string duck_db = "teal_data.db";
    std::string web_dir = "web";

    std::vector<std::string> peers;

    // Simple arg parsing if provided
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--http-port" && i + 1 < argc) {
            http_port = std::stoi(argv[++i]);
        } else if (arg == "--catalog" && i + 1 < argc) {
            catalog_db = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            duck_db = argv[++i];
        } else if (arg == "--join" && i + 1 < argc) {
            peers.push_back(argv[++i]);
        }
    }

    // Initialize Raft consensus layer
    RaftEngine::getInstance().init(peers, http_port);

    // Initialize Metadata Catalog
    if (!Catalog::getInstance().init(catalog_db)) {
        std::cerr << "CRITICAL: Failed to initialize SQLite Metadata Catalog." << std::endl;
        return 1;
    }
    std::cout << "Catalog database initialized: " << catalog_db << std::endl;

    // Initialize DuckDB Analytical Engine
    auto* engine = new DuckDbEngine();
    if (!engine->connect(duck_db)) {
        std::cerr << "CRITICAL: Failed to connect to DuckDB Engine." << std::endl;
        delete engine;
        return 1;
    }
    std::cout << "Analytical Engine (DuckDB) loaded: " << (duck_db.empty() ? ":memory:" : duck_db) << std::endl;

    // Start Job Scheduler
    Scheduler::getInstance().start(engine);

    // Start Garbage Collector (Sweeps every 60 minutes)
    GarbageCollector::getInstance().start(60);

    // Start HTTP Server / Web Dashboard
    HttpServer::getInstance().start(http_port, web_dir, engine);

    std::cout << "Teal is fully operational. Open Web Console at http://localhost:" << http_port << std::endl;
    std::cout << "Press Ctrl+C to terminate." << std::endl;

    // Maintain main loop
    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Shutting down Teal gateway services..." << std::endl;
    GarbageCollector::getInstance().stop();
    HttpServer::getInstance().stop();
    Scheduler::getInstance().stop();
    engine->disconnect();
    Catalog::getInstance().close();
    delete engine;

    std::cout << "Teal successfully shut down." << std::endl;
    return 0;
}
