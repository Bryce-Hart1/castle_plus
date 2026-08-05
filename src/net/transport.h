// refactored Bryce Hart Jul 21
// Byte-transport abstraction so a connection doesn't care whether it's talking
// plaintext or TLS. The crucial detail for TLS: a read can need to WRITE (and a
// write can need to READ) mid-handshake/key-update, so recv/send report the
// *direction* they're blocked on, and the connection sets its epoll interest
// accordingly. PlainTransport lives here; TlsTransport is in tls/.
#pragma once

#include <cstddef>

#include "net/socket.h"

namespace castle {

enum class IoStatus {
    Ok,              // made progress (out_n bytes)
    WouldBlockRead,  // retry when the fd is readable
    WouldBlockWrite,  // retry when the fd is writable (TLS can want this on read)
    Closed,          // peer closed cleanly
    Error,           // fatal
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual int fd() const = 0;

    // Advance a TLS handshake. Plaintext returns Ok immediately.
    virtual IoStatus handshake() = 0;

    virtual IoStatus recv(char* buf, size_t n, size_t& out_n) = 0;
    
    virtual IoStatus send(const char* buf, size_t n, size_t& out_n) = 0;
};

// Raw TCP: recv/send are read()/write(); handshake is a no-op.
class PlainTransport : public Transport {
public:
    explicit PlainTransport(Socket sock) : sock_(std::move(sock)) {}

    int fd() const override { 
        return sock_.get(); 
    }
    IoStatus handshake() override {
         return IoStatus::Ok; 
        }
    IoStatus recv(char* buf, size_t n, size_t& out_n) override;

    IoStatus send(const char* buf, size_t n, size_t& out_n) override;

private:
    Socket sock_;
};

}  // namespace castle
