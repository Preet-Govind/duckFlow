#include "stat_activity.h"

void StatActivityTracker::addSession(int fd, const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    SessionInfo info;
    info.session_id = next_session_id_++;
    info.client_ip = ip;
    info.current_query = "";
    info.query_start_time = std::chrono::system_clock::now();
    info.status = "IDLE";
    info.duckdb_conn = nullptr;
    sessions_[fd] = info;
}

void StatActivityTracker::removeSession(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(fd);
}

void StatActivityTracker::updateSession(int fd, const std::string& query, const std::string& status, void* duckdb_conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(fd);
    if (it != sessions_.end()) {
        it->second.current_query = query;
        it->second.query_start_time = std::chrono::system_clock::now();
        it->second.status = status;
        if (duckdb_conn) {
            it->second.duckdb_conn = duckdb_conn;
        }
    }
}

std::vector<SessionInfo> StatActivityTracker::getActiveSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionInfo> active;
    for (const auto& pair : sessions_) {
        active.push_back(pair.second);
    }
    return active;
}

#include "duckdb_loader.h"

bool StatActivityTracker::killSession(int session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sessions_) {
        if (pair.second.session_id == session_id) {
            if (pair.second.duckdb_conn) {
                DuckDbLoader::getInstance().duckdb_interrupt_fn((duckdb_connection)pair.second.duckdb_conn);
                pair.second.status = "CANCELED";
                return true;
            }
        }
    }
    return false;
}
