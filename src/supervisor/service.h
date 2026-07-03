// Runtime state for one managed backend (the config plus what's happening now).
// Touched only on the supervisor thread, so plain fields — no atomics/locks.
#pragma once

#include <sys/types.h>

#include <chrono>

#include "config/service_config.h"

namespace castle {

enum class ServiceState {
    Stopped,  // not running, no restart pending (e.g. during shutdown)
    Running,  // has a live child pid
    Backoff,  // crashed, waiting out the backoff before relaunch
    Failed,   // exited and autorestart is off
};

inline const char* to_string(ServiceState s) {
    switch (s) {
        case ServiceState::Stopped: return "stopped";
        case ServiceState::Running: return "running";
        case ServiceState::Backoff: return "backoff";
        case ServiceState::Failed:  return "failed";
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

    std::chrono::steady_clock::time_point next_restart{};  // when in Backoff
    std::chrono::steady_clock::time_point last_start{};
    std::chrono::steady_clock::time_point last_health{};
};

}  // namespace castle
