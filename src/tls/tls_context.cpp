// Claude — Date 06/19/2026
// SSL_CTX setup + per-connection SSL creation, with live cert reload (SIGHUP).
#include "tls/tls_context.h"

#include <openssl/err.h>

#include <mutex>  // std::unique_lock / std::shared_lock live in <shared_mutex>

#include "tls/tls_transport.h"
#include "util/log.h"

namespace castle {

namespace {

// Build a fully-configured server SSL_CTX from PEM files. Returns nullptr and
// sets err on any failure. Standalone so init() and reload() share one path.
SSL_CTX* build_ctx(const std::string& cert_path, const std::string& key_path,
                   std::string& err) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        err = "SSL_CTX_new failed";
        return nullptr;
    }

    // Modern floor + hardening.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION |
                                 SSL_OP_CIPHER_SERVER_PREFERENCE);
    // Non-blocking friendliness: allow partial writes and let us retry a write
    // with a moved buffer (our out_ buffer shifts as we drain it).
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) <= 0) {
        err = "failed to load certificate: " + cert_path;
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <=
        0) {
        err = "failed to load private key: " + key_path;
        SSL_CTX_free(ctx);
        return nullptr;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        err = "private key does not match certificate";
        SSL_CTX_free(ctx);
        return nullptr;
    }
    return ctx;
}

}  // namespace

TlsContext::~TlsContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

bool TlsContext::init(const std::string& cert_path, const std::string& key_path,
                      std::string& err) {
    SSL_CTX* ctx = build_ctx(cert_path, key_path, err);
    if (!ctx) return false;
    ctx_ = ctx;
    cert_path_ = cert_path;
    key_path_ = key_path;
    return true;  // no lock: init runs before any worker thread starts
}

bool TlsContext::reload(std::string& err) {
    // Build the replacement first, WITHOUT the lock — a bad renewal (missing or
    // mismatched files) must never disturb the running server.
    SSL_CTX* fresh = build_ctx(cert_path_, key_path_, err);
    if (!fresh) return false;

    SSL_CTX* old;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        old = ctx_;
        ctx_ = fresh;
    }
    // Safe to drop our ref now: in-flight connections created before the swap
    // hold their own ref (SSL_new bumps it) and keep the old cert until they
    // close; new wrap() calls see `fresh`.
    if (old) SSL_CTX_free(old);
    return true;
}

std::unique_ptr<Transport> TlsContext::wrap(Socket sock) {
    SSL* ssl;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        ssl = SSL_new(ctx_);  // thread-safe on a shared SSL_CTX
    }
    if (!ssl) {
        LOG_ERROR("tls: SSL_new failed");
        return nullptr;
    }
    SSL_set_fd(ssl, sock.get());  // BIO_NOCLOSE; Socket keeps fd ownership
    SSL_set_accept_state(ssl);    // we're the server
    return std::make_unique<TlsTransport>(std::move(sock), ssl);
}

}  // namespace castle
