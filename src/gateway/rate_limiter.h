// Claude — Date 07/05/2026
// Per-client-IP token bucket, checked at accept time (before any TLS/HTTP work)
// so floods are dropped cheaply. Each IPv4 gets a bucket that refills at `rate`
// tokens/sec up to `burst`; a connection costs one token, and if the bucket is
// empty the connection is dropped.
//
// Shared across the per-core listener threads, so it's mutex-guarded (accept is
// not the hot path). Behind the VPS/WireGuard ingress every request appears to
// come from the tunnel's single IP, so this currently acts as an aggregate cap
// there; it becomes true per-client once PROXY-protocol client-IP preservation
// lands.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace castle {

class RateLimiter {
public:
    // rate: tokens (connections) per second; burst: bucket capacity.
    RateLimiter(double rate, double burst)
        : rate_(rate),
          burst_(burst),
          overflow_{burst, std::chrono::steady_clock::now()} {}

    // Consume one token for `ipv4` (network-byte-order, as from sockaddr_in).
    // Returns false if the bucket is empty (caller should drop the connection).
    bool allow(uint32_t ipv4);

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last;
    };

    // Lazy-refill `b` from elapsed time, then try to spend one token.
    // Caller holds mu_.
    bool spend(Bucket& b, std::chrono::steady_clock::time_point now);

    // Drop buckets whose *implied* refill (stored tokens + elapsed time) is
    // full — refill is lazy, so the stored token count alone understates idle
    // buckets and would never reclaim them. Caller holds mu_.
    void prune_locked(std::chrono::steady_clock::time_point now);

    double rate_;
    double burst_;
    std::mutex mu_;
    std::unordered_map<uint32_t, Bucket> buckets_;

    // Hard bound on tracked IPs (~7 MB worst case). When the table is full even
    // after pruning, new IPs share `overflow_` instead of getting their own
    // bucket. Fail-closed by design: LRU eviction would hand an attacker cycling
    // >kMaxBuckets IPs a fresh full burst on every return, and refusing all new
    // IPs would lock out every legitimate new client during a flood. The shared
    // bucket bounds both memory and aggregate new-IP throughput while tracked
    // (pre-flood) IPs keep their own buckets. Behind the WG tunnel today all
    // traffic is one IP so this path is cold; it matters for direct-LAN exposure
    // now and PROXY-protocol client IPs later.
    static constexpr std::size_t kMaxBuckets = 100000;
    Bucket overflow_;
};

}  // namespace castle
