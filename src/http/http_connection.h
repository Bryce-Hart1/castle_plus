// Claude — Date 06/19/2026 last changed: 07/04/2026 by: Claude
// The client half of a proxied request. Flow: (TLS handshake if any) -> read +
// parse the request head (picohttpparser), route by Host/path, open the backend
// and forward a rewritten head, then STREAM the request body through as it
// arrives (no whole-body buffering) and stream the response back.
//
// I/O goes through a Transport (plaintext or TLS), so a single drive() loop
// handles both: advance handshake, pump reads, pump writes, then recompute epoll
// interest from whichever direction each side is blocked on (TLS reads can want
// to write, and vice versa).
//
// Bounded memory in both directions via backpressure: pause our client reads
// while the backend's send buffer is full (uploads), and pause the backend's
// reads while our client send buffer is full (downloads).
//
// Scope: one request per client connection (no keep-alive / pooling yet), with a
// configurable max body + idle timeouts. Chunked request bodies are still 501.
#pragma once

#include <chrono>
#include <cstddef>
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
                   const Router& router, std::chrono::seconds timeout,
                   size_t max_body);
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
    void on_backend_drained();         // backend send buffer drained => resume up
    void detach_backend() { backend_ = nullptr; }

private:
    enum class State {
        ReadingHead,  // accumulating + parsing the request head
        StreamingUp,  // head sent; forwarding the request body to the backend
        Streaming,    // request done; relaying the response back
        Closing,      // draining the last bytes, then close
    };

    void drive();              // handshake -> read pump -> write pump -> interest
    IoStatus pump_read();      // returns the direction the read side is blocked on
    IoStatus pump_write();     // returns the direction the write side is blocked on
    void handle_client_bytes(const char* data, size_t n);  // state dispatch
    void recompute_interest();
    void set_interest(uint32_t events);

    void process_head();       // parse + route; start backend; send head
    void forward_body(const char* data, size_t n);  // stream body -> backend
    void start_backend();      // non-blocking connect + BackendConnection
    void send_error(int code, const char* status, const char* body);
    void close_both();
    void touch() { last_activity_ = std::chrono::steady_clock::now(); }

    EventLoop& loop_;
    std::unique_ptr<Transport> transport_;
    const Router& router_;
    BackendConnection* backend_ = nullptr;
    std::chrono::seconds timeout_;
    size_t max_body_;         // Content-Length cap (0 = unlimited) -> 413

    std::chrono::steady_clock::time_point last_activity_;

    State state_ = State::ReadingHead;
    std::string in_;         // client bytes while still parsing the head
    std::string out_;        // bytes to write back to the client
    std::string upstream_;   // rewritten request head handed to the backend
    Backend backend_target_;
    size_t head_len_ = 0;      // length of the parsed request head in in_
    size_t body_remaining_ = 0;  // request body bytes still to forward
    bool responded_ = false;   // have we sent any response bytes yet?
    bool handshake_done_ = false;
    bool closed_ = false;      // guards against re-entrant teardown
    bool read_paused_ = false;   // our client reads paused (upload backpressure)
    bool backend_paused_ = false;  // we paused the backend (download backpressure)
    uint32_t interest_ = 0;    // current epoll interest (avoid redundant updates)
    IoStatus read_block_ = IoStatus::WouldBlockRead;   // last read-side block dir
    IoStatus write_block_ = IoStatus::Ok;              // last write-side block dir
};

}  // namespace castle
