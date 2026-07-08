// Claude — Date 06/19/2026 last changed: 07/04/2026 by: Claude
// The upstream half of a proxied request: the connection castle++ opens to the
// backend. Lives on the SAME EventLoop as its client HttpConnection (no locks
// between them), holds a raw pointer back to the client, and streams the
// backend's response into the client's output buffer. On backend EOF the
// response is complete (we force Connection: close upstream, so EOF frames it).
// (Added streaming-upload support: pending_bytes()/drain-notify for upload
// backpressure, and pause/resume_reads() for download backpressure.)
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "net/event_handler.h"
#include "net/socket.h"

namespace castle {

class EventLoop;
class HttpConnection;

// Flow-control thresholds shared with HttpConnection (which includes this
// header). Pause a direction once a peer's buffer exceeds HIGH; resume once it
// drains below LOW.
constexpr size_t kBackpressureHigh = 256 * 1024;
constexpr size_t kBackpressureLow = 64 * 1024;

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
    void send(const std::string& data) { send(data.data(), data.size()); }
    void send(const char* data, size_t n);

    // Bytes queued toward the backend but not yet written (upload backpressure).
    size_t pending_bytes() const { return out_.size(); }

    // Download backpressure: stop / resume reading the response when the client
    // is slow to drain it.
    void pause_reads();
    void resume_reads();

    // Called by the client when it's going away, so we don't call back into it.
    void detach_client() { client_ = nullptr; }

private:
    void flush();
    void fail(const char* why);
    void apply_interest();  // recompute epoll interest from connected/paused/out_
    void touch() { last_activity_ = std::chrono::steady_clock::now(); }

    EventLoop& loop_;
    Socket sock_;
    HttpConnection* client_;
    std::string out_;  // the upstream request/body bytes awaiting write
    std::chrono::seconds timeout_;
    std::chrono::steady_clock::time_point last_activity_;
    uint32_t interest_ = 0;      // current epoll interest (avoid redundant MODs)
    bool connected_ = false;
    bool read_paused_ = false;   // response reads paused (download backpressure)
    bool high_water_hit_ = false;  // out_ crossed HIGH; notify client on drain
};

}  // namespace castle
