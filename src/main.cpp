// castle++ entry point. M1: spin up one epoll EventLoop per core, each with its
// own SO_REUSEPORT listener on the same port, all serving the echo connection.
// SIGINT/SIGTERM are blocked everywhere and handled here via sigwait, so
// shutdown is a clean, single-threaded affair: signal -> stop every loop.
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h> // Linux event poll
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/service_config.h"
#include "control/control_server.h"
#include "gateway/rate_limiter.h"
#include "http/http_connection.h"
#include "http/router.h"
#include "net/conn_limit.h"
#include "net/echo_connection.h"
#include "net/event_loop.h"
#include "net/listener.h"
#include "net/socket.h"
#include "net/transport.h"
#include "supervisor/supervisor.h"
#include "tls/tls_context.h"
#include "util/log.h"
#include "util/printHelper.hpp" //for print function at startup

namespace {

struct Options {
    uint16_t port = 8080;
    unsigned threads = 0;                       // 0 => hardware_concurrency()
    std::string control_path = "/tmp/castle.sock";  // "" disables control plane
    std::string services_path;  // services manifest; empty disables supervisor
    std::string routes_path;    // routes file; empty => echo mode (M1)
    int timeout_sec = 30;       // idle connection timeout; 0 disables
    std::string tls_cert;       // PEM cert; with tls_key => HTTPS (proxy only)
    std::string tls_key;        // PEM private key
    size_t max_body = 1024ull * 1024 * 1024;  // request body cap (0 = unlimited)
    std::string log_file;  // WARN/ERROR mirrored here; empty = stderr only
    size_t max_conn = 0;   // max concurrent client connections (0 = unlimited)
    long rate = 0;         // per-IP connections/sec (0 = rate limiting off)
    long rate_burst = 0;   // token bucket size (0 => defaults to `rate`)
};

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [--port N] [--threads N] [--control PATH] "
                 "[--services PATH]\n"
                 "  --port N        TCP port to listen on (default 8080)\n"
                 "  --threads N     worker loops; 0/omitted = one per core\n"
                 "  --control PATH  unix control socket (default "
                 "/tmp/castle.sock; \"\" disables)\n"
                 "  --services PATH services manifest to supervise (omit = off)\n"
                 "  --routes PATH   routes file -> HTTP proxy (omit = echo mode)\n"
                 "  --timeout N     idle connection timeout, seconds "
                 "(default 30; 0 disables)\n"
                 "  --tls-cert PATH PEM certificate  (with --tls-key => HTTPS)\n"
                 "  --tls-key PATH  PEM private key\n"
                 "  --max-body N    max request body bytes (default 1 GiB; "
                 "0 = unlimited)\n"
                 "  --log-file PATH mirror WARN/ERROR to this file (retrievable "
                 "later)\n"
                 "  --max-conn N    max concurrent connections (0 = unlimited)\n"
                 "  --rate N        per-IP connections/sec, token bucket "
                 "(0 = off)\n"
                 "  --rate-burst N  token bucket size (default = --rate)\n",
                 prog);
}

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](long& out) -> bool {
            if (i + 1 >= argc){
             return false;   
            }
            out = std::strtol(argv[++i], nullptr, 10);
            return true;
        };
        long val = 0;
        if (arg == "--port" && next(val) && val > 0 && val <= 65535) {
            opts.port = static_cast<uint16_t>(val);
        } else if (arg == "--threads" && next(val) && val >= 0) {
            opts.threads = static_cast<unsigned>(val);
        } else if (arg == "--control") {
            if (i + 1 >= argc) return false;
            opts.control_path = argv[++i];
        } else if (arg == "--services") {
            if (i + 1 >= argc) return false;
            opts.services_path = argv[++i];
        } else if (arg == "--routes") {
            if (i + 1 >= argc) return false;
            opts.routes_path = argv[++i];
        } else if (arg == "--timeout" && next(val) && val >= 0) {
            opts.timeout_sec = static_cast<int>(val);
        } else if (arg == "--max-body" && next(val) && val >= 0) {
            opts.max_body = static_cast<size_t>(val);
        } else if (arg == "--log-file") {
            if (i + 1 >= argc) return false;
            opts.log_file = argv[++i];
        } else if (arg == "--max-conn" && next(val) && val >= 0) {
            opts.max_conn = static_cast<size_t>(val);
        } else if (arg == "--rate" && next(val) && val >= 0) {
            opts.rate = val;
        } else if (arg == "--rate-burst" && next(val) && val >= 0) {
            opts.rate_burst = val;
        } else if (arg == "--tls-cert") {
            if (i + 1 >= argc) return false;
            opts.tls_cert = argv[++i];
        } else if (arg == "--tls-key") {
            if (i + 1 >= argc) return false;
            opts.tls_key = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            return false;
        } else {
            std::fprintf(stderr, "Unknown/invalid argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// The connection factory wired into every listener. Swapping this is how later
// milestones change what the server speaks without touching the accept path.
std::unique_ptr<castle::EventHandler> make_echo(castle::EventLoop& loop, castle::Socket sock) {
    return std::make_unique<castle::EchoConnection>(loop, std::move(sock));
}

}  // namespace



int main(int argc, char** argv) {
    printHelper::printLogo();
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 2;
    }

    unsigned n_threads = opts.threads ? opts.threads : std::max(1u, std::thread::hardware_concurrency());

    // Bring up file logging first, so even early startup errors are captured.
    castle::log_init(opts.log_file);

    // Public-facing hardening: cap concurrent connections, and (optionally) a
    // per-IP token-bucket rate limiter shared by all listeners.
    castle::max_conn_limit() = opts.max_conn;
    std::unique_ptr<castle::RateLimiter> limiter;
    if (opts.rate > 0) {
        double burst = opts.rate_burst > 0 ? opts.rate_burst : opts.rate;
        limiter = std::make_unique<castle::RateLimiter>(
            static_cast<double>(opts.rate), burst);
    }

    // Writing to a socket whose peer has gone away must yield EPIPE, not a
    // process-killing signal.
    ::signal(SIGPIPE, SIG_IGN);

    // Block SIGINT/SIGTERM/SIGHUP/SIGCHLD in this (and every spawned) thread.
    // SIGINT/SIGTERM/SIGHUP are consumed by the sigwait below; SIGCHLD by the
    // supervisor's signalfd. Blocking SIGHUP also stops its default action
    // (terminate) — so a stray `kill -HUP` reloads instead of killing us.
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);
    sigaddset(&block_mask, SIGHUP);
    sigaddset(&block_mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_mask, nullptr);

    // Choose what the listeners speak: HTTP reverse proxy when routes are given,
    // otherwise the echo server. `router` is immutable after load(), so every
    // loop shares it by const reference with no locking; it must outlive the
    // workers, so it lives here in main.
    castle::Router router;
    castle::TlsContext tlsctx;
    castle::Listener::ConnFactory factory;
    bool tls_enabled = false;

    if (!opts.routes_path.empty()) {
        std::string rerr;
        if (!router.load(opts.routes_path, rerr)) {
            LOG_ERROR("routes: %s", rerr.c_str());
            return 1;
        }

        if (!opts.tls_cert.empty() || !opts.tls_key.empty()) {
            if (opts.tls_cert.empty() || opts.tls_key.empty()) {
                LOG_ERROR("--tls-cert and --tls-key must be given together");
                return 1;
            }
            std::string terr;
            if (!tlsctx.init(opts.tls_cert, opts.tls_key, terr)) {
                LOG_ERROR("tls: %s", terr.c_str());
                return 1;
            }
            tls_enabled = true;
        }

        auto timeout = std::chrono::seconds(opts.timeout_sec);
        size_t max_body = opts.max_body;
        if (tls_enabled) {
            factory = [&router, &tlsctx, timeout, max_body](
                          castle::EventLoop& loop, castle::Socket sock) {
                std::unique_ptr<castle::HttpConnection> conn;
                auto transport = tlsctx.wrap(std::move(sock));  // TLS-wrap the fd
                if (transport)
                    conn = std::make_unique<castle::HttpConnection>(
                        loop, std::move(transport), router, timeout, max_body);
                return conn;  // null on rare SSL_new failure -> listener skips
            };
        } else {
            factory = [&router, timeout, max_body](castle::EventLoop& loop,
                                                   castle::Socket sock) {
                return std::make_unique<castle::HttpConnection>(
                    loop,
                    std::make_unique<castle::PlainTransport>(std::move(sock)),
                    router, timeout, max_body);
            };
        }
    } else {
        if (!opts.tls_cert.empty()) {
            LOG_ERROR("--tls-cert requires --routes (TLS is for the proxy)");
            return 1;
        }
        factory = make_echo;
    }

    std::vector<std::unique_ptr<castle::EventLoop>> loops;
    loops.reserve(n_threads); //reserve the number of threads available

    for (unsigned i = 0; i < n_threads; ++i) {
        auto loop = std::make_unique<castle::EventLoop>();

        std::string err;
        castle::Socket lsock = castle::make_reuseport_listener(opts.port, 1024, err);
        if (!lsock.valid()) {
            LOG_ERROR("failed to create listener #%u: %s", i, err.c_str());
            return 1;
        }

        loop->add(std::make_unique<castle::Listener>(*loop, std::move(lsock), factory, limiter.get()), EPOLLIN | EPOLLET);
        if (opts.timeout_sec > 0) loop->enable_idle_timeouts(1000);  // sweep 1/s
        loops.push_back(std::move(loop));
    }

    LOG_INFO("castle++ listening on port %u with %u worker loop(s) [%s]",
             opts.port, n_threads,
             opts.routes_path.empty() ? "echo"
                                      : (tls_enabled ? "https" : "http-proxy"));

    std::vector<std::thread> workers;
    workers.reserve(n_threads);
    for (unsigned i = 0; i < n_threads; ++i) {
        castle::EventLoop* lp = loops[i].get();
        workers.emplace_back([lp] { lp->run(); });
    }

    // Start the process supervisor (its own EventLoop thread) if a manifest was
    // given. It launches and babysits the app backends.
    std::unique_ptr<castle::Supervisor> supervisor;
    if (!opts.services_path.empty()) {
        std::vector<castle::ServiceConfig> configs;
        std::string err;
        if (!castle::parse_services_file(opts.services_path, configs, err)) {
            LOG_ERROR("services manifest: %s", err.c_str());
            for (auto& loop : loops) loop->stop();
            for (auto& w : workers) w.join();
            return 1;
        }
        supervisor = std::make_unique<castle::Supervisor>(std::move(configs));
        if (!supervisor->start()) {
            LOG_ERROR("supervisor failed to start");
            supervisor.reset();
        }
    }

    // Bring up the control plane on its own thread (off the data path). Its
    // `shutdown` command pokes us with SIGTERM, which the sigwait below catches
    // — the exact same clean path as Ctrl-C.
    std::unique_ptr<castle::ControlServer> control;
    if (!opts.control_path.empty()) {
        std::vector<castle::EventLoop*> loop_ptrs;
        loop_ptrs.reserve(loops.size());
        for (auto& loop : loops) loop_ptrs.push_back(loop.get());
        control = std::make_unique<castle::ControlServer>(
            opts.control_path, std::move(loop_ptrs), 
            [] { ::kill(::getpid(), SIGTERM); }, supervisor.get()); 
        if (!control->start()) {
            LOG_WARN("control plane disabled (failed to start on %s)",
                     opts.control_path.c_str());
            control.reset();
        }
    }

    // Park until someone asks us to quit (Ctrl-C, SIGTERM, `shutdown`) or to
    // reload the TLS cert (SIGHUP / `systemctl reload`). SIGHUP loops; the
    // others fall through to shutdown.
    sigset_t wait_mask;
    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, SIGINT);
    sigaddset(&wait_mask, SIGTERM);
    sigaddset(&wait_mask, SIGHUP);
    int sig = 0;
    for (;;) {
        sigwait(&wait_mask, &sig);
        if (sig == SIGHUP) {
            if (tls_enabled) {
                std::string terr;
                if (tlsctx.reload(terr))
                    LOG_INFO("SIGHUP: reloaded TLS certificate");
                else
                    LOG_ERROR("SIGHUP: cert reload failed (keeping current): %s",
                              terr.c_str());
            } else {
                LOG_INFO("SIGHUP: no TLS cert configured; nothing to reload");
            }
            continue;
        }
        break;  // SIGINT / SIGTERM
    }
    LOG_INFO("received signal %d, shutting down...", sig);

    // Order: stop admin (no command races teardown), stop the supervisor (it
    // SIGTERMs then SIGKILLs the backends), then stop the loops and join.
    if (control) control->stop();
    if (supervisor) supervisor->stop();
    for (auto& loop : loops) loop->stop();
    for (auto& w : workers) w.join();

    LOG_INFO("castle++ stopped cleanly");
    return 0;
}
