// Upstream connection implementation. Connect is non-blocking: we register for
// EPOLLOUT, and the first writable event means the connect finished (or failed,
// via SO_ERROR). After that it's EPOLLIN to pump the response back to the client.
#include "http/backend_connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "http/http_connection.h"
#include "net/event_loop.h"
#include "util/log.h"

namespace castle {

//backend connection class
BackendConnection::BackendConnection(EventLoop& loop, Socket sock, HttpConnection* client, std::chrono::seconds timeout)
    : loop_(loop),
      sock_(std::move(sock)),
      client_(client),
      timeout_(timeout),
      last_activity_(std::chrono::steady_clock::now()) {}

BackendConnection::~BackendConnection() {
    // Sever the back-pointer so a peer torn down in the same batch never
    // dereferences us.
    if (client_) client_->detach_backend();
}

void BackendConnection::send(const std::string& data) {
    out_.append(data);
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
        // Now interested in the response; flush() re-adds EPOLLOUT if the
        // request didn't drain in one go.
        loop_.update(fd(), EPOLLIN | EPOLLET);
        want_write_ = false;
    }
    flush();
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

void BackendConnection::on_readable() {
    char buf[16384];
    for (;;) {
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

    if (out_.empty()) {
        if (want_write_) {
            loop_.update(fd(), EPOLLIN | EPOLLET);
            want_write_ = false;
        }
    } else if (!want_write_) {
        loop_.update(fd(), EPOLLIN | EPOLLOUT | EPOLLET);
        want_write_ = true;
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
