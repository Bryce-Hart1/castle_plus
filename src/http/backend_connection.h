// Claude — Date 06/19/2026
// The upstream half of a proxied request: the connection castle++ opens to the
// backend. Lives on the SAME EventLoop as its client HttpConnection (no locks
// between them), holds a raw pointer back to the client, and streams the
// backend's response into the client's output buffer. On backend EOF the
// response is complete (we force Connection: close upstream, so EOF frames it).
#pragma once

#include <chrono>
#include <string>

#include "net/event_handler.h"
#include "net/socket.h"

namespace castle {

class EventLoop;
class HttpConnection;

class BackendConnection : public EventHandler {
public:
    BackendConnection(EventLoop& loop, Socket sock, HttpConnection* client,
                      std::chrono::seconds timeout);
    ~BackendConnection() override;

    int fd() const override { return sock_.get(); }
    void on_readable() override;
    void on_writable() override;
    void on_error() override;
    void check_timeout(std::chrono::steady_clock::time_point now) override;

    // Queue request bytes for the backend; sent once the connect completes.
    void send(const std::string& data);
    // Called by the client when it's going away, so we don't call back into it.
    void detach_client() { client_ = nullptr; }

private:
    void flush();
    void fail(const char* why);
    void touch() { last_activity_ = std::chrono::steady_clock::now(); }

    EventLoop& loop_;
    Socket sock_;
    HttpConnection* client_;
    std::string out_;  // the upstream request bytes awaiting write
    std::chrono::seconds timeout_;
    std::chrono::steady_clock::time_point last_activity_;
    bool connected_ = false;
    bool want_write_ = false;
};

}  // namespace castle
