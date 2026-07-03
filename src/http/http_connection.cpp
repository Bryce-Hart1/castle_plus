// Claude — Date 06/19/2026
// Client-side proxy implementation, transport-agnostic (plaintext or TLS).
#include "http/http_connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "http/backend_connection.h"
#include "net/event_loop.h"
#include "net/socket.h"
#include "third_party/picohttpparser/picohttpparser.h"
#include "util/log.h"

namespace castle {

namespace {

constexpr size_t kMaxHead = 32 * 1024;         // request head cap  -> 431
constexpr size_t kMaxBody = 8 * 1024 * 1024;   // request body cap  -> 413
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
                               std::chrono::seconds timeout)
    : loop_(loop),
      transport_(std::move(transport)),
      router_(router),
      timeout_(timeout),
      last_activity_(std::chrono::steady_clock::now()),
      interest_(EPOLLIN | EPOLLET) {  // matches how the listener registered us
    loop_.stats().active_connections.fetch_add(1, std::memory_order_relaxed);
    loop_.stats().total_connections.fetch_add(1, std::memory_order_relaxed);
}

HttpConnection::~HttpConnection() {
    if (backend_) backend_->detach_client();
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
        size_t got = 0;
        IoStatus st = transport_->recv(buf, sizeof(buf), got);
        if (st == IoStatus::Ok) {
            touch();
            if (state_ == State::ReadingHead || state_ == State::ReadingBody) {
                in_.append(buf, got);
                loop_.stats().bytes_in.fetch_add(static_cast<uint64_t>(got),
                                                 std::memory_order_relaxed);
                if (state_ == State::ReadingHead) process_head();
                if (state_ == State::ReadingBody) maybe_start_backend();
                if (closed_) return IoStatus::Closed;
            }
            // Streaming/Closing: discard any extra client bytes (no pipelining).
            continue;
        }
        if (st == IoStatus::Closed || st == IoStatus::Error) {
            close_both();
            return st;
        }
        return st;  // WouldBlockRead / WouldBlockWrite
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
    if (state_ == State::Closing) {
        close_both();
        return IoStatus::Ok;
    }
    return IoStatus::Ok;
}

void HttpConnection::recompute_interest() {
    bool want_in = false, want_out = false;

    // Read side: keep reading (more request data, or to notice the client
    // hanging up) unless the read is specifically blocked wanting to write.
    if (read_block_ == IoStatus::WouldBlockWrite)
        want_out = true;
    else
        want_in = true;

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
    if (!(ev & (EPOLLIN | EPOLLOUT))) ev |= EPOLLIN;  // safety net
    set_interest(ev);
}

void HttpConnection::set_interest(uint32_t events) {
    if (events == interest_) return;
    interest_ = events;
    loop_.update(fd(), events);
}

// ---------------------------------------------------------------------------
// Request parsing / routing (unchanged from the plaintext version).
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
    if (content_length < 0 || static_cast<size_t>(content_length) > kMaxBody) {
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
    body_needed_ = static_cast<size_t>(content_length);
    state_ = State::ReadingBody;
}

void HttpConnection::maybe_start_backend() {
    if (in_.size() < head_len_ + body_needed_) return;  // need more body bytes
    upstream_.append(in_.data() + head_len_, body_needed_);
    start_backend();
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
    backend_->send(upstream_);  // queued; flushed when the connect completes
    loop_.add(std::move(bc), EPOLLOUT | EPOLLET);  // EPOLLOUT = connect done
    state_ = State::Streaming;
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
    recompute_interest();
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
