// One epoll instance + the fds registered on it. Exactly one EventLoop runs per
// worker thread; a handler and everything it spawns (e.g. a client conn and its
// backend conn, later) live on the same loop, so no locking is needed between
// them. Cross-thread control (stop, and later: hand-off tasks) goes through the
// eventfd wakeup.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/event_handler.h"
#include "net/loop_stats.h"

namespace castle {

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Take ownership of a handler and register its fd with `events`
    // (e.g. EPOLLIN | EPOLLET). Safe to call from within a callback on this
    // same loop (that's how the listener adds new connections).
    void add(std::unique_ptr<EventHandler> handler, uint32_t events);

    // Change the epoll interest mask for an already-registered fd.
    void update(int fd, uint32_t events);

    // Schedule a handler for teardown. Deferred to the end of the current
    // epoll batch so we never destroy a handler while dispatching into it.
    void request_close(int fd);

    // Run `task` on THIS loop's own thread, ASAP. Thread-safe — this is the
    // sanctioned way for another thread (e.g. the control plane) to touch
    // loop-local state without locks or races. The task is queued and the loop
    // is woken via its eventfd; it runs between connection events. Fire-and-
    // forget — capture a promise/future in the task if you need a reply back.
    void post(std::function<void()> task);

    // This loop's live counters (lock-free). The loop's own thread writes them;
    // other threads read with relaxed ordering. Aggregated for `status`.
    LoopStats& stats() { return stats_; }

    // Turn on a periodic sweep (its own timerfd) that calls check_timeout() on
    // every registered handler, so idle connections get closed. Call once,
    // before run() — not thread-safe against a running loop.
    void enable_idle_timeouts(int sweep_interval_ms);

    // Run until stop() is called. Blocks the calling thread.
    void run();

    // Ask the loop to exit. Thread-safe; wakes the loop via its eventfd.
    void stop();

private:
    void drain_wakeup();
    void run_pending_tasks();
    void sweep_timeouts();
    void process_pending_close();

    int epoll_fd_ = -1;
    int wake_fd_ = -1;     // eventfd: the only thing data.ptr == nullptr means
    int timeout_fd_ = -1;  // timerfd for the idle sweep (owned by an FdWatcher)
    std::atomic<bool> running_{true};

    // Declared before handlers_ so it outlives them: connection destructors
    // decrement these counters as the map is torn down.
    LoopStats stats_;

    std::mutex task_mutex_;
    std::vector<std::function<void()>> tasks_;

    std::unordered_map<int, std::unique_ptr<EventHandler>> handlers_;
    std::unordered_set<int> pending_close_;
};

}  // namespace castle
