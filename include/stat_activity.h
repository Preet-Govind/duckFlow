#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <atomic>

struct SessionInfo {
    int session_id;
    std::string client_ip;
    std::string current_query;
    std::chrono::system_clock::time_point query_start_time;
    std::string status; // "IDLE", "ACTIVE"
    void* duckdb_conn; // Store duckdb_connection pointer
};

class StatActivityTracker {
public:
    static StatActivityTracker& getInstance() {
        static StatActivityTracker instance;
        return instance;
    }

    void addSession(int fd, const std::string& ip);
    void removeSession(int fd);
    void updateSession(int fd, const std::string& query, const std::string& status, void* duckdb_conn = nullptr);
    std::vector<SessionInfo> getActiveSessions();
    bool killSession(int session_id);
    
    void recordQuery(bool success) {
        if (success) {
            total_queries_executed_++;
        } else {
            total_failed_queries_++;
        }
    }
    
    uint64_t getTotalQueries() const { return total_queries_executed_.load(); }
    uint64_t getTotalFailed() const { return total_failed_queries_.load(); }

private:
    StatActivityTracker() = default;
    ~StatActivityTracker() = default;
    StatActivityTracker(const StatActivityTracker&) = delete;
    StatActivityTracker& operator=(const StatActivityTracker&) = delete;

    std::mutex mutex_;
    std::unordered_map<int, SessionInfo> sessions_;
    int next_session_id_ = 1;
    
    std::atomic<uint64_t> total_queries_executed_{0};
    std::atomic<uint64_t> total_failed_queries_{0};
};
