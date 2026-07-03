// Claude — Date 06/19/2026
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
#include <cstring>
#include <future>
#include <memory>
#include <sstream>

#include "net/fd_watcher.h"
#include "util/log.h"

namespace castle {

using std::chrono::seconds;
using std::chrono::steady_clock;

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
    loop_.add(std::make_unique<FdWatcher>(signal_fd_, [this] { on_sigchld(); }),
              EPOLLIN);
    loop_.add(std::make_unique<FdWatcher>(timer_fd_, [this] { on_tick(); }),
              EPOLLIN);

    // Launch everything before the thread starts spinning the loop.
    for (auto& s : services_){

    spawn(s);
    }

    thread_ = std::thread([this] { loop_.run(); });
    LOG_INFO("supervisor managing %zu service(s)", services_.size());
    return true;
}

void Supervisor::stop() {
    if (!thread_.joinable()) return;  // never started / already stopped
    // Kick off graceful shutdown on the supervisor thread; it stops the loop
    // once all children are reaped (or the grace deadline forces a SIGKILL).
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
    if (pid < 0) {
        LOG_ERROR("supervisor: fork '%s': %s", s.cfg.name.c_str(),
                  std::strerror(errno));
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
    s.last_start = steady_clock::now();
    LOG_INFO("supervisor: launched '%s' (pid %d)", s.cfg.name.c_str(), pid);
}

void Supervisor::schedule_restart(Service& s) {
    const auto now = steady_clock::now();
    const auto ran = std::chrono::duration_cast<seconds>(now - s.last_start)
                         .count();

    if (ran >= kStableSec) {
        // It stayed up a good while — treat this as a fresh failure, not a loop.
        s.current_backoff_sec = s.cfg.backoff_min_sec;
    } else if (s.current_backoff_sec == 0) {
        s.current_backoff_sec = s.cfg.backoff_min_sec;
    } else {
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
            s->state = ServiceState::Stopped;
        } else if (s->wants_restart) {
            s->wants_restart = false;
            s->current_backoff_sec = 0;
            spawn(*s);  // operator-requested: relaunch now
        } else if (s->cfg.autorestart) {
            schedule_restart(*s);
        } else {
            s->state = ServiceState::Failed;
        }
    }

    if (shutting_down_) finish_if_drained();
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
    if (!s) return false;
    if (s->pid > 0) {
        // Mark it, then SIGTERM; on_sigchld relaunches immediately.
        s->wants_restart = true;
        ::kill(s->pid, SIGTERM);
    } else {
        s->current_backoff_sec = 0;
        s->wants_restart = false;
        spawn(*s);
    }
    return true;
}

std::string Supervisor::render_services() const {
    std::ostringstream os;
    os << "NAME                 STATE     PID      HEALTH   RESTARTS\n";
    for (const auto& s : services_) {
        char health[8] = "-";
        if (s.cfg.health_port != 0)
            std::snprintf(health, sizeof(health), "%s",
                          s.health_known ? (s.healthy ? "up" : "down") : "?");
        char pidbuf[12] = "-";
        if (s.pid > 0) std::snprintf(pidbuf, sizeof(pidbuf), "%d", s.pid);

        char line[160];
        std::snprintf(line, sizeof(line), "%-20s %-9s %-8s %-8s %d\n",
                      s.cfg.name.c_str(), to_string(s.state), pidbuf, health,
                      s.restart_count);
        os << line;
    }
    return os.str();
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

bool Supervisor::tcp_health_ok(const std::string& host, uint16_t port,
                               int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
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
