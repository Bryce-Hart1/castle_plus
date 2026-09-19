// Bryce Hart — Date 09/18/2026
// Connectivity watch implementation. Probes are plain TCP connects to IP
// literals: no DNS (a blocking resolver would turn "DNS is down" into "the
// thread hung"), and port 443 because it's the egress port firewalls are least
// likely to block.

#include "../net/connectivity.h"

#include <cstdint>

#include "../net/socket.h"
#include "../util/log.h"

namespace castle {

namespace {

struct ProbeTarget {
    const char* host;
    uint16_t port;
};

constexpr ProbeTarget kProbeTargets[] = {
    {"1.1.1.1", 443},  // Cloudflare
    {"8.8.8.8", 443},  // Google
};

constexpr int kProbeTimeoutMs = 2000;

}  // namespace

bool internet_reachable(int timeout_ms) {
    for (const auto& t : kProbeTargets) {
        if (tcp_probe(t.host, t.port, timeout_ms)) return true;
    }
    return false;
}

ConnectivityMonitor::ConnectivityMonitor(std::chrono::seconds interval) : interval_(interval) {}

ConnectivityMonitor::~ConnectivityMonitor() {
    stop();
}

void ConnectivityMonitor::start() {
    thread_ = std::thread([this] { run(); });
    LOG_INFO("internet watch: probing every %llds",
             static_cast<long long>(interval_.count()));
}

void ConnectivityMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ConnectivityMonitor::run() {
    using std::chrono::steady_clock;
    steady_clock::time_point down_since{};

    for (;;) {
        const bool up = internet_reachable(kProbeTimeoutMs);
        const NetState prev = internet_state().load(std::memory_order_relaxed);

        // Log transitions only: one CRITICAL per outage, one INFO per recovery.
        if (!up && prev != NetState::Down) {
            down_since = steady_clock::now();
            LOG_CRITICAL("internet connectivity lost: no TCP response from "
                         "1.1.1.1:443 or 8.8.8.8:443");
        } else if (up && prev == NetState::Down) {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                steady_clock::now() - down_since).count();
            LOG_INFO("internet connectivity restored (down ~%llds)",
                     static_cast<long long>(secs));
        }
        internet_state().store(up ? NetState::Up : NetState::Down,
                               std::memory_order_relaxed);

        std::unique_lock<std::mutex> lock(m_);
        if (cv_.wait_for(lock, interval_, [this] { return stop_; })) return;
    }
}

}  // namespace castle
