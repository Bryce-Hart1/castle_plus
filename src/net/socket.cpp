// Claude — Date 06/19/2026
// Socket helper implementations. Linux-specific (SO_REUSEPORT, accept4 etc. are
// used elsewhere); this whole project targets Linux (x86-64).
#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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

}  // namespace castle
