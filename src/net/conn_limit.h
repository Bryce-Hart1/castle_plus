// Claude — Date 07/05/2026
// Process-wide cap on concurrent client connections — a cheap DoS guard so a
// flood can't exhaust file descriptors/memory. Each client connection bumps the
// counter in its ctor and drops it in its dtor; the listener reads it before
// accepting and drops new connections once the cap is hit. The check-then-create
// is a soft cap (a few over the limit under concurrency is fine).
#pragma once

#include <atomic>
#include <cstddef>

namespace castle {

inline std::atomic<std::size_t>& active_conn_count() {
    static std::atomic<std::size_t> count{0};
    return count;
}

// 0 = unlimited. Set once from main before threads start.
inline std::size_t& max_conn_limit() {
    static std::size_t limit = 0;
    return limit;
}

// True if a new client connection is allowed right now.
inline bool conn_slot_available() {
    std::size_t m = max_conn_limit();
    return m == 0 || active_conn_count().load(std::memory_order_relaxed) < m;
}

}  // namespace castle
