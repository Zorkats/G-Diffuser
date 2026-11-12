// port/net/gdx_net_host_info.cpp: see gdx_net_host_info.h.
// Copyright (c) Zorkats

#include "gdx_net_host_info.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef GDX_HAVE_MINIUPNPC
#include "miniupnpc.h"
#include "upnpcommands.h"
#endif

namespace gdx::net::host_info {

namespace {

#ifdef _WIN32
// Winsock is started once for the process and never stopped, like the lobby socket's (gdx_net_socket.cpp).
bool WinsockUp() {
    static const bool up = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return up;
}
using SocketHandle = SOCKET;
const SocketHandle kNoSocket = INVALID_SOCKET;
void CloseHandle_(SocketHandle s) {
    closesocket(s);
}
#else
bool WinsockUp() {
    return true;
}
using SocketHandle = int;
const SocketHandle kNoSocket = -1;
void CloseHandle_(SocketHandle s) {
    close(s);
}
#endif

bool FromSockaddr(const sockaddr* sa, NetAddress& out) {
    if (sa == nullptr) {
        return false;
    }
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        out = NetAddress::FromV4(ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        NetAddress a;
        std::memcpy(a.ip.data(), &in6->sin6_addr, 16);
        a.port = ntohs(in6->sin6_port);
        out = a;
        return true;
    }
    return false;
}

// The address as a sockaddr of its own family: a mapped IPv4 address as AF_INET.
socklen_t ToSockaddr(const NetAddress& address, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    if (address.IsV4()) {
        auto* in = reinterpret_cast<sockaddr_in*>(&out);
        in->sin_family = AF_INET;
        in->sin_port = htons(address.port);
        in->sin_addr.s_addr = htonl(address.Ipv4());
        return static_cast<socklen_t>(sizeof(sockaddr_in));
    }
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&out);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(address.port);
    std::memcpy(&in6->sin6_addr, address.ip.data(), 16);
    return static_cast<socklen_t>(sizeof(sockaddr_in6));
}

bool IsLoopback(const NetAddress& a) {
    if (a.IsV4()) {
        return (a.Ipv4() >> 24) == 127;
    }
    static const uint8_t kLoop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    return std::memcmp(a.ip.data(), kLoop, 16) == 0;
}

// 2000::/3, the global unicast range (RFC 4291 section 2.5.4). Wider than portmap::IsGlobalUnicastIPv6, which also
// refuses the documentation range: here a wider test only hides more.
bool IsGlobalV6(const NetAddress& a) {
    return !a.IsV4() && (a.ip[0] & 0xE0u) == 0x20u;
}

// Not one of the ranges RFC 6890 marks as not globally reachable that a host meets: this network, private, shared
// (carrier-grade NAT), loopback, link-local, multicast and reserved.
bool IsPublicV4(uint32_t v4) {
    const uint32_t a = v4 >> 24;
    const uint32_t b = (v4 >> 16) & 0xFFu;
    if (a == 0 || a == 10 || a == 127 || a >= 224) {
        return false;
    }
    if ((a == 100 && (b & 0xC0u) == 64) || (a == 169 && b == 254) || (a == 172 && (b & 0xF0u) == 16) ||
        (a == 192 && b == 168)) {
        return false;
    }
    return true;
}

constexpr unsigned kIfaTemporary = 0x01;  // IFA_F_TEMPORARY
constexpr unsigned kIfaDeprecated = 0x20; // IFA_F_DEPRECATED

bool HexByte(const char* two, uint8_t& out) {
    unsigned value = 0;
    for (int i = 0; i < 2; i++) {
        const char c = two[i];
        const unsigned digit = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                               : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                               : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                      : 16u;
        if (digit > 15) {
            return false;
        }
        value = value * 16 + digit;
    }
    out = static_cast<uint8_t>(value);
    return true;
}

void AddOnce(std::vector<NetAddress>& list, const NetAddress& a) {
    if (std::find(list.begin(), list.end(), a) == list.end()) {
        list.push_back(a);
    }
}

} // namespace

std::vector<NetAddress> Resolve(const std::string& hostAndPort, uint16_t fallbackPort) {
    std::vector<NetAddress> found;
    if (!WinsockUp() || hostAndPort.empty()) {
        return found;
    }
    std::string host = hostAndPort;
    uint16_t port = fallbackPort;
    // "name:port"; an IPv6 literal goes in brackets: "[2001:db8::1]:3478".
    const size_t colon = host.rfind(':');
    const bool bracketed = !host.empty() && host.front() == '[';
    if (colon != std::string::npos && (bracketed ? host.find(']') < colon : host.find(':') == colon)) {
        const long value = std::strtol(host.c_str() + colon + 1, nullptr, 10);
        if (value <= 0 || value > 65535) {
            return found;
        }
        port = static_cast<uint16_t>(value);
        host.resize(colon);
    }
    if (bracketed) {
        if (host.size() < 2 || host.back() != ']') {
            return found;
        }
        host = host.substr(1, host.size() - 2);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) {
        return found;
    }
    for (const addrinfo* it = list; it != nullptr; it = it->ai_next) {
        NetAddress a;
        if (FromSockaddr(it->ai_addr, a)) {
            a.port = port;
            AddOnce(found, a);
        }
    }
    freeaddrinfo(list);
    return found;
}

std::vector<NetAddress> LocalAddresses() {
    std::vector<LocalAddress> found;
#ifdef _WIN32
    if (!WinsockUp()) {
        return {};
    }
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC,
                                      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        return {};
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            LocalAddress local;
            if (FromSockaddr(unicast->Address.lpSockaddr, local.address) && !IsLoopback(local.address)) {
                local.address.port = 0;
                if (!local.address.IsV4()) {
                    // A temporary address is the one Windows gives a random suffix; the stable one, randomised
                    // or not, reports its suffix as the link's (Get-NetIPAddress shows the same two values).
                    local.temporary = unicast->SuffixOrigin == IpSuffixOriginRandom;
                    local.deprecated = unicast->DadState == IpDadStateDeprecated;
                }
                found.push_back(local);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return {};
    }
    std::vector<LocalAddress> flags;
    {
        std::ifstream in("/proc/net/if_inet6"); // missing on macOS: no flags, every address kept
        std::ostringstream text;
        text << in.rdbuf();
        flags = ParseIfInet6(text.str());
    }
    for (const ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
        if ((it->ifa_flags & IFF_UP) == 0 || (it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        LocalAddress local;
        if (FromSockaddr(it->ifa_addr, local.address) && !IsLoopback(local.address)) {
            local.address.port = 0;
            for (const LocalAddress& known : flags) {
                if (!local.address.IsV4() && known.address.ip == local.address.ip) {
                    local.temporary = known.temporary;
                    local.deprecated = known.deprecated;
                    break;
                }
            }
            found.push_back(local);
        }
    }
    freeifaddrs(list);
#endif
    return PreferTemporary(found);
}

std::vector<LocalAddress> ParseIfInet6(const std::string& text) {
    std::vector<LocalAddress> found;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string hex;
        std::string index;
        std::string prefix;
        std::string scope;
        std::string flags;
        if (!(fields >> hex >> index >> prefix >> scope >> flags) || hex.size() != 32 || flags.empty() ||
            flags.size() > 8) {
            continue;
        }
        LocalAddress local;
        bool ok = true;
        for (size_t i = 0; i < 16 && ok; i++) {
            ok = HexByte(&hex[i * 2], local.address.ip[i]);
        }
        unsigned bits = 0;
        for (size_t i = 0; i < flags.size() && ok; i++) {
            uint8_t nibble = 0;
            const char pair[2] = { '0', flags[i] };
            ok = HexByte(pair, nibble);
            bits = bits * 16 + nibble;
        }
        if (!ok || local.address.IsV4()) {
            continue;
        }
        local.temporary = (bits & kIfaTemporary) != 0;
        local.deprecated = (bits & kIfaDeprecated) != 0;
        found.push_back(local);
    }
    return found;
}

std::vector<NetAddress> PreferTemporary(const std::vector<LocalAddress>& all) {
    const bool temporary = std::any_of(all.begin(), all.end(), [](const LocalAddress& l) {
        return l.temporary && !l.deprecated && IsGlobalV6(l.address);
    });
    std::vector<NetAddress> kept;
    for (const LocalAddress& l : all) {
        if (!l.address.IsV4() && l.deprecated) {
            continue;
        }
        if (temporary && IsGlobalV6(l.address) && !l.temporary) {
            continue;
        }
        AddOnce(kept, l.address);
    }
    return kept;
}

std::string LogText(const NetAddress& address) {
    NetAddress cut = address;
    const char* mark = nullptr;
    if (address.IsV4()) {
        if (IsPublicV4(address.Ipv4())) {
            cut = NetAddress::FromV4(address.Ipv4() & 0xFFFFFF00u, address.port);
            mark = " (/24)";
        }
    } else if (IsGlobalV6(address)) {
        std::fill(cut.ip.begin() + 6, cut.ip.end(), uint8_t{ 0 });
        mark = " (/48)";
    }
    std::string text = FormatAddress(cut);
    if (mark != nullptr) {
        text += mark;
    }
    return text;
}

bool DefaultGateway(NetAddress& gateway) {
#ifdef _WIN32
    if (!WinsockUp()) {
        return false;
    }
    // The route the system would take to a public address is the default route; its next hop is the gateway.
    MIB_IPFORWARDROW row{};
    const DWORD publicAddress = htonl(0x01010101u); // 1.1.1.1: any public address works, nothing is sent
    if (GetBestRoute(publicAddress, 0, &row) != NO_ERROR || row.dwForwardNextHop == 0) {
        return false;
    }
    gateway = NetAddress::FromV4(ntohl(row.dwForwardNextHop), 0);
    return true;
#elif defined(__linux__)
    // Destination 00000000 is the default route; the gateway column is the address in host byte order as hex.
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line); // the header
    while (std::getline(routes, line)) {
        char name[64] = {};
        unsigned long destination = 0;
        unsigned long next = 0;
        unsigned int flags = 0;
        if (std::sscanf(line.c_str(), "%63s %lx %lx %X", name, &destination, &next, &flags) != 4) {
            continue;
        }
        if (destination == 0 && next != 0 && (flags & 0x2u) != 0) { // RTF_GATEWAY
            gateway = NetAddress::FromV4(ntohl(static_cast<uint32_t>(next)), 0);
            return true;
        }
    }
    return false;
#else
    (void)gateway;
    return false;
#endif
}

bool LocalAddressToward(const NetAddress& peer, NetAddress& local) {
    if (!WinsockUp()) {
        return false;
    }
    sockaddr_storage to{};
    NetAddress target = peer;
    if (target.port == 0) {
        target.port = 9; // connect needs a port; a UDP connect sends nothing
    }
    const socklen_t toLength = ToSockaddr(target, to);
    const SocketHandle s = socket(to.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        return false;
    }
    bool ok = false;
    if (connect(s, reinterpret_cast<const sockaddr*>(&to), toLength) == 0) {
        sockaddr_storage mine{};
        socklen_t length = static_cast<socklen_t>(sizeof(mine));
        NetAddress a;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&mine), &length) == 0 &&
            FromSockaddr(reinterpret_cast<const sockaddr*>(&mine), a)) {
            a.port = 0;
            local = a;
            ok = true;
        }
    }
    CloseHandle_(s);
    return ok;
}

#ifdef GDX_HAVE_MINIUPNPC

namespace {

constexpr int kUpnpConflict = 718;       // ConflictInMappingEntry: another host holds the external port
constexpr int kUpnpPermanentOnly = 725;  // OnlyPermanentLeasesSupported
const char kUpnpDescription[] = "G-Diffuser G-NET";

int AddUpnp(const UpnpMapping& m) {
    char external[8];
    char internal[8];
    char lease[16];
    std::snprintf(external, sizeof external, "%u", static_cast<unsigned>(m.external.port));
    std::snprintf(internal, sizeof internal, "%u", static_cast<unsigned>(m.internalPort));
    std::snprintf(lease, sizeof lease, "%u", static_cast<unsigned>(m.leaseSeconds));
    return UPNP_AddPortMapping(m.controlUrl.c_str(), m.serviceType.c_str(), external, internal,
                               m.internalClient.c_str(), kUpnpDescription, "UDP", nullptr, lease);
}

} // namespace

std::optional<UpnpMapping> UpnpMap(uint16_t localPort, uint32_t leaseSeconds) {
    if (localPort == 0 || !WinsockUp()) {
        return std::nullopt;
    }
    // upnpDiscover's own list, searched for all at once: upnpDiscover sends one type at a time and waits the delay
    // after each, four delays in all when no router answers.
    static const char* const kTypes[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
        "upnp:rootdevice",
        nullptr,
    };
    int error = 0;
    UPNPDev* devices =
        upnpDiscoverDevices(kTypes, kUpnpDiscoverMs, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error, 1);
    if (devices == nullptr) {
        return std::nullopt;
    }
    UPNPUrls urls{};
    IGDdatas data{};
    char lan[64] = {};
    char wan[64] = {};
    // UPNP_CONNECTED_IGD only: a router whose external address is private sits behind another NAT, and a mapping
    // there reaches no one outside.
    const int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof lan, wan, sizeof wan);
    freeUPNPDevlist(devices);
    std::optional<UpnpMapping> result;
    in_addr wanIp{};
    if (igd == UPNP_CONNECTED_IGD && urls.controlURL != nullptr && lan[0] != '\0' &&
        inet_pton(AF_INET, wan, &wanIp) == 1) {
        UpnpMapping m;
        m.internalPort = localPort;
        m.controlUrl = urls.controlURL;
        m.serviceType = data.first.servicetype;
        m.internalClient = lan;
        for (int i = 0; i < kUpnpPortTries && localPort + i <= 0xFFFF; ++i) {
            m.external = NetAddress::FromV4(ntohl(wanIp.s_addr), static_cast<uint16_t>(localPort + i));
            m.leaseSeconds = leaseSeconds;
            int code = AddUpnp(m);
            if (code == kUpnpPermanentOnly) {
                m.leaseSeconds = 0;
                code = AddUpnp(m);
            }
            if (code == UPNPCOMMAND_SUCCESS) {
                result = m;
                break;
            }
            if (code != kUpnpConflict) {
                break;
            }
        }
    }
    if (igd != UPNP_NO_IGD) {
        FreeUPNPUrls(&urls);
    }
    return result;
}

bool UpnpRenew(const UpnpMapping& mapping) {
    return WinsockUp() && AddUpnp(mapping) == UPNPCOMMAND_SUCCESS;
}

void UpnpUnmap(const UpnpMapping& mapping) {
    if (!WinsockUp()) {
        return;
    }
    char external[8];
    std::snprintf(external, sizeof external, "%u", static_cast<unsigned>(mapping.external.port));
    UPNP_DeletePortMapping(mapping.controlUrl.c_str(), mapping.serviceType.c_str(), external, "UDP", nullptr);
}

#else // no miniupnpc in this build

std::optional<UpnpMapping> UpnpMap(uint16_t, uint32_t) {
    return std::nullopt;
}

bool UpnpRenew(const UpnpMapping&) {
    return false;
}

void UpnpUnmap(const UpnpMapping&) {}

#endif

} // namespace gdx::net::host_info
