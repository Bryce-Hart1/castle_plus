// Claude — Date 06/19/2026 last changed: 10/31/2026 by: Claude
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
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "http/backend_connection.h"
#include "net/conn_limit.h"
#include "net/event_loop.h"
#include "net/socket.h"
#include "supervisor/supervisor.h"
#include "third_party/picohttpparser/picohttpparser.h"
#include "util/log.h"
#include "util/uptime.hpp"

namespace castle {

namespace {

constexpr size_t kMaxHead = 32 * 1024;  // request head cap -> 431
constexpr size_t kMaxHeaders = 100;

// castle's own status paths live under this prefix; it is reserved on every
// Host and never proxied.
constexpr char kInternalPrefix[] = "/_castle/";

// /_castle/health probes run inline on this worker loop (tcp_probe blocks).
// Against 127.0.0.1 backends a connect succeeds or is refused at once, so it
// costs microseconds; the cap only bites for a remote, blackholed backend,
// which then stalls this one loop for at most this long per backend.
constexpr int kInternalProbeMs = 200;

// After answering an error before the request body was read, keep reading (and
// discarding) for a bit before closing. Closing with unread bytes in the socket
// makes the kernel send RST, and the client can lose the response it was owed.
constexpr size_t kLingerMaxBytes = 1024 * 1024;
constexpr auto kLingerTime = std::chrono::seconds(2);

// The challenge that makes a browser prompt for the vault token. Bearer
// clients send their header up front and never see it.
constexpr char kAuthChallenge[] =
    "WWW-Authenticate: Basic realm=\"castle\", charset=\"UTF-8\"\r\n";

// Rejected-credential WARNs, at most one per second process-wide, so a
// scanner can't flood the log file. The count of dropped lines rides along.
void warn_auth(const std::string& route, const char* why) {
    static std::atomic<long long> next_ok{0};
    static std::atomic<unsigned long> dropped{0};
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    long long expected = next_ok.load(std::memory_order_relaxed);
    if (now < expected ||
        !next_ok.compare_exchange_strong(expected, now + 1,
                                         std::memory_order_relaxed)) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    LOG_WARN("http: route '%s' refused: %s (%lu more suppressed)", route.c_str(),
             why, dropped.exchange(0, std::memory_order_relaxed));
}

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

// Strict Content-Length parse: digits only, non-empty, no sign/whitespace/junk
// (picohttpparser already trims OWS around the value). strtol was too lax here
// ("5abc" -> 5, silent saturation on overflow) — and castle frames the body it
// forwards off this value, so front/back disagreement is a request-smuggling
// primitive. The 18-digit cap guarantees the value fits a long.
bool parse_content_length(const char* v, size_t len, long& out) {
    if (len == 0 || len > 18) return false;
    long n = 0;
    for (size_t i = 0; i < len; ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        n = n * 10 + (v[i] - '0');
    }
    out = n;
    return true;
}

}  // namespace

HttpConnection::HttpConnection(EventLoop& loop, std::unique_ptr<Transport> transport, const Router& router, std::chrono::seconds timeout, size_t max_body,
const Supervisor* supervisor)
: loop_(loop), transport_(std::move(transport)), router_(router), supervisor_(supervisor), timeout_(timeout), max_body_(max_body), last_activity_(std::chrono::steady_clock::now()),
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
        if (client_eof_) return IoStatus::WouldBlockRead;   // nothing more coming
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
        if (st == IoStatus::Closed) {
            // A client that half-closes (shutdown(SHUT_WR)) after sending its
            // request is still waiting for the response — tearing down here
            // would destroy a reply that's already in flight. Once the request
            // is fully forwarded, EOF just means "no more request bytes": stop
            // reading and let the response finish draining.
            if (state_ == State::Streaming || state_ == State::Closing) {
                client_eof_ = true;
                return IoStatus::WouldBlockRead;
            }
            close_both();  // EOF mid-head or mid-upload is a genuine abort
            return st;
        }
        if (st == IoStatus::Error) {
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
            // No pipelining: extra client bytes after the request are ignored.
            break;
        case State::Closing:
            if (linger_) {
                lingered_ += n;  // the unread body of a rejected request
                if (lingered_ > kLingerMaxBytes) close_both();
            }
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
        // Lingering: hold the socket open until the client stops sending (EOF),
        // or check_timeout() / the byte cap gives up on it.
        if (linger_ && !client_eof_) return IoStatus::Ok;
        close_both();
        return IoStatus::Ok;
    }
    return IoStatus::Ok;
}

void HttpConnection::recompute_interest() {
    bool want_in = false, want_out = false;

    // Read side: keep reading (more request data / notice client close) unless
    // we're paused for upload backpressure, the client already sent EOF (nothing
    // more will ever arrive), or the read wants to write (TLS).
    if (!read_paused_ && !client_eof_) {
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
    // Watch for readable by default — but not while deliberately paused, nor
    // after client EOF (then we go quiet until there's something to write;
    // EPOLLHUP/EPOLLERR still fire regardless).
    if (!want_in && !want_out && !read_paused_ && !client_eof_) ev |= EPOLLIN;
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
        if (in_.size() > kMaxHead) {
            linger_ = true;  // the rest of the head is still coming
            send_error(431, "Request Header Fields Too Large", "header too large");
        }
        return;  // wait for more bytes
    }
    if (pret == -1) {
        send_error(400, "Bad Request", "malformed request");
        return;
    }
    head_len_ = static_cast<size_t>(pret);

    // castle's own status endpoints: answered right here, before any header
    // handling or routing, on any Host.
    constexpr size_t kPrefixLen = sizeof(kInternalPrefix) - 1;
    if (path_len >= kPrefixLen &&
        std::memcmp(path, kInternalPrefix, kPrefixLen) == 0) {
        std::string target(path, path_len);
        size_t q = target.find('?');
        if (q != std::string::npos) target.resize(q);
        handle_internal(std::string(method, method_len), target);
        return;
    }

    std::string host;
    long content_length = 0;
    bool saw_content_length = false;
    bool chunked = false;
    const char* authz = nullptr;  // Authorization value, held back from `head`
    size_t authz_len = 0;

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
        if (iequals(h.name, h.name_len, "content-length")) {
            // Duplicate Content-Length headers are rejected outright (even
            // identical ones): castle would frame off one while forwarding
            // both, and a backend picking the other is a smuggling desync.
            if (saw_content_length) {
                send_error(400, "Bad Request", "duplicate content-length");
                return;
            }
            saw_content_length = true;
            if (!parse_content_length(h.value, h.value_len, content_length)) {
                send_error(400, "Bad Request", "invalid content-length");
                return;
            }
        }
        if (iequals(h.name, h.name_len, "transfer-encoding")) chunked = true;
        if (iequals(h.name, h.name_len, "authorization")) {
            // Two credentials leave "which one counts" up to whoever reads
            // them; castle and the backend must never disagree on that.
            if (authz) {
                send_error(400, "Bad Request", "duplicate authorization");
                return;
            }
            authz = h.value;
            authz_len = h.value_len;
            continue;  // appended below, only if the route doesn't consume it
        }

        if (is_hop_by_hop(h.name, h.name_len)) continue;
        head.append(h.name, h.name_len);
        head += ": ";
        head.append(h.value, h.value_len);
        head += "\r\n";
    }

    // Checking Transfer-Encoding before ever using content_length is
    // load-bearing for smuggling defense: a TE+CL request must die here, never
    // reach framing. (parse_content_length can't produce negatives, so no < 0
    // check is needed.)
    if (chunked) {
        send_error(501, "Not Implemented", "chunked request body unsupported");
        return;
    }

    // From here on the head is valid, so a refusal knows how much body is still
    // in flight and can linger long enough for the client to read the answer.
    const bool body_unread =
        static_cast<size_t>(content_length) > in_.size() - head_len_;
    auto refuse = [&](int code, const char* status, const char* body,
                      const char* extra = nullptr) {
        linger_ = body_unread;
        send_response(code, status, body, false, extra);
    };

    const std::string target(path, path_len);
    const Route* route = router_.match(host, target);
    if (!route || !route->allows(target)) {
        refuse(404, "Not Found", "no matching route");
        return;
    }

    if (route->auth) {
        const Access access = route->auth->check(authz, authz_len);
        if (access == Access::None) {
            warn_auth(route->name, authz ? "bad credential" : "no credential");
            refuse(401, "Unauthorized", "credential required", kAuthChallenge);
            return;
        }
        // Exact match: methods are case-sensitive, so "get" is not GET.
        const std::string m(method, method_len);
        const bool read_only_method = m == "GET" || m == "HEAD";
        if (access == Access::ReadOnly && !read_only_method) {
            warn_auth(route->name, "read-only credential, write method");
            refuse(403, "Forbidden", "read-only credential");
            return;
        }
        // Consumed here: the credential never travels further downstream.
    } else if (authz) {
        head += "Authorization: ";
        head.append(authz, authz_len);
        head += "\r\n";
    }
    head += "Connection: close\r\n\r\n";

    const size_t max_body = route->max_body.value_or(max_body_);
    if (max_body != 0 && static_cast<size_t>(content_length) > max_body) {
        refuse(413, "Payload Too Large", "request body too large");
        return;
    }

    upstream_ = std::move(head);
    backend_target_ = route->backend;
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

// ---------------------------------------------------------------------------
// castle's own status endpoints. Uptime is never a running timer: it's now()
// minus a start instant stamped once (castle at startup, a backend at spawn).
// Bodies name routes/services only — never backend addresses — since this is
// served on the public listener.
// ---------------------------------------------------------------------------
void HttpConnection::handle_internal(const std::string& method,
                                     const std::string& path) {
    const bool head_only = method == "HEAD";
    if (method != "GET" && !head_only) {
        send_response(405, "Method Not Allowed", "use GET or HEAD\n");
        return;
    }

    if (path == "/_castle/health") {
        bool all_up = true;
        std::string body = render_health(all_up);
        if (all_up)
            send_response(200, "OK", body, head_only);
        else
            send_response(503, "Service Unavailable", body, head_only);
    } else if (path == "/_castle/uptime") {
        send_response(200, "OK", render_uptime(), head_only);
    } else {
        send_response(404, "Not Found", "unknown castle endpoint\n", head_only);
    }
}

// Live reachability: a fresh TCP connect to each distinct backend, not the
// supervisor's cached health flag. castle answering at all proves castle is up.
std::string HttpConnection::render_health(bool& all_up) const {
    std::string out = "castle: up\n";
    all_up = true;

    std::vector<const Backend*> probed;
    for (const auto& r : router_.routes()) {
        // Several routes can share a backend; probe (and list) it once.
        bool dup = false;
        for (const Backend* b : probed) {
            if (b->host == r.backend.host && b->port == r.backend.port) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        probed.push_back(&r.backend);

        const bool up = tcp_probe(r.backend.host, r.backend.port, kInternalProbeMs);
        if (!up) all_up = false;
        out += r.name;
        out += up ? ": up\n" : ": DOWN\n";
    }
    return out;
}

std::string HttpConnection::render_uptime() const {
    using std::chrono::duration_cast;
    using std::chrono::seconds;
    const auto now = std::chrono::steady_clock::now();

    auto line = [](const std::string& name, long long secs) {
        return name + ": " + std::to_string(secs) + "s (" +
               human_duration(secs) + ")";
    };

    const long long castle_secs =
        duration_cast<seconds>(now - process_start()).count();
    std::string out = line("castle", castle_secs) + " since " +
                      wall_timestamp(process_start_wall()) + "\n";

    if (supervisor_) {
        for (const auto& s : supervisor_->uptime_snapshot()) {
            if (!s.running) {
                out += s.name + ": down\n";
                continue;
            }
            out += line(s.name, duration_cast<seconds>(now - s.since).count());
            out += '\n';
        }
    }
    return out;
}

void HttpConnection::send_error(int code, const char* status, const char* body) {
    send_response(code, status, body);
}

void HttpConnection::send_response(int code, const char* status,
                                   const std::string& body, bool head_only,
                                   const char* extra_headers) {
    char head[192];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                  "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                  "Connection: close\r\n",
                  code, status, body.size());
    out_ = head;
    if (extra_headers) out_ += extra_headers;
    out_ += "\r\n";
    if (!head_only) out_ += body;
    responded_ = true;
    state_ = State::Closing;
    if (linger_) linger_deadline_ = std::chrono::steady_clock::now() + kLingerTime;
    write_block_ = pump_write();
    if (closed_) return;
    recompute_interest();
}

// ---------------------------------------------------------------------------
void HttpConnection::check_timeout(std::chrono::steady_clock::time_point now) {
    if (linger_ && state_ == State::Closing && out_.empty() &&
        now > linger_deadline_) {
        close_both();  // the client never stopped sending; give up on it
        return;
    }
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
