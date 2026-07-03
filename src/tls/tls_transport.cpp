// Claude — Date 06/19/2026
// Non-blocking OpenSSL I/O. Every call can come back WANT_READ or WANT_WRITE
// regardless of whether we asked to read or write; map_error turns that into the
// direction the caller should wait on.
#include "tls/tls_transport.h"

namespace castle {

TlsTransport::~TlsTransport() {
    if (ssl_) SSL_free(ssl_);  // BIO_NOCLOSE: fd is closed by sock_'s dtor
}

IoStatus TlsTransport::handshake() {
    int r = SSL_do_handshake(ssl_);
    if (r == 1) return IoStatus::Ok;
    return map_error(r);
}

IoStatus TlsTransport::recv(char* buf, size_t n, size_t& out_n) {
    out_n = 0;
    int r = SSL_read(ssl_, buf, static_cast<int>(n));
    if (r > 0) {
        out_n = static_cast<size_t>(r);
        return IoStatus::Ok;
    }
    return map_error(r);
}

IoStatus TlsTransport::send(const char* buf, size_t n, size_t& out_n) {
    out_n = 0;
    int r = SSL_write(ssl_, buf, static_cast<int>(n));
    if (r > 0) {
        out_n = static_cast<size_t>(r);
        return IoStatus::Ok;
    }
    return map_error(r);
}

IoStatus TlsTransport::map_error(int ret) {
    switch (SSL_get_error(ssl_, ret)) {
        case SSL_ERROR_WANT_READ:
            return IoStatus::WouldBlockRead;
        case SSL_ERROR_WANT_WRITE:
            return IoStatus::WouldBlockWrite;
        case SSL_ERROR_ZERO_RETURN:  // peer sent close_notify
            return IoStatus::Closed;
        case SSL_ERROR_SYSCALL:      // EOF or transport error mid-stream
            return IoStatus::Closed;
        default:
            return IoStatus::Error;
    }
}

}  // namespace castle
