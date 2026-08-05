//Peer reviewed Bryce Hart 7-5-26
// Per-loop live counters. The owning worker thread is the only writer; the
// control plane reads them (relaxed) and sums across loops for `status`. Because
// each loop owns its own instance and only it writes, there's no write
// contention — the lock-free read path the control thread uses. alignas(64)
// keeps two loops' counters off the same cache line (no false sharing).
#pragma once

#include <atomic>
#include <cstdint>

namespace castle {

struct alignas(64) LoopStats {
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> bytes_out{0};
};

}  // namespace castle
