#include "garbage_collector.h"
#include "catalog.h"
#include <iostream>
#include <filesystem>
#include <vector>

void GarbageCollector::start(int interval_minutes) {
    if (running_) return;
    interval_minutes_ = interval_minutes;
    running_ = true;
    gc_thread_ = std::thread(&GarbageCollector::runLoop, this);
    std::cout << "Garbage Collector: Background daemon started (Sweep Interval: " << interval_minutes_ << "m)." << std::endl;
}

void GarbageCollector::stop() {
    running_ = false;
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

void GarbageCollector::runLoop() {
    while (running_) {
        // Sleep in small increments to allow for quick shutdown interruption
        for (int i = 0; i < interval_minutes_ * 60 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (running_) {
            triggerSweep();
        }
    }
}

void GarbageCollector::triggerSweep() {
    std::cout << "\n[GC] Starting automated Garbage Collection sweep..." << std::endl;
    sweepOldLogs();
    sweepOrphanedTenants();
    optimizeCatalog();
    std::cout << "[GC] Sweep complete. System optimized.\n" << std::endl;
}

void GarbageCollector::sweepOldLogs() {
    // Retain only the last 7 days of execution logs to prevent SQLite bloat
    std::cout << "[GC] Sweeping stale job history logs..." << std::endl;
    std::string sql = "DELETE FROM job_history WHERE started_at < datetime('now', '-7 days');";
    if (Catalog::getInstance().execute(sql)) {
        // SQLite doesn't natively return rows affected from sqlite3_exec, but the state is now pruned!
        std::cout << "[GC] Stale logs pruned successfully." << std::endl;
    }
}

void GarbageCollector::sweepOrphanedTenants() {
    std::cout << "[GC] Scanning for orphaned physical Tenant databases..." << std::endl;
    
    // 1. Get all active tenant paths from the Metadata Catalog
    auto active_tenants = Catalog::getInstance().getTenants();
    std::vector<std::string> valid_paths;
    for (const auto& t : active_tenants) {
        // Strip URI parameters if they exist (e.g., ?access_mode=READ_ONLY)
        std::string clean_path = t.db_path;
        size_t query_pos = clean_path.find("?");
        if (query_pos != std::string::npos) {
            clean_path = clean_path.substr(0, query_pos);
        }
        valid_paths.push_back(clean_path);
    }
    
    // 2. Scan the hard drive 'tenants/' directory for physical files
    if (!std::filesystem::exists("tenants")) return;
    
    int orphaned_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("tenants")) {
        if (entry.is_regular_file()) {
            std::string physical_path = entry.path().string();
            
            // Check if this physical file exists in our active SQL metadata routing table
            bool is_valid = false;
            for (const auto& vp : valid_paths) {
                if (physical_path == vp) {
                    is_valid = true;
                    break;
                }
            }
            
            // If it's an orphaned DuckDB file (or a WAL/tmp file associated with a deleted DB)
            if (!is_valid) {
                std::cout << "[GC] Found orphaned file: " << physical_path << ". Shredding from SSD..." << std::endl;
                try {
                    std::filesystem::remove(entry.path());
                    orphaned_count++;
                } catch (const std::exception& e) {
                    std::cerr << "[GC] Warning: Failed to remove file " << physical_path << " - " << e.what() << std::endl;
                }
            }
        }
    }
    
    if (orphaned_count > 0) {
        std::cout << "[GC] Shredded " << orphaned_count << " orphaned tenant files." << std::endl;
    } else {
        std::cout << "[GC] No orphaned tenant files found." << std::endl;
    }
}

void GarbageCollector::optimizeCatalog() {
    // Execute SQLite VACUUM to defragment the B-Tree and reclaim raw byte space
    std::cout << "[GC] Defragmenting Metadata Catalog B-Tree (VACUUM)..." << std::endl;
    Catalog::getInstance().execute("VACUUM;");
}
