// Claude — Date 07/05/2026
// Token-bucket implementation. Refill is lazy (computed from elapsed time on
// access), so there's no background timer.
#include "gateway/rate_limiter.h"

#include <algorithm>

namespace castle {

bool RateLimiter::allow(uint32_t ipv4) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mu_);

    auto it = buckets_.find(ipv4);
    if (it == buckets_.end()) {
        if (buckets_.size() >= kMaxBuckets) prune_locked();
        // New client starts with a full bucket, then spends one token.
        buckets_.emplace(ipv4, Bucket{burst_ - 1.0, now});
        return true;
    }

    Bucket& b = it->second;
    const double elapsed =
        std::chrono::duration<double>(now - b.last).count();
    b.last = now;
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    return false;  // empty bucket -> drop
}

void RateLimiter::prune_locked() {
    // Remove buckets that have refilled to full — a full bucket carries no
    // state (identical to a fresh one), so dropping it is free.
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (it->second.tokens >= burst_)
            it = buckets_.erase(it);
        else
            ++it;
    }
}

}  // namespace castle
