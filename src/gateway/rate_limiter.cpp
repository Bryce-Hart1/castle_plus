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
        if (buckets_.size() >= kMaxBuckets) prune_locked(now);
        if (buckets_.size() >= kMaxBuckets) {
            // Table is full even after pruning (a diverse-IP flood). Don't
            // insert — charge the shared overflow bucket instead, so memory
            // stays bounded and new-IP traffic is collectively rate-limited.
            // See the header for why this beats LRU eviction here.
            return spend(overflow_, now);
        }
        // New client starts with a full bucket, then spends one token.
        buckets_.emplace(ipv4, Bucket{burst_ - 1.0, now});
        return true;
    }
    return spend(it->second, now);
}

bool RateLimiter::spend(Bucket& b,
                        std::chrono::steady_clock::time_point now) {
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

void RateLimiter::prune_locked(std::chrono::steady_clock::time_point now) {
    // Remove buckets whose refill *would be* full — a full bucket carries no
    // state (identical to a fresh one), so dropping it is free. Refill is lazy
    // (only updated when that IP connects again), so the check must add the
    // elapsed time; testing the stored token count alone would never reclaim
    // an abandoned bucket last seen below full.
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        const double elapsed =
            std::chrono::duration<double>(now - it->second.last).count();
        if (it->second.tokens + elapsed * rate_ >= burst_)
            it = buckets_.erase(it);
        else
            ++it;
    }
}

}  // namespace castle
