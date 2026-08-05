// Peer reviewed 7-5-26 Bryce Hart
// Runtime state for one managed backend (the config plus what's happening now).
// Touched only on the supervisor thread, so plain fields — no atomics/locks.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <deque>
#include <string>

#include "config/service_config.h"

namespace castle {

enum class ServiceState {
    Stopped,      // not running, no restart pending (e.g. during shutdown)
    Running,      // has a live child pid
    Backoff,      // crashed, waiting out the backoff before relaunch
    Failed,       // exited and autorestart is off
    Quarantined,  // tripped the failure circuit breaker; will NOT be restarted
                  // until an operator clears it with `restart <name>`
};

inline const char* to_string(ServiceState s) {
    switch (s) {
        case ServiceState::Stopped:     return "stopped";
        case ServiceState::Running:     return "running";
        case ServiceState::Backoff:     return "backoff";
        case ServiceState::Failed:      return "failed";
        case ServiceState::Quarantined: return "quarantined";
    }
    return "?";
}

struct Service {
    ServiceConfig cfg;

    pid_t pid = -1;
    ServiceState state = ServiceState::Stopped;
    int restart_count = 0;
    int current_backoff_sec = 0;
    bool wants_restart = false;  // operator asked for an immediate restart

    bool health_known = false;
    bool healthy = false;

    // ---- failure history (drives `backendstatus` and the circuit breaker) ----
    int failure_count = 0;  // total failures since castle started

    // Instants of the failures still inside the breaker's rolling window, oldest
    // first. steady_clock on purpose: an NTP step or a DST change must not be
    // able to widen the window (hiding a crash loop) or collapse it (quarantining
    // a healthy service). Pruned from the front as entries age out, so its size
    // IS the current in-window failure count.
    std::deque<std::chrono::steady_clock::time_point> recent_failures;

    // Last failure, for display. Both clocks: system_clock to print a wall time
    // an operator can correlate with other logs, steady_clock to compute "how
    // long ago" without trusting the wall clock.
    bool ever_failed = false;
    std::chrono::system_clock::time_point last_failure_wall{};
    std::chrono::steady_clock::time_point last_failure_mono{};
    std::string last_failure_reason;  // "exit 1" / "signal 9 (SIGKILL)"

    // When this service went permanently down — quarantined by the breaker, or
    // Failed with autorestart off. This is the "down since" an operator needs:
    // how long the outage has been running, and a wall time to line up against
    // other logs. Set on entering either state, cleared on the next spawn.
    // Only meaningful while state is Quarantined or Failed.
    bool is_down = false;
    std::chrono::system_clock::time_point down_since_wall{};
    std::chrono::steady_clock::time_point down_since_mono{};

    std::chrono::steady_clock::time_point next_restart{};  // when in Backoff
    std::chrono::steady_clock::time_point last_start{};
    std::chrono::steady_clock::time_point last_health{};
};

}  // namespace castle
