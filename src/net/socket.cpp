// Claude — Date 06/19/2026
// Socket helper implementations. Linux-specific (SO_REUSEPORT, accept4 etc. are
// used elsewhere); this whole project targets Linux (x86-64).
#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace castle {

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Socket::set_nonblocking() {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
}

Socket make_reuseport_listener(const std::string& bind_addr, uint16_t port, int backlog, std::string& err){
    // SOCK_CLOEXEC matters here: the supervisor fork/execs backends, and a
    // listening fd inherited by a backend keeps the shared SO_REUSEPORT queue
    // alive after castle drops it — the kernel would keep hashing connections
    // onto a queue nobody accepts.
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return Socket{};
    }
    Socket sock(fd);  // takes ownership; auto-closes on any early return below

    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        err = std::string("SO_REUSEADDR: ") + std::strerror(errno);
        return Socket{};
    }
    // The key option: lets every worker thread bind the same port so the kernel
    // hashes new connections across the listeners.
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        err = std::string("SO_REUSEPORT: ") + std::strerror(errno);
        return Socket{};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind_addr.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
            err = "bind address not a valid IPv4 address: " + bind_addr;
            return Socket{};
        }
        // IP_FREEBIND lets us bind an address that doesn't exist yet — e.g. the
        // WireGuard IP before wg0 comes up at boot — so systemd ordering against
        // wg-quick is advisory, not load-bearing.
        if (::setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(one)) < 0) {
            err = std::string("IP_FREEBIND: ") + std::strerror(errno);
            return Socket{};
        }
    }
    if(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0){
        err = std::string("bind: ") + std::strerror(errno);
        return Socket{};
    }

    if (::listen(fd, backlog) < 0) {
        err = std::string("listen: ") + std::strerror(errno);
        return Socket{};
    }

    return sock;
}

namespace {

// Non-blocking connect so the wait is bounded by timeout_ms rather than the
// kernel's SYN retry schedule (which can run past a minute). Returns the
// connected (still non-blocking) socket, or an invalid one with `why` set to
// "refused", "timeout" or a short reason.
Socket connect_with_timeout(const std::string& host, uint16_t port,
                            int timeout_ms, std::string& why) {
    Socket sock(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!sock.valid()) {
        why = "socket failed";
        return Socket{};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        why = "not an IPv4 address";
        return Socket{};
    }

    int r = ::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) return sock;
    if (errno != EINPROGRESS) {
        why = errno == ECONNREFUSED ? "refused" : std::strerror(errno);
        return Socket{};
    }

    pollfd p{sock.get(), POLLOUT, 0};
    const int pr = ::poll(&p, 1, timeout_ms);
    if (pr == 0) {
        why = "timeout";
        return Socket{};
    }
    if (pr < 0 || !(p.revents & (POLLOUT | POLLERR | POLLHUP))) {
        why = "poll failed";
        return Socket{};
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        why = "getsockopt failed";
        return Socket{};
    }
    if (err != 0) {
        why = err == ECONNREFUSED ? "refused" : std::strerror(err);
        return Socket{};
    }
    return sock;
}

}  // namespace

bool tcp_probe(const std::string& host, uint16_t port, int timeout_ms){
    std::string why;
    return connect_with_timeout(host, port, timeout_ms, why).valid();
}

std::optional<int> http_get_status(const std::string& host, uint16_t port,
                                   const std::string& path, int timeout_ms,
                                   std::string& err) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto remaining_ms = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - clock::now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    };

    Socket sock = connect_with_timeout(host, port, timeout_ms, err);
    if (!sock.valid()) return std::nullopt;

    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                            std::to_string(port) +
                            "\r\nConnection: close\r\n\r\n";
    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = ::send(sock.get(), req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{sock.get(), POLLOUT, 0};
            if (::poll(&p, 1, remaining_ms()) <= 0) {
                err = "timeout";
                return std::nullopt;
            }
            continue;
        }
        err = "send failed";
        return std::nullopt;
    }

    // Only the status line matters: "HTTP/1.x NNN ...". Read until it's whole.
    std::string got;
    char buf[256];
    while (got.find("\r\n") == std::string::npos && got.size() < 1024) {
        pollfd p{sock.get(), POLLIN, 0};
        const int pr = ::poll(&p, 1, remaining_ms());
        if (pr == 0) {
            err = "timeout";
            return std::nullopt;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            err = "poll failed";
            return std::nullopt;
        }
        ssize_t n = ::recv(sock.get(), buf, sizeof(buf), 0);
        if (n > 0) {
            got.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;  // closed; parse whatever arrived
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            err = "recv failed";
            return std::nullopt;
        }
    }

    if (got.size() < 12 || got.compare(0, 7, "HTTP/1.") != 0 || got[8] != ' ' ||
        !std::isdigit(static_cast<unsigned char>(got[9])) ||
        !std::isdigit(static_cast<unsigned char>(got[10])) ||
        !std::isdigit(static_cast<unsigned char>(got[11]))) {
        err = "bad response";
        return std::nullopt;
    }
    return (got[9] - '0') * 100 + (got[10] - '0') * 10 + (got[11] - '0');
}

}  // namespace castle
