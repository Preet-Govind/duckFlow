#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <iostream>
#include "httplib.h"

enum class RaftState { FOLLOWER, CANDIDATE, LEADER };

class RaftEngine {
public:
    static RaftEngine& getInstance() {
        static RaftEngine instance;
        return instance;
    }

    void init(const std::vector<std::string>& peers, int my_port);
    void shutdown();
    
    // Dynamic cluster modification
    void addPeer(const std::string& peer);
    int getPort() const { return my_port_; }
    
    // Propose an action to the cluster
    bool propose(const std::string& action, const std::string& payload_json);
    
    // Accessors
    RaftState getState() const { return state_; }
    std::string getLeader() const { return current_leader_; }

private:
    RaftEngine() = default;
    ~RaftEngine() { shutdown(); }

    std::vector<std::string> peers_;
    int my_port_ = 8081;
    
    std::mutex mutex_;
    RaftState state_ = RaftState::FOLLOWER;
    int current_term_ = 0;
    std::string current_leader_ = "";
    
    std::atomic<bool> running_{false};
    std::thread election_thread_;
    
    void runElectionTimer();
    void startElection();
    void broadcastHeartbeats();
};
