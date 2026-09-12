#include "raft.h"
#include <chrono>
#include <random>

void RaftEngine::init(const std::vector<std::string>& peers, int my_port) {
    peers_ = peers;
    my_port_ = my_port;
    if (peers_.empty()) {
        std::cout << "Raft: No peers provided. Running in STANDALONE (Leader) mode." << std::endl;
        state_ = RaftState::LEADER;
        return;
    }
    
    // Dynamically announce ourselves to the mesh!
    for (const auto& peer : peers_) {
        std::string host = peer;
        if (host.find("http://") == 0) host = host.substr(7);
        if (host.find("https://") == 0) host = host.substr(8);
        
        httplib::Client cli(host);
        cli.set_connection_timeout(2, 0);
        std::string payload = "{\"peer\":\"http://localhost:" + std::to_string(my_port_) + "\"}";
        cli.Post("/api/raft/join", payload, "application/json");
    }
    
    running_ = true;
    election_thread_ = std::thread(&RaftEngine::runElectionTimer, this);
    std::cout << "Raft: Node started as FOLLOWER. Tracking " << peers_.size() << " peers." << std::endl;
}

void RaftEngine::addPeer(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(peers_.begin(), peers_.end(), peer) == peers_.end()) {
        peers_.push_back(peer);
        std::cout << "Raft: Dynamic Peer Discovered! Added " << peer << " to topology mesh." << std::endl;
        if (state_ == RaftState::LEADER) {
            std::cout << "Raft: Node transitioning from STANDALONE to DISTRIBUTED Leader." << std::endl;
        }
    }
}

void RaftEngine::shutdown() {
    running_ = false;
    if (election_thread_.joinable()) {
        election_thread_.join();
    }
}

void RaftEngine::runElectionTimer() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(1500, 3000); // 1.5s to 3.0s timeout
    
    while (running_) {
        int timeout_ms = dist(rng);
        
        // Wait for timeout or heartbeat interrupt (simplified polling for now)
        int waited = 0;
        bool heartbeat_received = false;
        
        while (waited < timeout_ms && running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            waited += 100;
            // In a real implementation, receiving a POST /api/raft/append_entries 
            // would set heartbeat_received = true and break this inner loop.
        }
        
        if (!running_) break;
        
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == RaftState::FOLLOWER && !heartbeat_received) {
            startElection();
        } else if (state_ == RaftState::LEADER) {
            broadcastHeartbeats();
        }
    }
}

void RaftEngine::startElection() {
    state_ = RaftState::CANDIDATE;
    current_term_++;
    std::cout << "Raft: Election timeout! Starting election for term " << current_term_ << std::endl;
    
    int votes = 1; // Vote for self
    
    // Broadcast RequestVote via HTTP...
    for (const auto& peer : peers_) {
        // ... (Simulated for now, would send POST /api/raft/vote)
        // If majority votes yes, state_ = RaftState::LEADER;
    }
    
    // If we win the election (or if we are the only node):
    if (votes > peers_.size() / 2) {
        state_ = RaftState::LEADER;
        std::cout << "Raft: Won election! Now acting as LEADER for term " << current_term_ << std::endl;
        broadcastHeartbeats();
    } else {
        state_ = RaftState::FOLLOWER; // Failed, revert
    }
}

void RaftEngine::broadcastHeartbeats() {
    // Send POST /api/raft/append_entries to all peers
    // std::cout << "Raft: Broadcasting heartbeats..." << std::endl;
}

bool RaftEngine::propose(const std::string& action, const std::string& payload_json) {
    if (state_ != RaftState::LEADER && !peers_.empty()) {
        std::cerr << "Raft: Cannot propose action, I am not the Leader!" << std::endl;
        return false;
    }
    
    if (peers_.empty()) {
        return true; // Standalone mode, auto-commit
    }
    
    int success_count = 1; // Auto-vote for self
    std::string raft_payload = "{\"action\":\"" + action + "\", \"data\":" + payload_json + "}";
    
    for (const auto& peer : peers_) {
        // Strip http:// if present for httplib
        std::string host = peer;
        if (host.find("http://") == 0) host = host.substr(7);
        if (host.find("https://") == 0) host = host.substr(8);
        
        httplib::Client cli(host);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        
        if (auto res = cli.Post("/api/raft/append_entries", raft_payload, "application/json")) {
            if (res->status == 200) {
                success_count++;
            }
        }
    }
    
    bool quorum_reached = success_count > (peers_.size() / 2);
    if (quorum_reached) {
        std::cout << "Raft: Proposal [" << action << "] committed to quorum (" << success_count << "/" << (peers_.size() + 1) << " nodes)." << std::endl;
    } else {
        std::cerr << "Raft: Proposal [" << action << "] failed to reach quorum!" << std::endl;
    }
    
    return quorum_reached;
}
