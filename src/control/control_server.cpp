// Control-plane implementation. Deliberately simple/blocking: this thread is off
// the data path, so it can afford to block in poll()/read() without hurting
// throughput. poll() always watches stop_fd_ alongside the real fd so shutdown
// interrupts even a parked admin session.
#include "control/control_server.h"
#include "util/autocorrect.hpp"

#include <poll.h>
#include <sys/eventfd.h> // Linux-only (runs in docker / on the server)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "net/event_loop.h"
#include "supervisor/supervisor.h"
#include "util/log.h"

namespace castle {

namespace {

// Best-effort blocking write of the whole buffer. Admin traffic is tiny; we
// just want correctness, not throughput. Errors end the session.
void write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

}  // namespace

ControlServer::ControlServer(std::string socket_path,
                             std::vector<EventLoop*> loops,
                             std::function<void()> on_shutdown,
                             Supervisor* supervisor)
    : socket_path_(std::move(socket_path)),
      loops_(std::move(loops)),
      on_shutdown_(std::move(on_shutdown)),
      supervisor_(supervisor) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("control: socket: %s", std::strerror(errno));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("control: socket path too long: %s", socket_path_.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // Clear any stale socket left by a previous run, then lock it down to the
    // owning user (it's an admin channel).
    ::unlink(socket_path_.c_str());
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
        0) {
        LOG_ERROR("control: bind %s: %s", socket_path_.c_str(),
                  std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    ::chmod(socket_path_.c_str(), 0600);

    if (::listen(listen_fd_, 8) < 0) {
        LOG_ERROR("control: listen: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }

    stop_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) {
        LOG_ERROR("control: eventfd: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("control plane listening on unix:%s", socket_path_.c_str());
    return true;
}

void ControlServer::stop() {
    // Idempotent: only the first caller that flips running_ true->false runs.
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    if (stop_fd_ >= 0) {
        uint64_t one = 1;
        ssize_t r = ::write(stop_fd_, &one, sizeof(one));
        (void)r;
    }
    if (thread_.joinable()) thread_.join();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (stop_fd_ >= 0) {
        ::close(stop_fd_);
        stop_fd_ = -1;
    }
    if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

void ControlServer::run() {
    while (running_.load(std::memory_order_acquire)) {
        pollfd fds[2];
        fds[0] = {listen_fd_, POLLIN, 0};
        fds[1] = {stop_fd_, POLLIN, 0};

        int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("control: poll: %s", std::strerror(errno));
            break;
        }
        if (fds[1].revents & POLLIN) break;  // stop requested

        if (fds[0].revents & POLLIN) {
            int cfd = ::accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) continue;  // EINTR/EAGAIN/transient — try again
            handle_client(cfd);
            ::close(cfd);
        }
    }
}

void ControlServer::handle_client(int cfd) {
    write_all(cfd, "castle++ control — type 'help'\n");

    std::string inbuf;
    char buf[1024];
    while (running_.load(std::memory_order_acquire)) {
        pollfd fds[2];
        fds[0] = {cfd, POLLIN, 0};
        fds[1] = {stop_fd_, POLLIN, 0};

        int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) break;       // server shutting down
        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t n = ::read(cfd, buf, sizeof(buf));
        if (n <= 0) break;  // client closed or error
        inbuf.append(buf, static_cast<size_t>(n));

        size_t nl;
        while ((nl = inbuf.find('\n')) != std::string::npos) {
            std::string line = inbuf.substr(0, nl);
            inbuf.erase(0, nl + 1);

            bool close_session = false;
            std::string resp = dispatch(line, close_session);
            if (!resp.empty()) write_all(cfd, resp);
            if (close_session) return;
        }
    }
}

std::string ControlServer::dispatch(const std::string& raw, bool& close_session) {
    // Trim surrounding whitespace; first token is the command.
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string line = raw.substr(b, e - b + 1);
    std::string cmd = line.substr(0, line.find_first_of(" \t"));
    
    for (char& c : cmd) c = static_cast<char>(std::tolower(c));

    // Everything after the first token is the (case-preserved) argument.
    std::string arg;
    size_t sp = line.find_first_of(" \t");
    if (sp != std::string::npos) {
        size_t a = line.find_first_not_of(" \t", sp);
        if (a != std::string::npos) arg = line.substr(a);
    }
    /**
     * bryce hart 7-1-26
     * list of commands that is ran via connection to castle
     */
    if (cmd == "help") {
        return "commands:\n"
               "  help            this text\n"
               "  ping            liveness check -> pong\n"
               "  status          aggregated counters across all loops\n"
               "  health          probe every loop via the async round-trip\n"
               "  services        list supervised backends\n"
               "  restart <name>  restart a supervised backend\n"
               "  errors          log lines since the last pull (needs --log-file)\n"
               "  shutdown        gracefully stop castle++\n"
               "  exit            close this admin session\n";
    }
    if (cmd == "ping") return "pong\n";
    if (cmd == "status" || cmd == "stats") return cmd_status();
    if (cmd == "health") return cmd_health();
    if (cmd == "errors") return cmd_errors();
    if (cmd == "services" || cmd == "backends") {
        return supervisor_ ? supervisor_->describe_services()
                           : "no supervisor configured\n";
    }
    if (cmd == "restart") {
        if (!supervisor_) return "no supervisor configured\n";
        if (arg.empty()) return "usage: restart <service>\n";
        return supervisor_->restart_service(arg)
                   ? "restarting '" + arg + "'\n"
                   : "no such service: " + arg + "\n";
    }
    if (cmd == "shutdown") {
        if (on_shutdown_) on_shutdown_();
        return "shutting down castle++...\n";
    }
    if (cmd == "exit" || cmd == "quit") {
        close_session = true;
        return "bye\n";
    }
    return "unknown command: " + cmd + " (try 'help')\n";
}

std::string ControlServer::cmd_errors() {
    // First call returns the whole log; each later call returns only what's new.
    std::vector<std::string> lines = castle::log_pull_new();
    if (lines.empty()) return "no new log entries since last pull\n";
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

std::string ControlServer::cmd_status() const {
    uint64_t active = 0, total = 0, bin = 0, bout = 0;
    for (auto* lp : loops_) {
        const LoopStats& s = lp->stats();
        active += s.active_connections.load(std::memory_order_relaxed);
        total += s.total_connections.load(std::memory_order_relaxed);
        bin += s.bytes_in.load(std::memory_order_relaxed);
        bout += s.bytes_out.load(std::memory_order_relaxed);
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "loops:              %zu\n"
                  "active_connections: %llu\n"
                  "total_connections:  %llu\n"
                  "bytes_in:           %llu\n"
                  "bytes_out:          %llu\n",
                  loops_.size(), static_cast<unsigned long long>(active),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(bin),
                  static_cast<unsigned long long>(bout));
    return buf;
}

std::string ControlServer::cmd_health() {
    using namespace std::chrono;

    // Claude — Date 06/19/2026
    // The async round-trip in action: hand each loop a task (set a promise),
    // wake it via its eventfd, then wait for the acks. A wedged loop won't
    // fulfil its promise and shows up as a timeout.
    const auto t0 = steady_clock::now();
    std::vector<std::future<void>> futures;
    futures.reserve(loops_.size());
    for (auto* lp : loops_) {
        auto prom = std::make_shared<std::promise<void>>();
        futures.push_back(prom->get_future());
        lp->post([prom] { prom->set_value(); });
    }

    const auto deadline = t0 + milliseconds(500);
    size_t responded = 0;
    for (auto& f : futures) {
        if (f.wait_until(deadline) == std::future_status::ready) {
            try {
                f.get();
                ++responded;
            } catch (...) {
                // broken promise (loop torn down) — counts as unresponsive
            }
        }
    }

    const auto rtt_us =
        duration_cast<microseconds>(steady_clock::now() - t0).count();
    char buf[160];
    std::snprintf(
        buf, sizeof(buf),
        "health: %zu/%zu event loops responsive (task round-trip in %lldus)\n",
        responded, loops_.size(), static_cast<long long>(rtt_us));
    return buf;
}

}  // namespace castle
