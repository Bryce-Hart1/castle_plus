// Claude — Date 06/19/2026
// PlainTransport: the plaintext path. EINTR is retried in-place so it never
// escapes as a spurious would-block (which, under edge-triggered epoll, could
// stall until the next edge).
#include "net/transport.h"

#include <unistd.h>

#include <cerrno>

namespace castle {

IoStatus PlainTransport::recv(char* buf, size_t n, size_t& out_n) {
    out_n = 0;
    ssize_t r;
    do {
        r = ::read(sock_.get(), buf, n);
    } while (r < 0 && errno == EINTR);

    if (r > 0) {
        out_n = static_cast<size_t>(r);
        return IoStatus::Ok;
    }
    if (r == 0) return IoStatus::Closed;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlockRead;
    return IoStatus::Error;
}

IoStatus PlainTransport::send(const char* buf, size_t n, size_t& out_n) {
    out_n = 0;
    ssize_t r;
    do {
        r = ::write(sock_.get(), buf, n);
    } while (r < 0 && errno == EINTR);

    if (r > 0) {
        out_n = static_cast<size_t>(r);
        return IoStatus::Ok;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlockWrite;
    return IoStatus::Error;
}

}  // namespace castle
