// Claude — Date 06/19/2026
// Process supervisor: launches the app backends, reaps them (signalfd/SIGCHLD),
// restarts with exponential backoff, and runs periodic TCP health checks. It is
// itself just an EventLoop on its own thread — the same reactor the network
// workers use — so ALL service state is touched on one thread and needs no
// locks. The control plane reaches it through EventLoop::post() (describe /
// restart), exactly the async round-trip pattern from the control milestone.
//
// Lifetime hierarchy: systemd -> castle++ -> these backends. On shutdown the
// supervisor SIGTERMs its children, waits a grace period, then SIGKILLs.
#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "net/event_loop.h"
#include "supervisor/service.h"

namespace castle {

class Supervisor {
public:
    explicit Supervisor(std::vector<ServiceConfig> configs);
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // Set up signalfd/timerfd, launch services, spawn the supervisor thread.
    bool start();
    // Graceful stop: SIGTERM children, grace period, SIGKILL stragglers, join.
    void stop();

    // Control-plane API. Thread-safe: each posts a task to the supervisor loop
    // and waits for the result, so the work runs on the supervisor's own thread.
    std::string describe_services();
    // Per-backend uptime / failure history / quarantine state (`backendstatus`).
    std::string describe_backend_status();
    bool restart_service(const std::string& name);

private:
    // ---- all of the following run ON the supervisor thread ----
    void on_sigchld();          // reap + decide restart
    void on_tick();             // backoff relaunches + health checks
    void spawn(Service& s);     // fork/execv one service
    void schedule_restart(Service& s);
    void begin_shutdown();      // SIGTERM all, arm the kill deadline
    void finish_if_drained();   // stop the loop once no children remain
    bool do_restart(const std::string& name);
    std::string render_services() const;
    std::string render_backend_status() const;
    Service* find_by_pid(pid_t pid);
    Service* find_by_name(const std::string& name);

    // ---- failure accounting / circuit breaker ----
    // Record one unexpected exit (`status` as returned by waitpid) against `s`.
    void record_failure(Service& s, int status);
    // Stamp the start of a permanent outage (quarantine / failed-no-restart).
    static void mark_down(Service& s);
    // Drop failures that have aged out of the rolling window. After this call
    // s.recent_failures.size() is the in-window failure count.
    static void prune_failures(Service& s,
                               std::chrono::steady_clock::time_point now);

    static bool tcp_health_ok(const std::string& host, uint16_t port,
                              int timeout_ms);

    std::vector<Service> services_;
    EventLoop loop_;
    std::thread thread_;

    int signal_fd_ = -1;  // owned by an FdWatcher in loop_ (don't close here)
    int timer_fd_ = -1;   // ditto

    bool shutting_down_ = false;
    std::chrono::steady_clock::time_point kill_deadline_{};

    static constexpr int kGraceSec = 5;    // SIGTERM -> SIGKILL window
    static constexpr int kTickMs = 1000;   // supervisor tick granularity
    static constexpr int kStableSec = 10;  // ran this long => reset backoff

    // Failure circuit breaker. More than kMaxFailures failures inside
    // kFailureWindow quarantines the backend: it is logged at ERROR (mirrored to
    // the file log as HIGH) and NOT restarted again, because a service failing
    // this often is broken in a way restarting cannot fix — and an endless
    // restart loop burns CPU and buries the real cause in log noise. An operator
    // clears it with `restart <name>` once the underlying problem is addressed.
    static constexpr int kMaxFailures = 5;
    static constexpr std::chrono::hours kFailureWindow{24};

    // A shorter bucket reported alongside the breaker's window, so "6 failures"
    // reads differently when they all happened in the last few minutes than when
    // they're spread across the day. Display only — the breaker ignores it.
    static constexpr std::chrono::hours kRecentWindow{1};
};

}  // namespace castle
