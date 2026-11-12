// port/net/gdx_net_socket.h: thin non-blocking UDP socket for the online layer, IPv4 and IPv6 (O5.3).
//
// One class covers Winsock2 on Windows and BSD sockets elsewhere, so the protocol code above it
// never includes an OS header. Nothing here blocks, nothing throws, and the only process-wide state
// is the Winsock start-up count kept in the .cpp. A UdpSocket is not thread-safe: use one owner
// thread per socket.

#ifndef GDX_NET_SOCKET_H
#define GDX_NET_SOCKET_H

#include "gdx_net_address.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx {
namespace net {

// ip holds 16 bytes in network order, an IPv4 address IPv4-mapped (gdx_net_address.h). All zero is the
// unspecified address "::". port is in host byte order.
struct NetAddress {
    IpBytes ip{};
    uint16_t port = 0;

    // a.b.c.d given as (a << 24) | (b << 16) | (c << 8) | d, so 127.0.0.1:24302 is FromV4(0x7F000001, 24302).
    static NetAddress FromV4(uint32_t ip4, uint16_t port) {
        NetAddress a;
        a.ip = MappedV4(ip4);
        a.port = port;
        return a;
    }

    bool IsV4() const { return IsMappedV4(ip); }
    // The IPv4 address in the FromV4 form; 0 for an IPv6 address.
    uint32_t Ipv4() const { return V4Of(ip); }

    bool operator==(const NetAddress& other) const { return ip == other.ip && port == other.port; }
    bool operator!=(const NetAddress& other) const { return !(*this == other); }
};

// Accepts "a.b.c.d:port", four decimal octets without leading zeros, or "[v6]:port", eight groups of one to four
// hex digits with at most one "::" (RFC 4291 2.2, without a dotted IPv4 tail or a zone). The port runs from 0 to
// 65535. There is no name lookup, so "localhost:1" is rejected. On failure out is left unchanged.
bool ParseAddress(const char* text, NetAddress& out);

// Returns the canonical text that ParseAddress reads back to the same address: "a.b.c.d:port" for IPv4, and
// "[v6]:port" for IPv6 in the RFC 5952 form (lower case, no leading zeros, the longest run of two or more zero
// groups as "::").
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

    // Binds a non-blocking UDP socket. Port 0 asks the OS for an ephemeral port.
    //  - An IPv4 address (mapped), 0.0.0.0 included, opens an IPv4 socket.
    //  - The unspecified address "::" opens one dual-stack socket that reaches IPv4 and IPv6 peers. Where the
    //    OS has no IPv6, it opens an IPv4 socket on 0.0.0.0 instead, and DualStack() says so.
    //  - Any other IPv6 address opens an IPv6-only socket bound to it.
    // Returns false if this object is already open, the bind fails or the OS refuses a socket.
    bool Open(const NetAddress& bindAddr);
    void Close();
    bool IsOpen() const { return handle_ != kNoHandle; }
    // The bound port in host byte order, or 0 while closed.
    uint16_t LocalPort() const { return localPort_; }
    // True when the open socket is IPv6, so it can send to IPv6 addresses. An IPv4 socket refuses them.
    bool Ipv6() const { return ipv6_; }
    // True when the open socket reaches both families.
    bool DualStack() const { return dualStack_; }

    // Lets SendTo reach a broadcast address such as 255.255.255.255 (LAN room discovery). Off by
    // default, as the OS has it. Use an IPv4 socket for it: IPv6 has no broadcast. Returns false while closed
    // or when the OS refuses the option.
    bool SetBroadcast(bool on);

    // Error as well when the socket cannot reach the address's family: an IPv6 address from an IPv4 socket, or
    // an IPv4 address from an IPv6-only one.
    SendResult SendTo(const NetAddress& to, const void* data, std::size_t len);
    RecvResult RecvFrom(void* buf, std::size_t cap, std::size_t& outLen, NetAddress& from);

private:
    // The OS handle (SOCKET or int) kept as a plain integer. -1 is invalid on both platforms: on
    // Windows INVALID_SOCKET is ~0, which converts back to -1 through intptr_t.
    static constexpr std::intptr_t kNoHandle = -1;

    std::intptr_t handle_ = kNoHandle;
    uint16_t localPort_ = 0;
    bool ipv6_ = false;
    bool dualStack_ = false;
    // Staging area that RecvFrom reads into before copying; see the .cpp for why.
    std::vector<unsigned char> scratch_;
};

}  // namespace net
}  // namespace gdx

#endif  // GDX_NET_SOCKET_H
