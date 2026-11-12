// port/net/gdx_net_socket.cpp: Winsock2 and BSD socket back end for gdx::net::UdpSocket.

#include "gdx_net_socket.h"

#include <cstring>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// SIO_UDP_CONNRESET is defined in mswsock.h, not winsock2.h. Without this include the guard in
// CreateNative compiles to nothing and the connreset switch is never sent.
#include <mswsock.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gdx {
namespace net {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSockLen = int;
#else
using NativeSocket = int;
using NativeSockLen = socklen_t;
#endif

constexpr std::intptr_t kNativeInvalid = -1;

// The largest IPv4 UDP payload: 65535 minus the 8-byte UDP header and the 20-byte IP header.
constexpr std::size_t kMaxPayload = 65507;

// Every receive lands in this staging area first and is copied out only if it fits. 64 KB holds
// any UDP datagram, so the length check alone decides truncation. Relying on the OS instead would
// differ per platform: Windows fails with WSAEMSGSIZE, while Linux and BSD disagree on whether a
// short read is flagged at all.
constexpr std::size_t kScratchBytes = 65536;

// A stale ICMP error from an earlier send to a closed port is reported on the next receive and
// uses up that call. A few retries reach the datagram queued behind it. The cap stops a burst of
// errors from holding the caller's frame; past it the call reports WouldBlock.
constexpr int kMaxRecvRetries = 8;

#ifdef _WIN32
// The first Open starts Winsock and the last Close stops it. The mutex makes the first WSAStartup
// finish before another thread's socket call can read the count.
std::mutex gWinsockMutex;
int gWinsockUsers = 0;

bool AcquireWinsock() {
    std::lock_guard<std::mutex> lock(gWinsockMutex);
    if (gWinsockUsers == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
    }
    gWinsockUsers++;
    return true;
}

void ReleaseWinsock() {
    std::lock_guard<std::mutex> lock(gWinsockMutex);
    if (gWinsockUsers > 0 && --gWinsockUsers == 0) {
        WSACleanup();
    }
}
#else
// BSD sockets need no start-up or teardown call.
bool AcquireWinsock() {
    return true;
}

void ReleaseWinsock() {
}
#endif

// Creates a non-blocking UDP socket of the family (AF_INET or AF_INET6), or returns kNativeInvalid. An IPv6
// socket is dual-stack when dual is true and IPv6-only otherwise: the default differs per OS, so it is always set.
std::intptr_t CreateNative(int family, bool dual) {
#ifdef _WIN32
    const NativeSocket sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return kNativeInvalid;
    }
    // Windows reports an ICMP port-unreachable from one send as WSAECONNRESET on a later receive,
    // even when that later receive is for another peer. Turning the report off keeps a closed
    // port from poisoning the queue. RecvFrom still maps the error in case this call fails.
#ifdef SIO_UDP_CONNRESET
    BOOL disable = FALSE;
    DWORD unused = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &disable, sizeof(disable), nullptr, 0, &unused, nullptr, nullptr);
#endif
    // Without it, Windows lets another socket bind the IPv4 side of a dual-stack port that is in use, and that
    // socket then gets the IPv4 datagrams meant for this one. Linux refuses such a bind on its own.
    const BOOL exclusive = TRUE;
    if (setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) != 0) {
        closesocket(sock);
        return kNativeInvalid;
    }
    if (family == AF_INET6) {
        const DWORD v6only = dual ? 0 : 1;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0) {
            closesocket(sock);
            return kNativeInvalid;
        }
    }
    u_long nonBlocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) {
        closesocket(sock);
        return kNativeInvalid;
    }
    return static_cast<std::intptr_t>(sock);
#else
    const NativeSocket fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return kNativeInvalid;
    }
    if (family == AF_INET6) {
        const int v6only = dual ? 0 : 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
            close(fd);
            return kNativeInvalid;
        }
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return kNativeInvalid;
    }
    return static_cast<std::intptr_t>(fd);
#endif
}

void CloseNative(std::intptr_t handle) {
#ifdef _WIN32
    closesocket(static_cast<NativeSocket>(handle));
#else
    close(static_cast<NativeSocket>(handle));
#endif
}

// The OS form of addr for a socket of the family. An IPv4 socket takes only IPv4 addresses; an IPv6 socket
// takes every address, IPv4 ones mapped. Returns the length to pass, or 0 when the socket cannot reach addr.
NativeSockLen ToSockaddr(const NetAddress& addr, int family, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    if (family == AF_INET) {
        if (!addr.IsV4()) {
            return 0;
        }
        sockaddr_in& sa = reinterpret_cast<sockaddr_in&>(out);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(addr.Ipv4());
        sa.sin_port = htons(addr.port);
        return static_cast<NativeSockLen>(sizeof(sockaddr_in));
    }
    sockaddr_in6& sa = reinterpret_cast<sockaddr_in6&>(out);
    sa.sin6_family = AF_INET6;
    std::memcpy(&sa.sin6_addr, addr.ip.data(), 16);
    sa.sin6_port = htons(addr.port);
    return static_cast<NativeSockLen>(sizeof(sockaddr_in6));
}

NetAddress FromSockaddr(const sockaddr_storage& ss) {
    NetAddress addr;
    if (ss.ss_family == AF_INET) {
        const sockaddr_in& sa = reinterpret_cast<const sockaddr_in&>(ss);
        addr = NetAddress::FromV4(ntohl(sa.sin_addr.s_addr), ntohs(sa.sin_port));
    } else if (ss.ss_family == AF_INET6) {
        const sockaddr_in6& sa = reinterpret_cast<const sockaddr_in6&>(ss);
        std::memcpy(addr.ip.data(), &sa.sin6_addr, 16);
        addr.port = ntohs(sa.sin6_port);
    }
    return addr;
}

// Creates a socket of the family, binds it to want and reads back the port the OS chose. On any failure the
// socket is closed again, so a failed Open leaves nothing behind.
std::intptr_t OpenNative(const NetAddress& want, int family, bool dual, uint16_t& boundPort) {
    sockaddr_storage sa;
    const NativeSockLen saLen = ToSockaddr(want, family, sa);
    if (saLen == 0) {
        return kNativeInvalid;
    }
    const std::intptr_t handle = CreateNative(family, dual);
    if (handle == kNativeInvalid) {
        return kNativeInvalid;
    }
    // No SO_REUSEADDR. On Windows it lets a second process take over a UDP port that is in use.
    const NativeSocket sock = static_cast<NativeSocket>(handle);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&sa), saLen) != 0) {
        CloseNative(handle);
        return kNativeInvalid;
    }
    sockaddr_storage bound;
    std::memset(&bound, 0, sizeof(bound));
    NativeSockLen boundLen = static_cast<NativeSockLen>(sizeof(bound));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        CloseNative(handle);
        return kNativeInvalid;
    }
    boundPort = FromSockaddr(bound).port;
    return handle;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Reads [begin, end) as the text of an IPv6 address: groups of one to four hex digits split by ':', with at
// most one "::" standing for one or more zero groups. False for anything else.
bool ParseIpv6(const char* begin, const char* end, IpBytes& out) {
    uint16_t head[8] = {};
    uint16_t tail[8] = {};
    int heads = 0;
    int tails = 0;
    bool gap = false;
    const char* p = begin;
    if (end - begin >= 2 && p[0] == ':' && p[1] == ':') {
        gap = true;
        p += 2;
    } else if (p != end && *p == ':') {
        return false; // a single leading colon
    }
    while (p != end) {
        unsigned value = 0;
        int digits = 0;
        while (p != end && HexDigit(*p) >= 0) {
            if (++digits > 4) {
                return false;
            }
            value = value * 16 + static_cast<unsigned>(HexDigit(*p));
            p++;
        }
        if (digits == 0) {
            return false;
        }
        if (heads + tails == 8) {
            return false;
        }
        if (gap) {
            tail[tails++] = static_cast<uint16_t>(value);
        } else {
            head[heads++] = static_cast<uint16_t>(value);
        }
        if (p == end) {
            break;
        }
        if (*p != ':') {
            return false;
        }
        p++;
        if (p != end && *p == ':') {
            if (gap) {
                return false; // a second "::"
            }
            gap = true;
            p++;
        } else if (p == end) {
            return false; // a single trailing colon
        }
    }
    // "::" must stand for at least one group; without it all eight are needed.
    if (gap ? heads + tails > 7 : heads != 8) {
        return false;
    }
    uint16_t groups[8] = {};
    for (int i = 0; i < heads; i++) {
        groups[i] = head[i];
    }
    for (int i = 0; i < tails; i++) {
        groups[8 - tails + i] = tail[i];
    }
    for (int i = 0; i < 8; i++) {
        out[2 * i] = static_cast<uint8_t>(groups[i] >> 8);
        out[2 * i + 1] = static_cast<uint8_t>(groups[i]);
    }
    return true;
}

// Reads [begin, end) as an unsigned decimal of at most five digits with no sign, no spaces and no
// leading zero, and at most maxValue. Anything else returns false and leaves value untouched.
bool ParseDecimal(const char* begin, const char* end, unsigned maxValue, unsigned& value) {
    if (begin == end || end - begin > 5) {
        return false;
    }
    if (*begin == '0' && end - begin > 1) {
        return false;
    }
    unsigned result = 0;
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        result = result * 10 + static_cast<unsigned>(*p - '0');
    }
    if (result > maxValue) {
        return false;
    }
    value = result;
    return true;
}

}  // namespace

bool ParseAddress(const char* text, NetAddress& out) {
    if (text == nullptr) {
        return false;
    }
    if (text[0] == '[') {
        const char* close = std::strchr(text, ']');
        if (close == nullptr || close[1] != ':') {
            return false;
        }
        IpBytes ip{};
        unsigned port = 0;
        if (!ParseIpv6(text + 1, close, ip) || !ParseDecimal(close + 2, text + std::strlen(text), 65535, port)) {
            return false;
        }
        out.ip = ip;
        out.port = static_cast<uint16_t>(port);
        return true;
    }
    // A bare address is refused: a port is always required, so a missing one is a caller bug.
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr) {
        return false;
    }
    unsigned port = 0;
    if (!ParseDecimal(colon + 1, text + std::strlen(text), 65535, port)) {
        return false;
    }
    uint32_t ip = 0;
    const char* part = text;
    for (int i = 0; i < 4; i++) {
        const char* partEnd = part;
        while (partEnd != colon && *partEnd != '.') {
            partEnd++;
        }
        unsigned octet = 0;
        if (!ParseDecimal(part, partEnd, 255, octet)) {
            return false;
        }
        ip = (ip << 8) | octet;
        if (i < 3) {
            // Reaching the colon early means too few octets.
            if (partEnd == colon) {
                return false;
            }
            part = partEnd + 1;
        } else if (partEnd != colon) {
            // Text left before the colon after the fourth octet means too many octets.
            return false;
        }
    }
    out = NetAddress::FromV4(ip, static_cast<uint16_t>(port));
    return true;
}

std::string FormatAddress(const NetAddress& addr) {
    std::string text;
    if (addr.IsV4()) {
        const uint32_t ip = addr.Ipv4();
        for (int shift = 24; shift >= 0; shift -= 8) {
            text += std::to_string((ip >> shift) & 0xFFu);
            text += (shift > 0) ? '.' : ':';
        }
        text += std::to_string(addr.port);
        return text;
    }
    uint16_t groups[8];
    for (int i = 0; i < 8; i++) {
        groups[i] = static_cast<uint16_t>((addr.ip[2 * i] << 8) | addr.ip[2 * i + 1]);
    }
    // The longest run of two or more zero groups, the first one on a tie (RFC 5952 4.2).
    int bestAt = -1;
    int bestLen = 1;
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && groups[j] == 0) {
            j++;
        }
        if (j - i > bestLen) {
            bestAt = i;
            bestLen = j - i;
        }
        i = j == i ? i + 1 : j;
    }
    static const char kHex[] = "0123456789abcdef";
    text += '[';
    for (int i = 0; i < 8; i++) {
        if (i == bestAt) {
            text += "::";
            i += bestLen - 1;
            continue;
        }
        if (i > 0 && i != bestAt + bestLen) {
            text += ':';
        }
        bool started = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const unsigned nibble = (groups[i] >> shift) & 0xFu;
            if (nibble != 0 || started || shift == 0) {
                text += kHex[nibble];
                started = true;
            }
        }
    }
    text += "]:";
    text += std::to_string(addr.port);
    return text;
}

UdpSocket::~UdpSocket() {
    Close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_), localPort_(other.localPort_), ipv6_(other.ipv6_), dualStack_(other.dualStack_),
      scratch_(std::move(other.scratch_)) {
    other.handle_ = kNoHandle;
    other.localPort_ = 0;
    other.ipv6_ = false;
    other.dualStack_ = false;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        localPort_ = other.localPort_;
        ipv6_ = other.ipv6_;
        dualStack_ = other.dualStack_;
        scratch_ = std::move(other.scratch_);
        other.handle_ = kNoHandle;
        other.localPort_ = 0;
        other.ipv6_ = false;
        other.dualStack_ = false;
    }
    return *this;
}

bool UdpSocket::Open(const NetAddress& bindAddr) {
    if (IsOpen()) {
        return false;
    }
    // Sized before any OS call, so a failed allocation cannot leave a socket or a Winsock count behind.
    scratch_.resize(kScratchBytes);
    if (!AcquireWinsock()) {
        return false;
    }
    uint16_t port = 0;
    std::intptr_t handle = kNativeInvalid;
    bool ipv6 = false;
    bool dual = false;
    if (bindAddr.IsV4()) {
        handle = OpenNative(bindAddr, AF_INET, false, port);
    } else if (IsZeroIp(bindAddr.ip)) {
        handle = OpenNative(bindAddr, AF_INET6, true, port);
        ipv6 = dual = handle != kNativeInvalid;
        if (handle == kNativeInvalid) {
            // No IPv6 on this computer: IPv4 alone still reaches every IPv4 peer.
            handle = OpenNative(NetAddress::FromV4(0, bindAddr.port), AF_INET, false, port);
        }
    } else {
        handle = OpenNative(bindAddr, AF_INET6, false, port);
        ipv6 = true;
    }
    if (handle == kNativeInvalid) {
        ReleaseWinsock();
        return false;
    }
    handle_ = handle;
    localPort_ = port;
    ipv6_ = ipv6;
    dualStack_ = dual;
    return true;
}

void UdpSocket::Close() {
    if (!IsOpen()) {
        return;
    }
    CloseNative(handle_);
    handle_ = kNoHandle;
    localPort_ = 0;
    ipv6_ = false;
    dualStack_ = false;
    ReleaseWinsock();
}

bool UdpSocket::SetBroadcast(bool on) {
    if (!IsOpen()) {
        return false;
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
#ifdef _WIN32
    const BOOL value = on ? TRUE : FALSE;
    return setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    const int value = on ? 1 : 0;
    return setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) == 0;
#endif
}

SendResult UdpSocket::SendTo(const NetAddress& to, const void* data, std::size_t len) {
    if (!IsOpen() || data == nullptr || len > kMaxPayload) {
        return SendResult::Error;
    }
    if (ipv6_ && !dualStack_ && to.IsV4()) {
        return SendResult::Error; // an IPv6-only socket cannot reach an IPv4 peer; the OSes differ on the error
    }
    sockaddr_storage sa;
    const NativeSockLen saLen = ToSockaddr(to, ipv6_ ? AF_INET6 : AF_INET, sa);
    if (saLen == 0) {
        return SendResult::Error; // an IPv6 address from an IPv4 socket
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
#ifdef _WIN32
    const int n = sendto(sock, static_cast<const char*>(data), static_cast<int>(len), 0,
                         reinterpret_cast<const sockaddr*>(&sa), saLen);
    if (n == SOCKET_ERROR) {
        return WSAGetLastError() == WSAEWOULDBLOCK ? SendResult::WouldBlock : SendResult::Error;
    }
#else
    const ssize_t n = sendto(sock, data, len, 0, reinterpret_cast<const sockaddr*>(&sa), saLen);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? SendResult::WouldBlock : SendResult::Error;
    }
#endif
    return SendResult::Ok;
}

RecvResult UdpSocket::RecvFrom(void* buf, std::size_t cap, std::size_t& outLen, NetAddress& from) {
    outLen = 0;
    if (!IsOpen() || buf == nullptr) {
        return RecvResult::Error;
    }
    const NativeSocket sock = static_cast<NativeSocket>(handle_);
    for (int attempt = 0; attempt < kMaxRecvRetries; attempt++) {
        sockaddr_storage sa;
        std::memset(&sa, 0, sizeof(sa));
        NativeSockLen saLen = static_cast<NativeSockLen>(sizeof(sa));
#ifdef _WIN32
        const int n = recvfrom(sock, reinterpret_cast<char*>(scratch_.data()), static_cast<int>(scratch_.size()),
                               0, reinterpret_cast<sockaddr*>(&sa), &saLen);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return RecvResult::WouldBlock;
            }
            if (err == WSAEMSGSIZE) {
                // Cannot happen with a 64 KB staging area. If the stack ever reports it anyway,
                // the datagram is still dropped rather than read partially.
                from = FromSockaddr(sa);
                return RecvResult::Truncated;
            }
            if (err == WSAECONNRESET) {
                continue;
            }
            return RecvResult::Error;
        }
        const std::size_t got = static_cast<std::size_t>(n);
#else
        const ssize_t n = recvfrom(sock, scratch_.data(), scratch_.size(), 0,
                                   reinterpret_cast<sockaddr*>(&sa), &saLen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return RecvResult::WouldBlock;
            }
            // ECONNREFUSED and ECONNRESET are the BSD spelling of the same stale ICMP error.
            if (errno == EINTR || errno == ECONNREFUSED || errno == ECONNRESET) {
                continue;
            }
            return RecvResult::Error;
        }
        const std::size_t got = static_cast<std::size_t>(n);
#endif
        from = FromSockaddr(sa);
        if (got > cap) {
            return RecvResult::Truncated;
        }
        std::memcpy(buf, scratch_.data(), got);
        outLen = got;
        return RecvResult::Ok;
    }
    return RecvResult::WouldBlock;
}

}  // namespace net
}  // namespace gdx
