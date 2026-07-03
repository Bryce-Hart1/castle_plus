// Claude — Date 06/19/2026
// The client half of a proxied request. Flow: (TLS handshake if any) -> read +
// parse the request head (picohttpparser), route by Host/path, buffer the body,
// open a backend connection, forward a rewritten request (hop-by-hop headers
// stripped, Connection: close added), then stream the response back.
//
// I/O goes through a Transport (plaintext or TLS), so a single drive() loop
// handles both: advance handshake, pump reads, pump writes, then recompute epoll
// interest from whichever direction each side is blocked on (TLS reads can want
// to write, and vice versa).
//
// Scope: one request per client connection (no keep-alive / pooling yet), with
// request-size limits + idle timeouts.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "http/router.h"
#include "net/event_handler.h"
#include "net/transport.h"

namespace castle {

class EventLoop;
class BackendConnection;

class HttpConnection : public EventHandler {
public:
    HttpConnection(EventLoop& loop, std::unique_ptr<Transport> transport,
                   const Router& router, std::chrono::seconds timeout);
    ~HttpConnection() override;

    int fd() const override { return transport_->fd(); }
    void on_readable() override { drive(); }
    void on_writable() override { drive(); }
    void on_error() override { close_both(); }
    void check_timeout(std::chrono::steady_clock::time_point now) override;

    // ---- called by the paired BackendConnection (same loop/thread) ----
    void deliver_to_client(const char* data, size_t n);  // response bytes
    void on_backend_closed();          // backend EOF => response complete
    void on_backend_error(const char* why);  // pre-response failure => 5xx
    void detach_backend() { backend_ = nullptr; }

private:
    enum class State { ReadingHead, ReadingBody, Streaming, Closing };

    void drive();              // handshake -> read pump -> write pump -> interest
    IoStatus pump_read();      // returns the direction the read side is blocked on
    IoStatus pump_write();     // returns the direction the write side is blocked on
    void recompute_interest();
    void set_interest(uint32_t events);

    void process_head();       // parse + route; sets up upstream head + body len
    void maybe_start_backend();  // once the full body is buffered
    void start_backend();      // non-blocking connect + BackendConnection
    void send_error(int code, const char* status, const char* body);
    void close_both();
    void touch() { last_activity_ = std::chrono::steady_clock::now(); }

    EventLoop& loop_;
    std::unique_ptr<Transport> transport_;
    const Router& router_;
    BackendConnection* backend_ = nullptr;
    std::chrono::seconds timeout_;
    std::chrono::steady_clock::time_point last_activity_;

    State state_ = State::ReadingHead;
    std::string in_;         // bytes read from the client (head + body)
    std::string out_;        // bytes to write back to the client
    std::string upstream_;   // rewritten request to hand the backend
    Backend backend_target_;
    size_t head_len_ = 0;    // length of the parsed request head in in_
    size_t body_needed_ = 0;  // Content-Length
    bool responded_ = false;  // have we sent any response bytes yet?
    bool handshake_done_ = false;
    bool closed_ = false;     // guards against re-entrant teardown
    uint32_t interest_ = 0;   // current epoll interest (avoid redundant updates)
    IoStatus read_block_ = IoStatus::WouldBlockRead;   // last read-side block dir
    IoStatus write_block_ = IoStatus::Ok;              // last write-side block dir
};

}  // namespace castle
