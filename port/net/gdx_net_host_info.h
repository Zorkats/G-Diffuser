// port/net/gdx_net_host_info.h: what the operating system knows about this host's network (G-NET O5.6 to O5.8).
// Copyright (c) Zorkats
//
// A host lists its room with the addresses others can reach it on. These calls gather the inputs: the STUN servers'
// addresses, this host's own addresses (a global IPv6 one is reachable as it is), the default gateway, which is
// where a NAT-PMP or PCP request goes, and a UPnP port mapping (decision 58, through miniupnpc).
//
// Not headless and blocking: a name lookup can take seconds. The caller runs these on a helper thread, never on the
// game thread or the lobby worker's tick.

#pragma once

#include "gdx_net_socket.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gdx::net::host_info {

// "name:port" or "name" with fallbackPort. Every address the name resolves to, IPv4 and IPv6, in the resolver's
// order, without repeats. Empty when the lookup fails.
std::vector<NetAddress> Resolve(const std::string& hostAndPort, uint16_t fallbackPort);

// Every unicast address of every interface that is up, loopback excluded, through PreferTemporary. The ports are 0.
std::vector<NetAddress> LocalAddresses();

// ---- IPv6 address privacy (decision 59) ----

// One local address and what the system says about it. Windows: SuffixOrigin random and DadState deprecated; Linux:
// IFA_F_TEMPORARY and IFA_F_DEPRECATED from /proc/net/if_inet6. Elsewhere both stay false.
struct LocalAddress {
    NetAddress address;
    bool temporary = false;  // an RFC 8981 temporary IPv6 address, replaced every day or so
    bool deprecated = false; // past its preferred lifetime: kept for old connections only
};

// Linux's /proc/net/if_inet6, one address per line: "<32 hex digits> <ifindex> <prefix length> <scope> <flags>
// <name>", the flags in hex (IFA_F_TEMPORARY 0x01, IFA_F_DEPRECATED 0x20). Malformed lines are skipped. Pure, so the
// tests read sample lines with it.
std::vector<LocalAddress> ParseIfInet6(const std::string& text);

// The rule LocalAddresses applies: deprecated IPv6 addresses go, and when a temporary global IPv6 address exists, the
// stable global ones go too, so a room lists an address that changes rather than one that follows the machine. In
// order, without repeats.
std::vector<NetAddress> PreferTemporary(const std::vector<LocalAddress>& all);

// The address as the log writes it: a public IPv4 address cut to its /24 and a global IPv6 address to its /48, the
// cut named after it; private, shared, loopback and link-local addresses whole. Logs are shared in bug reports.
std::string LogText(const NetAddress& address);

// The IPv4 default gateway. false where the system does not say (macOS, and Android without /proc/net/route).
bool DefaultGateway(NetAddress& gateway);

// The local address the system would send from to reach peer, found by connecting a UDP socket, which sends
// nothing. The port is 0. false when there is no route.
bool LocalAddressToward(const NetAddress& peer, NetAddress& local);

// ---- UPnP IGD port mapping (decision 58) ----

constexpr int kUpnpDiscoverMs = 2000;
constexpr int kUpnpPortTries = 4; // the room's port, then the next three when another host on the network holds it

struct UpnpMapping {
    NetAddress external;        // the router's public IPv4 address, with the external port
    uint16_t internalPort = 0;
    uint32_t leaseSeconds = 0;  // what the router took: 0 when it keeps only permanent mappings (UPnP error 725)
    std::string controlUrl;
    std::string serviceType;
    std::string internalClient; // this host's address on the router's network, dotted
};

// Finds a connected Internet Gateway Device whose external address is public, and maps UDP to localPort on this
// host for leaseSeconds. Blocks for kUpnpDiscoverMs and the router's replies. nullopt when no router answers, it
// sits behind another NAT, it refuses, or the build has no UPnP (Android).
std::optional<UpnpMapping> UpnpMap(uint16_t localPort, uint32_t leaseSeconds);

// Adds the same mapping again, which starts its lease over. false when the router refuses. Blocks.
bool UpnpRenew(const UpnpMapping& mapping);

// Deletes the mapping. Blocks.
void UpnpUnmap(const UpnpMapping& mapping);

} // namespace gdx::net::host_info
