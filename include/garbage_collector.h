#pragma once
#include <thread>
#include <atomic>
#include <chrono>

class GarbageCollector {
public:
    static GarbageCollector& getInstance() {
        static GarbageCollector instance;
        return instance;
    }

    // Starts the background GC thread
    void start(int interval_minutes = 60);
    
    // Stops the background daemon
    void stop();
    
    // Manually trigger a full sweep
    void triggerSweep();

private:
    GarbageCollector() = default;
    ~GarbageCollector() { stop(); }

    std::atomic<bool> running_{false};
    std::thread gc_thread_;
    int interval_minutes_ = 60;

    void runLoop();
    void sweepOldLogs();
    void sweepOrphanedTenants();
    void optimizeCatalog();
};
