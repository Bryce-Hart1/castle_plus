// Claude — Date 06/19/2026
// A tiny EventHandler that owns a bare fd (eventfd/signalfd/timerfd/inotify...)
// and forwards readability to a callback. Owns the fd: closes it on destroy, so
// the EventLoop that holds the watcher controls the fd's lifetime. Reused by the
// supervisor (signalfd + timerfd) and, later, config hot-reload (inotify).
#pragma once

#include <unistd.h>

#include <functional>
#include <utility>

#include "net/event_handler.h"

namespace castle {

class FdWatcher : public EventHandler {
public:
    FdWatcher(int fd, std::function<void()> on_readable)
        : fd_(fd), on_readable_(std::move(on_readable)) {}
    ~FdWatcher() override {
        if (fd_ >= 0) ::close(fd_);
    }

    int fd() const override { 
        return fd_; 
    }
    void on_readable() override {
        if (on_readable_) on_readable_();
    }
    void on_writable() override {}

private:
    int fd_;
    std::function<void()> on_readable_;
};

}  // namespace castle
