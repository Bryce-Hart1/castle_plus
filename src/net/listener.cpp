// Bryce Hart — Date 06/19/2026 last changed: 07/05/2026 by: Bryce
// Edge-triggered accept loop: drain the accept queue to EAGAIN every wakeup,
// because epoll won't re-notify until a new connection arrives after we stop.
// Excess connections (over the per-IP rate or the global cap) are dropped here,
// at accept, before any TLS/HTTP work is spent on them.
#include "net/listener.h"

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "gateway/rate_limiter.h"
#include "net/conn_limit.h"
#include "net/event_loop.h"
#include "util/log.h"

namespace castle {

Listener::Listener(EventLoop& loop, Socket sock, ConnFactory factory, RateLimiter* limiter) : loop_(loop),
      sock_(std::move(sock)),
      factory_(std::move(factory)),
      limiter_(limiter) {}

void Listener::on_readable() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        // accept4 with SOCK_NONBLOCK: one syscall, no separate fcntl, and the
        // client fd is non-blocking from birth.
        int cfd = ::accept4(sock_.get(), reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;  // client bailed mid-handshake
            // EMFILE/ENFILE means we're out of fds. Don't spin: stop draining
            // this round and let the loop come back to us. (A production guard
            // would reserve a spare fd to accept-and-close; noted for later.
            // In the shipped config this shouldn't fire: --max-conn 2000 plus
            // LimitNOFILE=65536 in the systemd unit keeps fd usage far below
            // the limit — hitting this path means the config is wrong.)
            LOG_ERROR("accept4 failed: %s", std::strerror(errno));
            break;
        }

        // Drop early (before any TLS/HTTP work) if this client is over its
        // rate, or if we're at the global connection cap.
        if(limiter_ && !limiter_->allow(addr.sin_addr.s_addr)){
            ::close(cfd);
            continue;
        }
        if(!conn_slot_available()){
            ::close(cfd);
            continue;
        }

        // factory may return null (e.g. a TLS wrap failure); it already closed
        // the socket in that case, so just skip.
        auto conn = factory_(loop_, Socket(cfd));
        if (conn){
            loop_.add(std::move(conn), EPOLLIN | EPOLLET);
        }
    }
}

}  // namespace castle
