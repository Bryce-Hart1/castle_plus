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
    RateLimiter(double rate, double burst) : rate_(rate), burst_(burst) {}

    // Consume one token for `ipv4` (network-byte-order, as from sockaddr_in).
    // Returns false if the bucket is empty (caller should drop the connection).
    bool allow(uint32_t ipv4);

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last;
    };

    // Drop idle (full) buckets if the table grows too large. Caller holds mu_.
    void prune_locked();

    double rate_;
    double burst_;
    std::mutex mu_;
    std::unordered_map<uint32_t, Bucket> buckets_;

    static constexpr std::size_t kMaxBuckets = 100000;  // safety bound
};

}  // namespace castle
