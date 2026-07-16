/** read bytes, write them straight back. Trivial behaviour, but it
exercises the full non-blocking read/write/backpressure cycle that the real
HTTP and TLS connections (M2/M4) reuse: drain on read, buffer what won't
flush, toggle EPOLLOUT, and only then close. Treat this as the connection
FSM template, not throwaway code.
*/
#pragma once

#include <string>

#include "net/event_handler.h"
#include "net/socket.h"

namespace castle {

class EventLoop;

class EchoConnection : public EventHandler {
public:
    EchoConnection(EventLoop& loop, Socket sock);
    ~EchoConnection() override;  // decrements the loop's active-connection count

    int fd() const override { return sock_.get(); }
    void on_readable() override;
    void on_writable() override;
    void on_error() override;

private:
    // Try to push pending bytes out; manage EPOLLOUT interest and closing.
    void flush();

    EventLoop& loop_;
    Socket sock_;
    std::string out_; //bytes read but not yet echoed back
    bool want_write_ = false;// currently subscribed to EPOLLOUT?
    bool closing_ = false; // peer sent EOF; close once `out_` is flushed
};

}  // namespace castle
