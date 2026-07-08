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

Socket make_reuseport_listener(uint16_t port, int backlog, std::string& err) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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
