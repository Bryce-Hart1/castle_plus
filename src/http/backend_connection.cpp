// Claude — Date 06/19/2026 last changed: 07/04/2026 by: Claude
// Upstream connection implementation. Connect is non-blocking: we register for
// EPOLLOUT, and the first writable event means the connect finished (or failed,
// via SO_ERROR). After that it's EPOLLIN to pump the response back to the client.
// (Added: bidirectional backpressure — apply_interest() gates EPOLLIN on
// read_paused_, and flush() notifies the client once the upload buffer drains.)
#include "http/backend_connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "http/http_connection.h"
#include "net/event_loop.h"
#include "util/log.h"

namespace castle {

BackendConnection::BackendConnection(EventLoop& loop, Socket sock,
                                     HttpConnection* client,
                                     std::chrono::seconds timeout)
    : loop_(loop),
      sock_(std::move(sock)),
      client_(client),
      timeout_(timeout),
      last_activity_(std::chrono::steady_clock::now()),
      interest_(EPOLLOUT | EPOLLET) {}  // matches how start_backend registered us

BackendConnection::~BackendConnection() {
    // Sever the back-pointer so a peer torn down in the same batch never
    // dereferences us.
    if (client_) client_->detach_backend();
}

void BackendConnection::send(const char* data, size_t n) {
    out_.append(data, n);
    if (out_.size() > kBackpressureHigh) high_water_hit_ = true;
    if (connected_) flush();  // otherwise sent when the connect completes
}

void BackendConnection::on_writable() {
    if (!connected_) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(sock_.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
            err != 0) {
            fail("connect failed");
            return;
        }
        connected_ = true;
        touch();
    }
    flush();  // sends queued request/body; apply_interest() switches to EPOLLIN
}

void BackendConnection::on_readable() {
    char buf[16384];
    // deliver_to_client() may pause us (download backpressure) mid-loop.
    while (!read_paused_) {
        ssize_t n = ::read(sock_.get(), buf, sizeof(buf));
        if (n > 0) {
            touch();
            if (client_) client_->deliver_to_client(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            // Backend EOF => the response is complete (Connection: close).
            if (client_) {
                HttpConnection* c = client_;
                client_ = nullptr;
                c->on_backend_closed();
            }
            loop_.request_close(fd());
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail("backend read failed");
            return;
        }
    }
}

void BackendConnection::pause_reads() {
    if (read_paused_) return;
    read_paused_ = true;
    apply_interest();
}

void BackendConnection::resume_reads() {
    if (!read_paused_) return;
    read_paused_ = false;
    apply_interest();
    // Edge-triggered epoll won't re-notify for data that arrived while paused,
    // so pump the socket now to catch up.
    on_readable();
}

void BackendConnection::check_timeout(
    std::chrono::steady_clock::time_point now) {
    if (timeout_.count() <= 0) return;
    if (now - last_activity_ > timeout_) {
        LOG_WARN("http: upstream idle/connect timeout (fd %d, > %llds)", fd(),
                 static_cast<long long>(timeout_.count()));
        fail("upstream timeout");
    }
}

void BackendConnection::on_error() { fail("backend socket error"); }

void BackendConnection::flush() {
    size_t off = 0;
    while (off < out_.size()) {
        ssize_t n = ::write(sock_.get(), out_.data() + off, out_.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            touch();
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail("backend write failed");
            return;
        }
    }
    out_.erase(0, off);

    // Upload backpressure: once we've drained below the low-water mark, let the
    // client resume reading the request body. Fires once per episode.
    if (high_water_hit_ && out_.size() < kBackpressureLow) {
        high_water_hit_ = false;
        if (client_) client_->on_backend_drained();
    }

    apply_interest();
}

void BackendConnection::apply_interest() {
    uint32_t ev = EPOLLET;
    if (!connected_) {
        ev |= EPOLLOUT;  // still waiting for the connect to complete
    } else {
        if (!read_paused_) ev |= EPOLLIN;  // read the response unless paused
        if (!out_.empty()) ev |= EPOLLOUT;  // more request/body to write
    }
    if (ev != interest_) {
        interest_ = ev;
        loop_.update(fd(), ev);
    }
}

void BackendConnection::fail(const char* why) {
    if (client_) {
        HttpConnection* c = client_;
        client_ = nullptr;
        c->on_backend_error(why);
    }
    loop_.request_close(fd());
}

}  // namespace castle
