// Claude — Date 06/19/2026
// Non-blocking echo with proper backpressure. The shape here is the contract
// every future connection type follows.
#include "net/echo_connection.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "net/event_loop.h"

namespace castle {

EchoConnection::EchoConnection(EventLoop& loop, Socket sock)
    : loop_(loop), sock_(std::move(sock)) {
    loop_.stats().active_connections.fetch_add(1, std::memory_order_relaxed);
    loop_.stats().total_connections.fetch_add(1, std::memory_order_relaxed);
}

EchoConnection::~EchoConnection() {
    loop_.stats().active_connections.fetch_sub(1, std::memory_order_relaxed);
}

void EchoConnection::on_readable() {
    char buf[16384];
    // Edge-triggered: read until EAGAIN or we'd block forever on leftover data.
    for (;;) {
        ssize_t n = ::read(sock_.get(), buf, sizeof(buf));
        if (n > 0) {
            out_.append(buf, static_cast<size_t>(n));
            loop_.stats().bytes_in.fetch_add(static_cast<uint64_t>(n),
                                             std::memory_order_relaxed);
        } else if (n == 0) {
            // Peer half-closed. Flush whatever's buffered, then tear down.
            closing_ = true;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
            if (errno == EINTR) continue;
            loop_.request_close(fd());
            return;
        }
    }
    flush();
}

void EchoConnection::on_writable() { flush(); }

void EchoConnection::on_error() { loop_.request_close(fd()); }

void EchoConnection::flush() {
    size_t off = 0;
    while (off < out_.size()) {
        ssize_t n = ::write(sock_.get(), out_.data() + off, out_.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            loop_.stats().bytes_out.fetch_add(static_cast<uint64_t>(n),
                                              std::memory_order_relaxed);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // socket full
            if (errno == EINTR) continue;
            loop_.request_close(fd());
            return;
        }
    }
    out_.erase(0, off);

    if (out_.empty()) {
        // Everything flushed. Close if the peer is gone; otherwise drop our
        // EPOLLOUT subscription so we don't spin on a writable-but-idle socket.
        if (closing_) {
            loop_.request_close(fd());
            return;
        }
        if (want_write_) {
            loop_.update(fd(), EPOLLIN | EPOLLET);
            want_write_ = false;
        }
    } else if (!want_write_) {
        // Couldn't drain — the kernel send buffer is full. Wait for EPOLLOUT.
        loop_.update(fd(), EPOLLIN | EPOLLOUT | EPOLLET);
        want_write_ = true;
    }
}

}  // namespace castle
