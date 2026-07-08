// Claude — Date 06/19/2026 last changed: 07/04/2026 by: Claude
// Client-side proxy implementation, transport-agnostic (plaintext or TLS).
// Request bodies are streamed to the backend as they arrive (no whole-body
// buffering), with backpressure in both directions so large uploads/downloads
// stay bounded-memory.
#include "http/http_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "http/backend_connection.h"
#include "net/conn_limit.h"
#include "net/event_loop.h"
#include "net/socket.h"
#include "third_party/picohttpparser/picohttpparser.h"
#include "util/log.h"

namespace castle {

namespace {

constexpr size_t kMaxHead = 32 * 1024;  // request head cap -> 431
constexpr size_t kMaxHeaders = 100;

bool iequals(const char* a, size_t alen, const char* b) {
    size_t blen = std::strlen(b);
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; ++i)
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    return true;
}

// Headers castle++ must not forward verbatim (hop-by-hop / framing we control).
bool is_hop_by_hop(const char* name, size_t len) {
    static const char* kDrop[] = {"connection",       "keep-alive",
                                  "proxy-connection", "transfer-encoding",
                                  "te",               "trailer",
                                  "upgrade"};
    for (const char* d : kDrop)
        if (iequals(name, len, d)) return true;
    return false;
}

}  // namespace

HttpConnection::HttpConnection(EventLoop& loop,
                               std::unique_ptr<Transport> transport,
                               const Router& router,
                               std::chrono::seconds timeout, size_t max_body)
    : loop_(loop),
      transport_(std::move(transport)),
      router_(router),
      timeout_(timeout),
      max_body_(max_body),
      last_activity_(std::chrono::steady_clock::now()),
      interest_(EPOLLIN | EPOLLET) {  // matches how the listener registered us
    active_conn_count().fetch_add(1, std::memory_order_relaxed);  // global cap
    loop_.stats().active_connections.fetch_add(1, std::memory_order_relaxed);
    loop_.stats().total_connections.fetch_add(1, std::memory_order_relaxed);
}

HttpConnection::~HttpConnection() {
    if (backend_) backend_->detach_client();
    active_conn_count().fetch_sub(1, std::memory_order_relaxed);
    loop_.stats().active_connections.fetch_sub(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The unified event pump: handshake, then read, then write, then interest.
// ---------------------------------------------------------------------------
void HttpConnection::drive() {
    if (closed_) return;

    if (!handshake_done_) {
        IoStatus st = transport_->handshake();
        if (st == IoStatus::Ok) {
            handshake_done_ = true;
            touch();
        } else if (st == IoStatus::WouldBlockRead) {
            set_interest(EPOLLIN | EPOLLET);
            return;
        } else if (st == IoStatus::WouldBlockWrite) {
            set_interest(EPOLLOUT | EPOLLET);
            return;
        } else {
            close_both();
            return;
        }
    }

    read_block_ = pump_read();
    if (closed_) return;
    write_block_ = pump_write();
    if (closed_) return;
    recompute_interest();
}

IoStatus HttpConnection::pump_read() {
    char buf[16384];
    for (;;) {
        if (read_paused_) return IoStatus::WouldBlockRead;  // upload backpressure
        size_t got = 0;
        IoStatus st = transport_->recv(buf, sizeof(buf), got);
        if (st == IoStatus::Ok) {
            touch();
            loop_.stats().bytes_in.fetch_add(static_cast<uint64_t>(got),
                                             std::memory_order_relaxed);
            handle_client_bytes(buf, got);
            if (closed_) return IoStatus::Closed;
            continue;
        }
        if (st == IoStatus::Closed || st == IoStatus::Error) {
            close_both();
            return st;
        }
        return st;  // WouldBlockRead / WouldBlockWrite
    }
}

void HttpConnection::handle_client_bytes(const char* data, size_t n) {
    switch (state_) {
        case State::ReadingHead:
            in_.append(data, n);
            process_head();
            break;
        case State::StreamingUp:
            forward_body(data, n);
            break;
        case State::Streaming:
        case State::Closing:
            // No pipelining: extra client bytes after the request are ignored.
            break;
    }
}

IoStatus HttpConnection::pump_write() {
    while (!out_.empty()) {
        size_t sent = 0;
        IoStatus st = transport_->send(out_.data(), out_.size(), sent);
        if (st == IoStatus::Ok) {
            out_.erase(0, sent);
            loop_.stats().bytes_out.fetch_add(static_cast<uint64_t>(sent),
                                              std::memory_order_relaxed);
            touch();
            continue;
        }
        if (st == IoStatus::Closed || st == IoStatus::Error) {
            close_both();
            return st;
        }
        return st;  // WouldBlock*
    }

    // Response drained: if we'd paused the backend (download backpressure),
    // let it resume reading.
    if (backend_paused_ && backend_) {
        backend_paused_ = false;
        backend_->resume_reads();
    }
    if (state_ == State::Closing) {
        close_both();
        return IoStatus::Ok;
    }
    return IoStatus::Ok;
}

void HttpConnection::recompute_interest() {
    bool want_in = false, want_out = false;

    // Read side: keep reading (more request data / notice client close) unless
    // we're paused for upload backpressure, or the read wants to write (TLS).
    if (!read_paused_) {
        if (read_block_ == IoStatus::WouldBlockWrite)
            want_out = true;
        else
            want_in = true;
    }

    // Write side: only relevant while response bytes are still buffered.
    if (!out_.empty()) {
        if (write_block_ == IoStatus::WouldBlockRead)
            want_in = true;
        else
            want_out = true;
    }

    uint32_t ev = EPOLLET;
    if (want_in) ev |= EPOLLIN;
    if (want_out) ev |= EPOLLOUT;
    // Watch for readable by default — but not while deliberately paused (then we
    // go quiet until the backend drains; EPOLLHUP still fires regardless).
    if (!want_in && !want_out && !read_paused_) ev |= EPOLLIN;
    set_interest(ev);
}

void HttpConnection::set_interest(uint32_t events) {
    if (events == interest_) return;
    interest_ = events;
    loop_.update(fd(), events);
}

// ---------------------------------------------------------------------------
// Request head: parse, route, open the backend, and send the head. The body is
// then streamed by forward_body() as it arrives.
// ---------------------------------------------------------------------------
void HttpConnection::process_head() {
    const char* method;
    size_t method_len;
    const char* path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[kMaxHeaders];
    size_t num_headers = kMaxHeaders;

    int pret = phr_parse_request(in_.data(), in_.size(), &method, &method_len,
                                 &path, &path_len, &minor_version, headers,
                                 &num_headers, 0);
    if (pret == -2) {  // incomplete
        if (in_.size() > kMaxHead)
            send_error(431, "Request Header Fields Too Large", "header too large");
        return;  // wait for more bytes
    }
    if (pret == -1) {
        send_error(400, "Bad Request", "malformed request");
        return;
    }
    head_len_ = static_cast<size_t>(pret);

    std::string host;
    long content_length = 0;
    bool chunked = false;

    std::string head;
    head.reserve(head_len_ + 32);
    head.append(method, method_len);
    head += ' ';
    head.append(path, path_len);
    head += " HTTP/1.1\r\n";

    for (size_t i = 0; i < num_headers; ++i) {
        const auto& h = headers[i];
        if (h.name == nullptr) continue;  // header continuation line
        if (iequals(h.name, h.name_len, "host"))
            host.assign(h.value, h.value_len);
        if (iequals(h.name, h.name_len, "content-length"))
            content_length = std::strtol(
                std::string(h.value, h.value_len).c_str(), nullptr, 10);
        if (iequals(h.name, h.name_len, "transfer-encoding")) chunked = true;

        if (is_hop_by_hop(h.name, h.name_len)) continue;
        head.append(h.name, h.name_len);
        head += ": ";
        head.append(h.value, h.value_len);
        head += "\r\n";
    }
    head += "Connection: close\r\n\r\n";

    if (chunked) {
        send_error(501, "Not Implemented", "chunked request body unsupported");
        return;
    }
    if (content_length < 0) {
        send_error(400, "Bad Request", "invalid content-length");
        return;
    }
    if (max_body_ != 0 && static_cast<size_t>(content_length) > max_body_) {
        send_error(413, "Payload Too Large", "request body too large");
        return;
    }

    auto backend = router_.match(host, std::string(path, path_len));
    if (!backend) {
        send_error(404, "Not Found", "no matching route");
        return;
    }

    upstream_ = std::move(head);
    backend_target_ = *backend;
    body_remaining_ = static_cast<size_t>(content_length);

    start_backend();  // sends the head; leaves state_ == ReadingHead on success
    if (state_ == State::Closing || closed_) return;  // backend failed -> error

    if (body_remaining_ == 0) {
        state_ = State::Streaming;  // no body; just await the response
    } else {
        state_ = State::StreamingUp;
        // Forward any body bytes that arrived in the same read as the head.
        size_t buffered = in_.size() - head_len_;
        if (buffered > 0) forward_body(in_.data() + head_len_, buffered);
    }
    in_.clear();
    in_.shrink_to_fit();
}

void HttpConnection::forward_body(const char* data, size_t n) {
    if (!backend_) return;
    size_t take = std::min(n, body_remaining_);
    if (take > 0) {
        backend_->send(data, take);
        body_remaining_ -= take;
    }
    if (body_remaining_ == 0) {
        state_ = State::Streaming;  // request fully forwarded
    } else if (backend_->pending_bytes() > kBackpressureHigh) {
        read_paused_ = true;  // upload backpressure: stop reading the client body
    }
}

void HttpConnection::start_backend() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        send_error(502, "Bad Gateway", "cannot create upstream socket");
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend_target_.port);
    if (::inet_pton(AF_INET, backend_target_.host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        send_error(502, "Bad Gateway", "bad backend address");
        return;
    }

    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) {
        ::close(fd);
        send_error(502, "Bad Gateway", "cannot connect to backend");
        return;
    }

    auto bc =
        std::make_unique<BackendConnection>(loop_, Socket(fd), this, timeout_);
    backend_ = bc.get();
    backend_->send(upstream_);  // head; queued, flushed when the connect completes
    loop_.add(std::move(bc), EPOLLOUT | EPOLLET);  // EPOLLOUT = connect done
}

// ---------------------------------------------------------------------------
// Backend-driven callbacks (run on the same loop/thread).
// ---------------------------------------------------------------------------
void HttpConnection::deliver_to_client(const char* data, size_t n) {
    if (closed_) return;
    responded_ = true;
    out_.append(data, n);
    write_block_ = pump_write();
    if (closed_) return;
    // Download backpressure: if the client is slow and the response is piling
    // up, pause reading from the backend until we drain.
    if (backend_ && !backend_paused_ && out_.size() > kBackpressureHigh) {
        backend_->pause_reads();
        backend_paused_ = true;
    }
    recompute_interest();
}

void HttpConnection::on_backend_drained() {
    if (closed_ || !read_paused_) return;
    read_paused_ = false;  // upload backpressure released
    drive();               // resume reading the request body
}

void HttpConnection::on_backend_closed() {
    backend_ = nullptr;  // backend closes itself
    state_ = State::Closing;
    write_block_ = pump_write();  // drain remaining response, then close
    if (closed_) return;
    recompute_interest();
}

void HttpConnection::on_backend_error(const char* why) {
    backend_ = nullptr;
    if (!responded_)
        send_error(502, "Bad Gateway", why);
    else
        close_both();  // headers already sent; nothing graceful to do
}

void HttpConnection::send_error(int code, const char* status, const char* body) {
    char head[192];
    int blen = static_cast<int>(std::strlen(body));
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                  "Content-Length: %d\r\nConnection: close\r\n\r\n",
                  code, status, blen);
    out_ = head;
    out_ += body;
    responded_ = true;
    state_ = State::Closing;
    write_block_ = pump_write();
    if (closed_) return;
    recompute_interest();
}

// ---------------------------------------------------------------------------
void HttpConnection::check_timeout(std::chrono::steady_clock::time_point now) {
    if (timeout_.count() <= 0) return;
    if (now - last_activity_ > timeout_) {
        LOG_WARN("http: closing idle client (fd %d, idle > %llds)", fd(),
                 static_cast<long long>(timeout_.count()));
        close_both();
    }
}

void HttpConnection::close_both() {
    if (closed_) return;
    closed_ = true;
    if (backend_) {
        backend_->detach_client();
        loop_.request_close(backend_->fd());
        backend_ = nullptr;
    }
    loop_.request_close(fd());
}

}  // namespace castle
