// port/net/gdx_net_address.h: the 16-byte IP address every G-NET module shares (O5.3, dual stack).
//
// An IPv4 address is held IPv4-mapped (::ffff:a.b.c.d, RFC 4291 2.5.5.2), the form a dual-stack socket reports
// for IPv4 peers, so one value type and one comparison cover both families. Header-only, with no OS header, so
// the room and lobby codecs can carry addresses without the socket layer.

#ifndef GDX_NET_ADDRESS_H
#define GDX_NET_ADDRESS_H

#include <array>
#include <cstdint>

namespace gdx::net {

using IpBytes = std::array<uint8_t, 16>;

// a.b.c.d given as (a << 24) | (b << 16) | (c << 8) | d, held as ::ffff:a.b.c.d.
inline IpBytes MappedV4(uint32_t ip) {
    IpBytes out{};
    out[10] = 0xFF;
    out[11] = 0xFF;
    out[12] = static_cast<uint8_t>(ip >> 24);
    out[13] = static_cast<uint8_t>(ip >> 16);
    out[14] = static_cast<uint8_t>(ip >> 8);
    out[15] = static_cast<uint8_t>(ip);
    return out;
}

inline bool IsMappedV4(const IpBytes& ip) {
    for (int i = 0; i < 10; i++) {
        if (ip[i] != 0) {
            return false;
        }
    }
    return ip[10] == 0xFF && ip[11] == 0xFF;
}

// The IPv4 address of a mapped value, in the (a << 24) form; 0 for an IPv6 address.
inline uint32_t V4Of(const IpBytes& ip) {
    if (!IsMappedV4(ip)) {
        return 0;
    }
    return (static_cast<uint32_t>(ip[12]) << 24) | (static_cast<uint32_t>(ip[13]) << 16) |
           (static_cast<uint32_t>(ip[14]) << 8) | static_cast<uint32_t>(ip[15]);
}

inline bool IsZeroIp(const IpBytes& ip) {
    for (uint8_t b : ip) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

// Whether a member may send to the address: a listed endpoint, or a successor a handover names (decision 60).
// Refused: port 0, unspecified, this network (0/8), multicast, reserved, broadcast, link-local, and loopback
// unless allowLoopback (a local test directory). Private ranges stay: a host on the same network lists them.
inline bool DialableIp(const IpBytes& ip, uint16_t port, bool allowLoopback) {
    if (port == 0) {
        return false;
    }
    if (IsMappedV4(ip)) {
        const uint32_t v4 = V4Of(ip);
        const uint32_t top = v4 >> 24;
        if (top == 0 || top >= 224) { // this network, multicast, reserved and broadcast
            return false;
        }
        if ((v4 >> 16) == 0xA9FEu) { // 169.254.0.0/16, link-local
            return false;
        }
        return top != 127 || allowLoopback;
    }
    if (IsZeroIp(ip)) {
        return false;
    }
    IpBytes loopback{};
    loopback[15] = 1;
    if (ip == loopback) {
        return allowLoopback;
    }
    if (ip[0] == 0xFF || (ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80)) { // multicast, fe80::/10 link-local
        return false;
    }
    return true;
}

} // namespace gdx::net

#endif // GDX_NET_ADDRESS_H
