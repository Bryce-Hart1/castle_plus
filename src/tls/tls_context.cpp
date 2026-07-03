// Claude — Date 06/19/2026
// SSL_CTX setup + per-connection SSL creation.
#include "tls/tls_context.h"

#include <openssl/err.h>

#include "tls/tls_transport.h"
#include "util/log.h"

namespace castle {

TlsContext::~TlsContext() {
    if (ctx_) SSL_CTX_free(ctx_);
}

bool TlsContext::init(const std::string& cert_path, const std::string& key_path,
                      std::string& err) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        err = "SSL_CTX_new failed";
        return false;
    }

    // Modern floor + hardening.
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_RENEGOTIATION |
                                  SSL_OP_CIPHER_SERVER_PREFERENCE);
    // Non-blocking friendliness: allow partial writes and let us retry a write
    // with a moved buffer (our out_ buffer shifts as we drain it).
    SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                               SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path.c_str()) <= 0) {
        err = "failed to load certificate: " + cert_path;
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(), SSL_FILETYPE_PEM) <=
        0) {
        err = "failed to load private key: " + key_path;
        return false;
    }
    if (!SSL_CTX_check_private_key(ctx_)) {
        err = "private key does not match certificate";
        return false;
    }
    return true;
}

std::unique_ptr<Transport> TlsContext::wrap(Socket sock) {
    SSL* ssl = SSL_new(ctx_);
    if (!ssl) {
        LOG_ERROR("tls: SSL_new failed");
        return nullptr;
    }
    SSL_set_fd(ssl, sock.get());  // BIO_NOCLOSE; Socket keeps fd ownership
    SSL_set_accept_state(ssl);    // we're the server
    return std::make_unique<TlsTransport>(std::move(sock), ssl);
}

}  // namespace castle
