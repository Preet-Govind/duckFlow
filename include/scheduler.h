#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "engine.h"

class Scheduler {
public:
    static Scheduler& getInstance() {
        static Scheduler instance;
        return instance;
    }

    void start(ExecutionEngine* engine);
    void stop();
    
    // Evaluation helper
    static bool matchCron(const std::string& cron_expr, 
                          int min, int hr, int dom, int mon, int dow);

private:
    Scheduler() = default;
    ~Scheduler() { stop(); }
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void runLoop();
    void executeJob(int job_id, const std::string& name, const std::string& command);

    std::thread thread_;
    std::atomic<bool> running_{false};
    ExecutionEngine* engine_ = nullptr;
    std::mutex mutex_;
};
