// refactored Bryce Hart Jul 16
// Supervisor implementation. Everything in here that isn't start()/stop() or the
// describe/restart entry points runs on the supervisor's own EventLoop thread,
// reached either from an fd callback (signalfd/timerfd) or a posted task — so it
// freely touches services_ without locks.
#include "supervisor/supervisor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <memory>
#include <sstream>
#include <string>

#include "net/fd_watcher.h"
#include "util/log.h"

namespace castle {

using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {

// Compact, human-scannable duration. Resolution drops as the span grows — at two
// hours nobody cares about the seconds, and the column stays narrow.
std::string human_duration(long long secs) {
    if (secs < 0) secs = 0;
    char buf[32];
    if (secs < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", secs);
    } else if (secs < 3600) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", secs / 60, secs % 60);
    } else if (secs < 86400) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", secs / 3600,
                      (secs % 3600) / 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%lldd %02lldh", secs / 86400,
                      (secs % 86400) / 3600);
    }
    return buf;
}

std::string wall_timestamp(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

}  // namespace

Supervisor::Supervisor(std::vector<ServiceConfig> configs) {
    services_.reserve(configs.size());
    for (auto& c : configs) {
        Service s;
        s.cfg = std::move(c);
        services_.push_back(std::move(s));
    }
}

Supervisor::~Supervisor() { stop(); }

bool Supervisor::start() {
    // signalfd for SIGCHLD. SIGCHLD must already be blocked process-wide (main
    // does that before any thread starts) so it lands here, not on a handler.
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    signal_fd_ = ::signalfd(-1, &chld, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ < 0) {
        LOG_ERROR("supervisor: signalfd: %s", std::strerror(errno));
        return false;
    }

    // timerfd: a steady 1s tick drives backoff relaunches and health checks.
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0) {
        LOG_ERROR("supervisor: timerfd: %s", std::strerror(errno));
        ::close(signal_fd_);
        signal_fd_ = -1;
        return false;
    }
    itimerspec its{};
    its.it_interval.tv_sec = kTickMs / 1000;
    its.it_interval.tv_nsec = (kTickMs % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (::timerfd_settime(timer_fd_, 0, &its, nullptr) < 0) {
        LOG_ERROR("supervisor: timerfd_settime: %s", std::strerror(errno));
        ::close(signal_fd_);
        ::close(timer_fd_);
        signal_fd_ = timer_fd_ = -1;
        return false;
    }

    // Hand both fds to the loop (FdWatcher owns/closes them). Level-triggered:
    // the callbacks drain fully each time.
    loop_.add(std::make_unique<FdWatcher>(signal_fd_, [this] { on_sigchld(); }), EPOLLIN);
    loop_.add(std::make_unique<FdWatcher>(timer_fd_, [this] { on_tick(); }), EPOLLIN);

    // Launch everything before the thread starts spinning the loop.
    for (auto& s : services_){
        spawn(s);
    }

    thread_ = std::thread([this]{ loop_.run(); }); //store in thread_

    LOG_INFO("supervisor managing %zu service(s)", services_.size());
    return true;
}

void Supervisor::stop() {
    if (!thread_.joinable()) return;  // never started / already stopped
    // Kick off graceful shutdown on the supervisor thread; it stops the loop
    // once all children are reaped (or the grace deadline forces a SIGKILL
    loop_.post([this] { begin_shutdown(); });
    thread_.join();
}

void Supervisor::spawn(Service& s) {
    // Build argv in the parent (allocations are safe here, not after fork).
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(s.cfg.exec.c_str()));
    for (auto& a : s.cfg.args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0){
        LOG_ERROR("supervisor: fork '%s': %s", s.cfg.name.c_str(), std::strerror(errno));
        schedule_restart(s);
        return;
    }

    if (pid == 0) {
        // CHILD. Only async-signal-safe calls until execv. Reset the inherited
        // signal mask so the backend starts with a clean slate (we run with
        // SIGCHLD/SIGINT/SIGTERM blocked).
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);

        if (!s.cfg.workdir.empty() && ::chdir(s.cfg.workdir.c_str()) != 0)
            _exit(126);

        ::execv(s.cfg.exec.c_str(), argv.data());
        _exit(127);  // execv only returns on failure
    }

    //parent
    s.pid = pid;
    s.state = ServiceState::Running;
    s.is_down = false;  // running again: the outage window is closed
    s.last_start = steady_clock::now();
    LOG_INFO("supervisor: launched '%s' (pid %d)", s.cfg.name.c_str(), pid);
}

void Supervisor::prune_failures(Service& s,
                                std::chrono::steady_clock::time_point now) {
    // The deque is append-only in time order, so everything still in the window
    // is a suffix — pop from the front until the oldest entry is inside it.
    while (!s.recent_failures.empty() &&
           now - s.recent_failures.front() > kFailureWindow) {
        s.recent_failures.pop_front();
    }
}

void Supervisor::mark_down(Service& s) {
    // Stamp the start of a permanent outage (quarantined, or failed with
    // autorestart off). Guarded so a second terminal transition without an
    // intervening spawn doesn't reset the clock and hide how long it's been down.
    if (s.is_down) return;
    s.is_down = true;
    s.down_since_mono = steady_clock::now();
    s.down_since_wall = std::chrono::system_clock::now();
}

void Supervisor::record_failure(Service& s, int status) {
    const auto now = steady_clock::now();

    ++s.failure_count;
    s.ever_failed = true;
    s.last_failure_mono = now;
    s.last_failure_wall = std::chrono::system_clock::now();

    char reason[64];
    if (WIFEXITED(status)) {
        std::snprintf(reason, sizeof(reason), "exit %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        const char* name = ::strsignal(sig);
        std::snprintf(reason, sizeof(reason), "signal %d (%s)", sig,
                      name ? name : "?");
    } else {
        std::snprintf(reason, sizeof(reason), "unknown");
    }
    s.last_failure_reason = reason;

    s.recent_failures.push_back(now);
    prune_failures(s, now);
}

void Supervisor::schedule_restart(Service& s) {
    const auto now = steady_clock::now();
    const auto ran = std::chrono::duration_cast<seconds>(now - s.last_start).count();

    if(ran >= kStableSec) {
        // It stayed up a good while — treat this as a fresh failure, not a loop.
        s.current_backoff_sec = s.cfg.backoff_min_sec;
    }else if(s.current_backoff_sec == 0) {
        s.current_backoff_sec = s.cfg.backoff_min_sec;
    }else{
        s.current_backoff_sec =
            std::min(s.current_backoff_sec * 2, s.cfg.backoff_max_sec);
    }

    ++s.restart_count;
    s.state = ServiceState::Backoff;
    s.next_restart = now + seconds(s.current_backoff_sec);
    LOG_WARN("supervisor: '%s' down; restart #%d in %ds", s.cfg.name.c_str(),
             s.restart_count, s.current_backoff_sec);
}

void Supervisor::on_sigchld() {
    // Drain the (coalescing) signalfd, then reap every dead child.
    signalfd_siginfo si;

    // discard; waitpid is the source of truth
    while (::read(signal_fd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si)));

    int status;
    pid_t pid;
    
    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
        Service* s = find_by_pid(pid);
        if (!s) continue;  // unknown child, but it's reaped (no zombie)
        s->pid = -1;

        if (WIFEXITED(status)) {
            LOG_INFO("supervisor: '%s' exited (code %d)", s->cfg.name.c_str(),
                     WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOG_INFO("supervisor: '%s' killed by signal %d", s->cfg.name.c_str(),
                     WTERMSIG(status));
        }
        s->health_known = false;

        if (shutting_down_) {
            // We asked it to stop — not a failure.
            s->state = ServiceState::Stopped;
        } else if (s->wants_restart) {
            // Operator-requested restart — also not a failure.
            s->wants_restart = false;
            s->current_backoff_sec = 0;
            spawn(*s);  // relaunch now
        } else {
            // Anything else is an unexpected exit. A clean `exit 0` counts too:
            // a supervised backend is meant to stay up, so terminating at all is
            // a failure of the service.
            record_failure(*s, status);

            if (!s->cfg.autorestart) {
                s->state = ServiceState::Failed;
                mark_down(*s);
            } else if (static_cast<int>(s->recent_failures.size()) >
                       kMaxFailures) {
                // Circuit breaker tripped: stop restarting and shout about it.
                s->state = ServiceState::Quarantined;
                mark_down(*s);
                LOG_ERROR(
                    "supervisor: '%s' QUARANTINED — %zu failures in the last "
                    "%lldh (limit %d); last: %s. NOT restarting; fix the cause "
                    "then run `restart %s` to clear.",
                    s->cfg.name.c_str(), s->recent_failures.size(),
                    static_cast<long long>(kFailureWindow.count()), kMaxFailures,
                    s->last_failure_reason.c_str(), s->cfg.name.c_str());
            } else {
                schedule_restart(*s);
            }
        }
    }

    if (shutting_down_){
    finish_if_drained();
    }
}

void Supervisor::on_tick() {
    uint64_t expirations;
    while (::read(timer_fd_, &expirations, sizeof(expirations)) ==
           static_cast<ssize_t>(sizeof(expirations)))
        ;  // drain

    const auto now = steady_clock::now();

    if (shutting_down_) {
        if (now >= kill_deadline_) {
            for (auto& s : services_)
                if (s.pid > 0) ::kill(s.pid, SIGKILL);
        }
        finish_if_drained();
        return;
    }

    for (auto& s : services_) {
        if (s.state == ServiceState::Backoff && now >= s.next_restart) {
            spawn(s);
        }
        if (s.pid > 0 && s.cfg.health_port != 0 &&
            std::chrono::duration_cast<seconds>(now - s.last_health).count() >=
                s.cfg.health_interval_sec) {
            bool ok = tcp_health_ok(s.cfg.health_host, s.cfg.health_port, 500);
            if (!s.health_known || ok != s.healthy) {
                LOG_INFO("supervisor: '%s' health %s", s.cfg.name.c_str(),
                         ok ? "up" : "down");
            }
            s.healthy = ok;
            s.health_known = true;
            s.last_health = now;
        }
    }
}

void Supervisor::begin_shutdown() {
    shutting_down_ = true;
    kill_deadline_ = steady_clock::now() + seconds(kGraceSec);
    int running = 0;
    for (auto& s : services_) {
        if (s.pid > 0) {
            ::kill(s.pid, SIGTERM);
            ++running;
        } else {
            s.state = ServiceState::Stopped;
        }
    }
    LOG_INFO("supervisor: stopping %d running service(s) (SIGTERM, %ds grace)",
             running, kGraceSec);
    finish_if_drained();
}

void Supervisor::finish_if_drained() {
    for (const auto& s : services_)
        if (s.pid > 0) return;  // still waiting on a child
    loop_.stop();
}

bool Supervisor::do_restart(const std::string& name) {
    Service* s = find_by_name(name);
    if(!s) return false;

    // An operator restart is the acknowledgement that clears the circuit
    // breaker: without dropping the window here a quarantined service would
    // re-trip on its very next failure and could never be brought back. The
    // cumulative failure_count is deliberately kept — only the rolling window
    // is reset, so the history stays visible in `backendstatus`.
    if (s->state == ServiceState::Quarantined) {
        LOG_WARN("supervisor: clearing quarantine on '%s' (%d failures on "
                 "record); operator-requested restart",
                 s->cfg.name.c_str(), s->failure_count);
    }
    s->recent_failures.clear();

    if(s->pid > 0){
        // Mark it, then SIGTERM; on_sigchld relaunches immediately.
        s->wants_restart = true;
        ::kill(s->pid, SIGTERM);
    }else{
        s->current_backoff_sec = 0;
        s->wants_restart = false;
        spawn(*s);
    }
    return true;
}

std::string Supervisor::render_services() const {
    std::ostringstream os;
    // STATE is 12 wide to fit the longest state name ("quarantined").
    os << "NAME                 STATE        PID      HEALTH   RESTARTS\n";
    for (const auto& s : services_) {
        char health[8] = "-";
        if (s.cfg.health_port != 0)
            std::snprintf(health, sizeof(health), "%s",
                          s.health_known ? (s.healthy ? "up" : "down") : "?");
        char pidbuf[12] = "-";
        if (s.pid > 0) std::snprintf(pidbuf, sizeof(pidbuf), "%d", s.pid);

        char line[160];
        std::snprintf(line, sizeof(line), "%-20s %-12s %-8s %-8s %d\n",
                      s.cfg.name.c_str(), to_string(s.state), pidbuf, health,
                      s.restart_count); //swap for std::format soon
        os << line;
    }
    return os.str();
}

// Per-backend runtime + failure history. Runs on the supervisor thread (posted
// from the control plane), so services_ is safe to read without locks.
std::string Supervisor::render_backend_status() const {
    const auto now = steady_clock::now();

    std::ostringstream os;
    os << "NAME                 STATE        UPTIME       FAILURES: 1h  24h  "
          "TOTAL   LAST FAILURE\n";

    int down = 0;
    for (const auto& s : services_) {
        // Uptime is only meaningful while a child is actually alive.
        std::string uptime = "-";
        if (s.pid > 0 && s.state == ServiceState::Running) {
            uptime = human_duration(
                std::chrono::duration_cast<seconds>(now - s.last_start).count());
        }

        // Count both buckets without mutating, so this stays a const view. The
        // 1h bucket is a subset of the deque, which only ever holds 24h.
        std::size_t in_hour = 0, in_day = 0;
        for (const auto& t : s.recent_failures) {
            const auto age = now - t;
            if (age <= kFailureWindow) ++in_day;
            if (age <= kRecentWindow) ++in_hour;
        }

        std::string last_failure = "-";
        if (s.ever_failed) {
            last_failure =
                human_duration(std::chrono::duration_cast<seconds>(
                                   now - s.last_failure_mono)
                                   .count()) +
                " ago (" + s.last_failure_reason + ")";
        }

        if (s.is_down) ++down;

        char line[256];
        std::snprintf(line, sizeof(line),
                      "%-20s %-12s %-12s %-13zu %-4zu %-7d %s\n",
                      s.cfg.name.c_str(), to_string(s.state), uptime.c_str(),
                      in_hour, in_day, s.failure_count, last_failure.c_str());
        os << line;
    }

    if (services_.empty()) os << "(no services configured)\n";

    // Anything permanently down gets called out with when the outage started and
    // how to recover — that's the part an operator has to act on, and burying it
    // in a table column would be easy to miss.
    if (down > 0) {
        os << "\nPERMANENTLY DOWN — not being restarted:\n";
        for (const auto& s : services_) {
            if (!s.is_down) continue;

            const long long down_secs =
                std::chrono::duration_cast<seconds>(now - s.down_since_mono)
                    .count();

            char line[512];
            if (s.state == ServiceState::Quarantined) {
                std::snprintf(
                    line, sizeof(line),
                    "  '%s' quarantined %s ago (at %s)\n"
                    "      tripped by %d failures within %lldh (limit %d)\n"
                    "      last failure: %s\n"
                    "      recover with: restart %s\n",
                    s.cfg.name.c_str(), human_duration(down_secs).c_str(),
                    wall_timestamp(s.down_since_wall).c_str(),
                    static_cast<int>(s.recent_failures.size()),
                    static_cast<long long>(kFailureWindow.count()),
                    kMaxFailures, s.last_failure_reason.c_str(),
                    s.cfg.name.c_str());
            } else {
                std::snprintf(
                    line, sizeof(line),
                    "  '%s' down %s ago (at %s)\n"
                    "      exited with autorestart disabled\n"
                    "      last failure: %s\n"
                    "      recover with: restart %s\n",
                    s.cfg.name.c_str(), human_duration(down_secs).c_str(),
                    wall_timestamp(s.down_since_wall).c_str(),
                    s.last_failure_reason.c_str(), s.cfg.name.c_str());
            }
            os << line;
        }
    }

    return os.str();
}

std::string Supervisor::describe_backend_status() {
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut = prom->get_future();
    loop_.post([this, prom] { prom->set_value(render_backend_status()); });
    if (fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready)
        return fut.get();
    return "supervisor not responding\n";
}

Service* Supervisor::find_by_pid(pid_t pid) {
    for (auto& s : services_)
        if (s.pid == pid) return &s;
    return nullptr;
}

Service* Supervisor::find_by_name(const std::string& name) {
    for (auto& s : services_)
        if (s.cfg.name == name) return &s;
    return nullptr;
}

bool Supervisor::tcp_health_ok(const std::string& host, uint16_t port, int timeout_ms){

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0){
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1){
        ::close(fd);
        return false;
    }

    bool ok = false;
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                err == 0)
                ok = true;
        }
    }
    ::close(fd);
    return ok;
}

std::string Supervisor::describe_services() {
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut = prom->get_future();
    loop_.post([this, prom] { prom->set_value(render_services()); });
    if (fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready)
        return fut.get();
    return "supervisor not responding\n";
}

bool Supervisor::restart_service(const std::string& name) {
    auto prom = std::make_shared<std::promise<bool>>();
    auto fut = prom->get_future();
    loop_.post([this, name, prom] { prom->set_value(do_restart(name)); });
    if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready)
        return fut.get();
    return false;
}

}  // namespace castle
