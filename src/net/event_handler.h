// Claude — Date 06/19/2026
// The single interface every fd in the reactor implements (listeners,
// connections, later: the signalfd, inotify fd, backend sockets...). The
// EventLoop stores a pointer to one of these in epoll_event.data.ptr and calls
// back on readiness. This is the seam that keeps the loop ignorant of what any
// particular fd actually does.
#pragma once

#include <chrono>

namespace castle {

class EventHandler {
public:
    virtual ~EventHandler() = default;

    // The fd this handler owns / is registered under.
    virtual int fd() const = 0;

    // epoll reported the fd readable / writable. Implementations must fully
    // drain (edge-triggered) before returning, and may call
    // EventLoop::request_close(fd()) to schedule their own teardown.
    virtual void on_readable() = 0;
    virtual void on_writable() = 0;

    // EPOLLERR / EPOLLHUP. Default: nothing — most handlers just request_close.
    virtual void on_error() {}

    // Called on each idle-timeout sweep tick (if the loop has them enabled).
    // Handlers that care compare `now` against their last-activity time and
    // close themselves when idle too long. Default: no timeout (listeners,
    // signalfd, timerfd, etc.).
    virtual void check_timeout(std::chrono::steady_clock::time_point /*now*/) {}
};

}  // namespace castle
