#include "scheduler.h"
#include "catalog.h"
#include "sproc.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

static bool matchField(const std::string& field, int current_val) {
    if (field == "*") return true;

    // List support (1,2,3)
    if (field.find(',') != std::string::npos) {
        std::stringstream ss(field);
        std::string part;
        while (std::getline(ss, part, ',')) {
            if (matchField(part, current_val)) return true;
        }
        return false;
    }

    // Step support (*/5)
    if (field.rfind("*/", 0) == 0) {
        try {
            int step = std::stoi(field.substr(2));
            return (current_val % step) == 0;
        } catch (...) {
            return false;
        }
    }

    // Range support (1-5)
    size_t dash = field.find('-');
    if (dash != std::string::npos) {
        try {
            int start = std::stoi(field.substr(0, dash));
            int end = std::stoi(field.substr(dash + 1));
            return (current_val >= start && current_val <= end);
        } catch (...) {
            return false;
        }
    }

    // Exact value
    try {
        return std::stoi(field) == current_val;
    } catch (...) {
        return false;
    }
}

bool Scheduler::matchCron(const std::string& cron_expr, 
                          int min, int hr, int dom, int mon, int dow) {
    std::stringstream ss(cron_expr);
    std::string f_min, f_hr, f_dom, f_mon, f_dow;
    if (!(ss >> f_min >> f_hr >> f_dom >> f_mon >> f_dow)) {
        return false; // Invalid cron expression
    }

    return matchField(f_min, min) &&
           matchField(f_hr, hr) &&
           matchField(f_dom, dom) &&
           matchField(f_mon, mon) &&
           matchField(f_dow, dow);
}

void Scheduler::start(ExecutionEngine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;

    engine_ = engine;
    running_ = true;
    thread_ = std::thread(&Scheduler::runLoop, this);
    std::cout << "Job Scheduler started successfully." << std::endl;
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::cout << "Job Scheduler stopped." << std::endl;
}

void Scheduler::runLoop() {
    while (running_) {
        // Run every 5 seconds
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (!running_) break;

        // Get current local time
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm* local_tm = std::localtime(&time_t_now);

        int min = local_tm->tm_min;
        int hr = local_tm->tm_hour;
        int dom = local_tm->tm_mday;
        int mon = local_tm->tm_mon + 1; // tm_mon is 0-11
        int dow = local_tm->tm_wday;    // 0 is Sunday, 1-6

        // Format current minute string: YYYY-MM-DD HH:MM
        std::stringstream ss_min;
        ss_min << std::setfill('0') 
               << std::setw(4) << (local_tm->tm_year + 1900) << "-"
               << std::setw(2) << mon << "-"
               << std::setw(2) << dom << " "
               << std::setw(2) << hr << ":"
               << std::setw(2) << min;
        std::string current_minute_str = ss_min.str();

        auto jobs = Catalog::getInstance().getJobs();
        for (const auto& job : jobs) {
            if (job.status != "ENABLED") continue;

            // Check if it already ran in this specific minute
            if (!job.last_run.empty() && job.last_run.rfind(current_minute_str, 0) == 0) {
                continue;
            }

            // Check cron pattern match
            if (matchCron(job.cron_schedule, min, hr, dom, mon, dow)) {
                // Trigger job run asynchronously
                std::cout << "Triggering job: " << job.name << " (Cron: " << job.cron_schedule << ")" << std::endl;
                std::thread(&Scheduler::executeJob, this, job.id, job.name, job.command).detach();
            }
        }
    }
}

void Scheduler::executeJob(int job_id, const std::string& name, const std::string& command) {
    int64_t history_id = 0;
    if (!Catalog::getInstance().logJobStart(job_id, history_id)) {
        std::cerr << "Failed to log job start in Catalog." << std::endl;
        return;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Live SSE Streaming Sidecar Thread!
    std::atomic<bool> job_running{true};
    Catalog::getInstance().appendJobLog(history_id, "[Boot] Provisioning Engine Sandbox for Job: " + name + "\n");
    Catalog::getInstance().appendJobLog(history_id, "[Execute] Compiling pipeline: " + command + "\n");
    
    std::thread progress_thread([&]() {
        int seconds = 0;
        while (job_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            seconds++;
            if (job_running && seconds % 2 == 0) {
                Catalog::getInstance().appendJobLog(history_id, "[Progress] Scanning... elapsed " + std::to_string(seconds) + " seconds.\n");
            }
        }
    });

    std::string err_msg;
    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    
    bool ok = SQLRouter::routeAndExecute(command, engine_, cols, rows, err_msg);
    
    // Shutdown streaming sidecar
    job_running = false;
    progress_thread.join();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::string status = ok ? "SUCCESS" : "FAILED";
    std::string logs = "[Shutdown] Pipeline execution halted.\n";
    if (ok) {
        logs += "[Final] Status: SUCCESS\n[Final] Rows affected: " + std::to_string(rows.size()) + "\n";
    } else {
        logs += "[Final] Status: FAILED\n[Fatal] Error: " + err_msg + "\n";
    }
    
    Catalog::getInstance().appendJobLog(history_id, logs);
    Catalog::getInstance().logJobEnd(history_id, status, duration_ms, err_msg, ""); // Pass empty since we manually appended
    std::cout << "Finished job: " << name << " in " << duration_ms << "ms (Status: " << status << ")" << std::endl;
}
