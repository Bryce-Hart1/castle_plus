/**  Bryce Hart — Date 09/18/2026
* @brief Internet connectivity watch. castle+ is the front door for everything behind
* it, so losing the uplink is the one failure that makes the whole box useless
* while every local health check still says "fine". A small thread probes a couple 
* of well-known anycast addresses on an interval and logs CRITICAL the
* moment they all stop answering (once per outage, not once per tick), then
* INFO when the link comes back. The latest result is published through
* internet_state() for `systemstatus`. */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace castle {

enum class NetState {
    Unknown,  // watcher off, or first probe not finished yet
    Up,
    Down
};

// Latest probe result. Written only by the ConnectivityMonitor thread; read by
// the control plane.
inline std::atomic<NetState>& internet_state() {
    static std::atomic<NetState> state{NetState::Unknown};
    return state;
}

// True if ANY probe target accepts a TCP connection within timeout_ms. Blocks
// for up to (targets x timeout_ms) when the link is down.
bool internet_reachable(int timeout_ms);

class ConnectivityMonitor {
public:
    explicit ConnectivityMonitor(std::chrono::seconds interval);
    ~ConnectivityMonitor();

    ConnectivityMonitor(const ConnectivityMonitor&) = delete;
    ConnectivityMonitor& operator=(const ConnectivityMonitor&) = delete;

    // Spawn the watch thread. Call after signals are blocked in main so the
    // thread inherits the mask.
    void start();
    // Wake the thread out of its wait and join it. Idempotent.
    void stop();

private:
    void run();

    std::chrono::seconds interval_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;  // guarded by m_
    std::thread thread_;
};

}  //namespace castle
