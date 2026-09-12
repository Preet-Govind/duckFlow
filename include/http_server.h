#pragma once

#include "catalog.h"
#include <string>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include "engine.h"

struct RateLimitTracker {
    std::chrono::steady_clock::time_point last_reset;
    int queries_this_second = 0;
};

class HttpServer {
public:
    static HttpServer& getInstance() {
        static HttpServer instance;
        return instance;
    }

    void start(int port, const std::string& web_dir, ExecutionEngine* engine);
    void stop();

private:
    HttpServer() = default;
    ~HttpServer() { stop(); }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void runLoop();

    int port_ = 8080;
    std::string web_dir_;
    ExecutionEngine* engine_ = nullptr;
    std::mutex rate_limit_mutex_;
    std::map<std::string, RateLimitTracker> rate_limits_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    void* svr_ptr_ = nullptr;
};
