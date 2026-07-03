// Claude — Date 06/19/2026
// Owns the server SSL_CTX (cert/key, protocol options) and mints a TlsTransport
// per accepted connection. M4 loads a single cert/key (one cert can still cover
// several hostnames via SANs); multi-cert SNI selection is a small follow-up
// (add more SSL_CTX + a servername callback).
#pragma once

#include <openssl/ssl.h>

#include <memory>
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

    bool valid() const { return ctx_ != nullptr; }

    // Wrap a freshly accepted socket into a server-side TLS transport.
    std::unique_ptr<Transport> wrap(Socket sock);

private:
    SSL_CTX* ctx_ = nullptr;
};

}  // namespace castle
