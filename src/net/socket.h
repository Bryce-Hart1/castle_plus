/** Bryce Hart Jul 16
RAII wrapper around a file descriptor plus the small set of socket helpers
the reactor needs. Owning the fd here means connections/listeners close
cleanly when their handler is destroyed — no manual close() bookkeeping.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace castle {
class Socket {
public:
    Socket() = default;

    explicit Socket(int fd) : fd_(fd) {}

    ~Socket(){ 
        close(); 
    }

    // Move-only: an fd has exactly one owner.
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
         other.fd_ = -1; 
        }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    Socket(const Socket&) = delete;

    Socket& operator=(const Socket&) = delete;

    int get() const{ 
        return fd_; 
    }

    bool valid() const{ 
        return fd_ >= 0; 
    }

    // Relinquish ownership without closing (caller takes responsibility).
    int release(){
        int f = fd_;
        fd_ = -1;
        return f;
    }

    void close();
    bool set_nonblocking();

private:
    int fd_ = -1;
};

// Build a non-blocking listening socket bound to <bind_addr>:<port> with
// SO_REUSEADDR + SO_REUSEPORT. SO_REUSEPORT is the load-balancing trick: every
// per-core worker creates its own listener on the same port and the kernel
// spreads incoming connections across them, with no shared accept lock.
// An empty bind_addr means 0.0.0.0 (all interfaces). A specific address is
// bound with IP_FREEBIND so it may not exist yet (e.g. the WireGuard IP before
// wg0 is up at boot). Returns an invalid Socket and fills `err` on failure.
Socket make_reuseport_listener(const std::string& bind_addr, uint16_t port, int backlog, std::string& err);

// Blocking TCP reachability check: true if <host>:<port> (an IPv4 literal, no
// DNS) accepts a connection within timeout_ms. The connection is closed straight
// away. Used by the supervisor's backend health checks and the internet watch.
bool tcp_probe(const std::string& host, uint16_t port, int timeout_ms);

// Blocking `GET <path>` to <host>:<port> (IPv4 literal) with Connection: close.
// Reads only the status line and returns its code. The whole exchange is
// bounded by timeout_ms. On failure returns nullopt and sets `err` to
// "refused", "timeout", "bad response" or a short reason. Used by the
// `swiftsearch` control command to hit /healthz.
std::optional<int> http_get_status(const std::string& host, uint16_t port,
                                   const std::string& path, int timeout_ms,
                                   std::string& err);

}  // namespace castle
