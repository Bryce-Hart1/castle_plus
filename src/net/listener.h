// Claude — Date 06/19/2026
// An EventHandler that owns a listening socket and, on readability, accepts new
// connections and registers them on the same loop. What KIND of connection it
// builds is injected as a factory, so the same listener serves echo today and
// HTTP/TLS connections in later milestones without changes here.
#pragma once

#include <functional>
#include <memory>

#include "net/event_handler.h"
#include "net/socket.h"

namespace castle {

class EventLoop;
class RateLimiter;

class Listener : public EventHandler {
public:
    // Given the accepted client socket, produce the handler that will service
    // it. The factory does not register the handler — the listener does.
    using ConnFactory =
        std::function<std::unique_ptr<EventHandler>(EventLoop&, Socket)>;

    // `limiter` is optional (may be null); when present, connections over the
    // per-IP token-bucket rate are dropped at accept.
    Listener(EventLoop& loop, Socket sock, ConnFactory factory,
             RateLimiter* limiter = nullptr);

    int fd() const override { return sock_.get(); }
    void on_readable() override;
    void on_writable() override {}

private:
    EventLoop& loop_;
    Socket sock_;
    ConnFactory factory_;
    RateLimiter* limiter_;
};

}  // namespace castle
