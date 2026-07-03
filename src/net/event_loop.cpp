// Claude — Date 06/19/2026
// The heart of the reactor. Edge-triggered epoll: handlers MUST drain their fd
// to EAGAIN, because epoll only re-notifies on a new edge. Handler destruction
// is deferred (process_pending_close) so a callback can safely ask to close its
// own fd mid-dispatch.
#include "net/event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "net/fd_watcher.h"
#include "util/log.h"

namespace castle {

EventLoop::EventLoop() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_ERROR("epoll_create1 failed: %s", std::strerror(errno));
        return;
    }
    // Claude — Date 06/19/2026
    // eventfd registered with data.ptr == nullptr is our cross-thread doorbell:
    // stop() (and future task hand-off) writes it to break epoll_wait.
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        LOG_ERROR("eventfd failed: %s", std::strerror(errno));
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;  // level-triggered is fine for the doorbell
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
        LOG_ERROR("epoll_ctl(wake_fd) failed: %s", std::strerror(errno));
    }
}

EventLoop::~EventLoop() {
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

void EventLoop::add(std::unique_ptr<EventHandler> handler, uint32_t events) {
    const int fd = handler->fd();
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = handler.get();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(ADD fd=%d) failed: %s", fd, std::strerror(errno));
        return;  // handler unique_ptr drops here -> fd closed
    }
    handlers_[fd] = std::move(handler);
}

void EventLoop::update(int fd, uint32_t events) {
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) return;
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = it->second.get();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(MOD fd=%d) failed: %s", fd, std::strerror(errno));
    }
}

void EventLoop::request_close(int fd) { pending_close_.insert(fd); }

void EventLoop::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        tasks_.push_back(std::move(task));
    }
    // Ring the doorbell so the loop wakes and drains the queue promptly.
    uint64_t one = 1;
    ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    (void)r;
}

void EventLoop::enable_idle_timeouts(int sweep_interval_ms) {
    timeout_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timeout_fd_ < 0) {
        LOG_ERROR("enable_idle_timeouts: timerfd_create: %s",
                  std::strerror(errno));
        return;
    }
    itimerspec its{};
    its.it_interval.tv_sec = sweep_interval_ms / 1000;
    its.it_interval.tv_nsec = (sweep_interval_ms % 1000) * 1000000L;
    its.it_value = its.it_interval;
    if (::timerfd_settime(timeout_fd_, 0, &its, nullptr) < 0) {
        LOG_ERROR("enable_idle_timeouts: timerfd_settime: %s",
                  std::strerror(errno));
        ::close(timeout_fd_);
        timeout_fd_ = -1;
        return;
    }
    // FdWatcher owns/closes the fd; level-triggered, drained in the callback.
    add(std::make_unique<FdWatcher>(timeout_fd_, [this] { sweep_timeouts(); }),
        EPOLLIN);
}

void EventLoop::sweep_timeouts() {
    uint64_t expirations;
    while (::read(timeout_fd_, &expirations, sizeof(expirations)) ==
           static_cast<ssize_t>(sizeof(expirations)))
        ;  // drain

    // A timed-out handler only schedules its own (deferred) close, so iterating
    // handlers_ here is safe — the map isn't mutated mid-loop.
    auto now = std::chrono::steady_clock::now();
    for (auto& [fd, handler] : handlers_) handler->check_timeout(now);
}

void EventLoop::run_pending_tasks() {
    // Swap the queue out under the lock, then run tasks unlocked so a task may
    // itself post() without deadlocking.
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        local.swap(tasks_);
    }
    for (auto& task : local) task();
}

void EventLoop::process_pending_close() {
    for (int fd : pending_close_) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);  // destroys handler -> Socket closes the fd
    }
    pending_close_.clear();
}

void EventLoop::drain_wakeup() {
    uint64_t counter;
    while (::read(wake_fd_, &counter, sizeof(counter)) > 0) {
        // keep draining
    }
}

void EventLoop::stop() {
    running_.store(false, std::memory_order_release);
    uint64_t one = 1;
    // Ignore result: a failed wake just means the loop checks running_ on its
    // next natural wakeup. Best-effort doorbell.
    ssize_t r = ::write(wake_fd_, &one, sizeof(one));
    (void)r;
}

void EventLoop::run() {
    constexpr int kMaxEvents = 256;
    std::array<epoll_event, kMaxEvents> events;

    while (running_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(epoll_fd_, events.data(), kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            epoll_event& ev = events[i];

            if (ev.data.ptr == nullptr) {
                // The doorbell: stop() and/or queued cross-thread tasks. Drain
                // the eventfd, then run whatever was posted. (running_ is
                // re-checked at the top of the while loop.)
                drain_wakeup();
                run_pending_tasks();
                continue;
            }

            auto* handler = static_cast<EventHandler*>(ev.data.ptr);

            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                handler->on_error();
                continue;
            }
            if (ev.events & EPOLLIN) {
                handler->on_readable();
            }
            // Claude — Date 06/19/2026
            // Skip the write callback if on_readable() already scheduled this
            // fd for close — its handler may be half torn down.
            if ((ev.events & EPOLLOUT) && !pending_close_.count(handler->fd())) {
                handler->on_writable();
            }
        }

        process_pending_close();
    }
}

}  // namespace castle
