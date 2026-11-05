// port/net/gdx_net_socket.h: thin non-blocking UDP socket for the online layer (IPv4 only).
//
// One class covers Winsock2 on Windows and BSD sockets elsewhere, so the protocol code above it
// never includes an OS header. Nothing here blocks, nothing throws, and the only process-wide state
// is the Winsock start-up count kept in the .cpp. A UdpSocket is not thread-safe: use one owner
// thread per socket.

#ifndef GDX_NET_SOCKET_H
#define GDX_NET_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx {
namespace net {

// Both fields are host byte order, so 127.0.0.1:24302 is ip 0x7F000001 and port 24302.
struct NetAddress {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool operator==(const NetAddress& other) const { return ip == other.ip && port == other.port; }
    bool operator!=(const NetAddress& other) const { return !(*this == other); }
};

// Accepts "a.b.c.d:port" only: four decimal octets without leading zeros, and a port from 0 to
// 65535. There is no name lookup, so "localhost:1" is rejected. On failure out is left unchanged.
bool ParseAddress(const char* text, NetAddress& out);

// Returns the canonical "a.b.c.d:port" text that ParseAddress reads back to the same address.
std::string FormatAddress(const NetAddress& addr);

// Ok: the datagram was handed to the OS.
// WouldBlock: the send buffer is full. Nothing was sent, so retry on a later tick.
// Error: this datagram was not sent (bad argument, closed socket or OS failure). The socket stays open.
enum class SendResult { Ok, WouldBlock, Error };

// Ok: one datagram was copied into buf and outLen holds its length.
// Truncated: the datagram was longer than cap. It is consumed and dropped, buf is untouched and
// outLen is 0. A truncated payload is never handed out.
// WouldBlock: nothing is queued. This is the normal idle result, not a failure.
// Error: closed socket, null buffer or OS failure. The socket stays open.
enum class RecvResult { Ok, Truncated, WouldBlock, Error };

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    // Binds a non-blocking IPv4 UDP socket. Port 0 asks the OS for an ephemeral port. Returns false
    // if this object is already open, the bind fails or the OS refuses a socket.
    bool Open(const NetAddress& bindAddr);
    void Close();
    bool IsOpen() const { return handle_ != kNoHandle; }
    // The bound port in host byte order, or 0 while closed.
    uint16_t LocalPort() const { return localPort_; }

    SendResult SendTo(const NetAddress& to, const void* data, std::size_t len);
    RecvResult RecvFrom(void* buf, std::size_t cap, std::size_t& outLen, NetAddress& from);

private:
    // The OS handle (SOCKET or int) kept as a plain integer. -1 is invalid on both platforms: on
    // Windows INVALID_SOCKET is ~0, which converts back to -1 through intptr_t.
    static constexpr std::intptr_t kNoHandle = -1;

    std::intptr_t handle_ = kNoHandle;
    uint16_t localPort_ = 0;
    // Staging area that RecvFrom reads into before copying; see the .cpp for why.
    std::vector<unsigned char> scratch_;
};

}  // namespace net
}  // namespace gdx

#endif  // GDX_NET_SOCKET_H
