// Claude — Date 06/19/2026 Peer reviewed Bryce Hart 7-04-26
// The control plane. Runs on its own thread, off the data path, listening on a
// Unix-domain socket so admin traffic never competes with request serving and
// can't be reached from the network. Two ways it talks to the workers:
//   * read-only queries (status)  -> read each loop's lock-free atomic counters
//   * loop-touching work (health) -> EventLoop::post() a task + eventfd wakeup,
//                                     the loop runs it and acks back (async
//                                     round-trip). State-changing supervisor
//                                     commands (restart backend X, reload) plug
//                                     in here in M3.
// Because it's its own thread and not on the reactor, it's allowed to block.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace castle {

class EventLoop;
class Supervisor;

std::string systemStatusHelper();

// Per-backend uptime, failure counts and quarantine state. Takes the supervisor
// (may be null) rather than reading service state directly: that state lives on
// the supervisor thread, so the call has to be marshalled onto it.
std::string backendStatusHelper(Supervisor* supervisor);

class ControlServer {
public:
    // `loops` are borrowed (not owned) for stats/health; `on_shutdown` is
    // invoked when an admin issues `shutdown`. `supervisor` is optional (may be
    // null) — when present, the `services`/`restart` commands are enabled.
    ControlServer(std::string socket_path, std::vector<EventLoop*> loops, std::function<void()> on_shutdown, Supervisor* supervisor = nullptr);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Bind the socket and spawn the admin thread. Returns false on failure.
    bool start();
    // Stop the admin thread, close the socket, unlink the path. Idempotent.
    void stop();

private:
    void run();                  // thread body: poll(listen, stop) accept loop
    void handle_client(int cfd);  // blocking line protocol for one admin client
    std::string dispatch(const std::string& line, bool& close_session);
    std::string cmd_status() const;
    std::string cmd_health();
    std::string cmd_errors();  // new log lines since the last pull

    std::string socket_path_;
    std::vector<EventLoop*> loops_;
    std::function<void()> on_shutdown_;
    Supervisor* supervisor_ = nullptr;  // optional; enables services/restart
    int listen_fd_ = -1;
    int stop_fd_ = -1;  // eventfd used to break poll() on shutdown
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace castle
