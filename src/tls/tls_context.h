// Claude — Date 06/19/2026
// Owns the server SSL_CTX (cert/key, protocol options) and mints a TlsTransport
// per accepted connection. M4 loads a single cert/key (one cert can still cover
// several hostnames via SANs); multi-cert SNI selection is a small follow-up
// (add more SSL_CTX + a servername callback).
#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <shared_mutex>
#include <string>

#include "net/socket.h"
#include "net/transport.h"

namespace castle {

class TlsContext {
public:
    ~TlsContext();

    // Build the context from PEM files. Returns false + sets err on failure.
    bool init(const std::string& cert_path, const std::string& key_path,
              std::string& err);

    // Re-read the same cert/key files and swap them in live (on SIGHUP, for
    // Let's Encrypt renewals). Thread-safe against concurrent wrap() calls; on
    // failure the current cert is kept. Returns false + sets err on failure.
    bool reload(std::string& err);

    bool valid() const { return ctx_ != nullptr; }

    // Wrap a freshly accepted socket into a server-side TLS transport. Called on
    // worker threads; takes a shared lock so many can mint SSLs concurrently
    // while reload() (exclusive) swaps the context.
    std::unique_ptr<Transport> wrap(Socket sock);

private:
    SSL_CTX* ctx_ = nullptr;
    std::string cert_path_;
    std::string key_path_;
    // Guards ctx_: shared for wrap() (SSL_new is thread-safe on one CTX),
    // exclusive for reload()'s pointer swap + free of the old CTX.
    mutable std::shared_mutex mutex_;
};

}  // namespace castle
