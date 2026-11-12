// port/net/gdx_net_gather.h: the addresses a host lists its room under, and whether others can reach it
// (G-NET O5.6 to O5.8 and O5.11).
// Copyright (c) Zorkats
//
// Run on the room's own socket, so every address learnt is the one peers will send to:
//   - STUN Binding requests to up to two IPv4 servers (plan decision 28). Each answer is a public address. Two
//     servers that see the same mapped port show an endpoint-independent NAT (decision 33).
//   - A port mapping from the default gateway: PCP, then NAT-PMP (portmap::MapClient). It is renewed at half its
//     lifetime and deleted when the room closes.
//   - Or a UPnP mapping the caller made before the gathering (decision 58). The router calls block, so the caller
//     runs them; the gatherer says when to renew and when to delete, and asks nothing of PCP or NAT-PMP then.
//   - The host's global IPv6 addresses, reachable as they are when the socket is dual-stack.
//   - An address the player advertises in place of their own, a tunnel's for instance (O5.11, ODD P3). It is
//     listed first and counts as reachable: the player says it is.
//
// Headless: no socket, no clock. The caller sends what it is handed through send, and offers every datagram that
// arrives to Receive before the room sees it. The inputs that need the operating system (name lookups, the gateway,
// the local addresses) are gathered by the caller beforehand, off the lobby worker's tick (host_info).

#pragma once

#include "gdx_net_directory.h"
#include "gdx_net_portmap.h"
#include "gdx_net_socket.h"
#include "gdx_net_stun.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace gdx::net::gather {

constexpr size_t kStunServers = 2;          // two answers are enough for the agreement check
constexpr uint32_t kGatherMs = 8000;        // PCP then NAT-PMP each run out in 3.75 s; STUN in 3 s
constexpr uint32_t kMappingSeconds = 7200;  // asked of the gateway; it may grant less
constexpr uint32_t kMinRenewMs = 60000;     // never renew more often than once a minute
constexpr uint32_t kMaxLifetimeSeconds = 86400; // a granted lifetime counts for a day at most

using Send = std::function<void(const NetAddress& to, const uint8_t* data, size_t len)>;
using Random = std::function<void(uint8_t* out, size_t n)>;

struct Inputs {
    std::vector<NetAddress> stunServers;     // resolved, in order; IPv4 ones are used, the first kStunServers distinct
    std::optional<NetAddress> gateway;       // the IPv4 default gateway, any port
    std::optional<NetAddress> localToGateway; // this host's address on the gateway's network
    std::vector<NetAddress> localAddresses;  // every local address; global IPv6 ones are listed
    uint16_t localPort = 0;                  // the room socket's port
    bool dualStack = false;                  // the room socket takes IPv6
    std::optional<NetAddress> advertise;     // the player's own address to list first (O5.11)

    // Decision 58: a UPnP mapping made before the gathering. The hooks run the blocking router calls off the tick.
    std::optional<NetAddress> upnpExternal;  // the router's public address and the mapped port
    uint32_t upnpLeaseSeconds = 0;           // what the router granted; 0: permanent, never renewed
    std::function<void()> upnpRenew;         // called at half the lease
    std::function<void()> upnpRelease;       // called once, by Release
};

// Conversions between the socket's address and the STUN and port-mapping modules' form.
stun::IpAddress IpOf(const NetAddress& address);
bool AddressOf(const stun::MappedAddress& mapped, NetAddress& out);

class Gatherer {
public:
    Gatherer(Inputs inputs, Send send, Random random);

    void Start(uint32_t now);
    // Sends what is due: STUN and mapping retries, and the mapping's renewal.
    void Update(uint32_t now);
    // true when the datagram was an answer to one of ours; the room never sees it then.
    bool Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t now);
    // Every request answered or given up, or kGatherMs gone by. The endpoints are final then; the mapping's
    // renewal goes on.
    bool Done(uint32_t now) const;
    // Removes the mapping: sends the deletion, or calls upnpRelease, once. Nothing else is sent afterwards.
    void Release(uint32_t now);

    // At most directory::kEndpointsMax: the advertised address, the UPnP mapping, the PCP or NAT-PMP mapping, the
    // STUN addresses, the global IPv6 addresses, without repeats.
    std::vector<directory::Endpoint> Endpoints() const;
    // Decision 33, or the player's word for an advertised address.
    bool Reachable() const;
    portmap::Reachability Checks() const;
    // What the page says: the public address seen, if any.
    std::optional<NetAddress> PublicAddress() const;
    bool Mapped() const { return mapped_ || (inputs_.upnpExternal.has_value() && !released_); }

private:
    struct Stun {
        NetAddress server;
        stun::BindingClient client;
    };

    void StartMapping(uint32_t now, uint32_t lifetime);

    Inputs inputs_;
    Send send_;
    Random random_;
    std::vector<Stun> stun_;
    portmap::MapClient map_;
    portmap::Nonce nonce_{};
    bool mapping_ = false;      // a mapping request runs, or ran
    bool mapped_ = false;       // a mapping was granted, and has not been released
    bool released_ = false;
    std::optional<NetAddress> mappedExternal_; // PCP's assigned address; NAT-PMP gives only the port
    uint16_t mappedPort_ = 0;
    uint32_t renewAt_ = 0;
    uint32_t upnpRenewAt_ = 0;
    uint32_t startedAt_ = 0;
    bool started_ = false;
};

} // namespace gdx::net::gather
