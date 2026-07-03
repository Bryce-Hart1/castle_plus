// Claude — Date 06/19/2026
// TLS transport: an OpenSSL SSL* driven in non-blocking mode over an owned
// socket. SSL_set_fd uses a BIO_NOCLOSE socket BIO, so SSL_free does NOT close
// the fd — the owned Socket does. recv/send map OpenSSL's WANT_READ/WANT_WRITE
// onto the transport's would-block directions.
#pragma once

#include <openssl/ssl.h>

#include <cstddef>

#include "net/socket.h"
#include "net/transport.h"

namespace castle {

class TlsTransport : public Transport {
public:
    TlsTransport(Socket sock, SSL* ssl) : sock_(std::move(sock)), ssl_(ssl) {}
    ~TlsTransport() override;

    int fd() const override { return sock_.get(); }
    IoStatus handshake() override;
    IoStatus recv(char* buf, size_t n, size_t& out_n) override;
    IoStatus send(const char* buf, size_t n, size_t& out_n) override;

private:
    IoStatus map_error(int ret);

    Socket sock_;
    SSL* ssl_;
};

}  // namespace castle
