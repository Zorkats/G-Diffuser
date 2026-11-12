// port/net/gdx_net_gather.cpp: see gdx_net_gather.h.
// Copyright (c) Zorkats

#include "gdx_net_gather.h"

#include "gdx_net_listing.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace gdx::net::gather {

stun::IpAddress IpOf(const NetAddress& address) {
    stun::IpAddress ip;
    if (address.IsV4()) {
        const uint32_t v4 = address.Ipv4();
        ip.family = 4;
        ip.bytes[0] = static_cast<uint8_t>(v4 >> 24);
        ip.bytes[1] = static_cast<uint8_t>(v4 >> 16);
        ip.bytes[2] = static_cast<uint8_t>(v4 >> 8);
        ip.bytes[3] = static_cast<uint8_t>(v4);
    } else {
        ip.family = 6;
        std::memcpy(ip.bytes, address.ip.data(), 16);
    }
    return ip;
}

bool AddressOf(const stun::MappedAddress& mapped, NetAddress& out) {
    if (mapped.ip.family == 4) {
        const uint8_t* b = mapped.ip.bytes;
        out = NetAddress::FromV4((uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3],
                                 mapped.port);
        return true;
    }
    if (mapped.ip.family == 6) {
        NetAddress a;
        std::memcpy(a.ip.data(), mapped.ip.bytes, 16);
        a.port = mapped.port;
        out = a;
        return true;
    }
    return false;
}

namespace {

// Half a granted lifetime, a minute at least. A day at most counts: a reply from the gateway is unauthenticated, and a
// lifetime near 2^32 s would wrap the deadline.
uint32_t RenewDelayMs(uint32_t lifetimeSeconds) {
    return (std::max)(kMinRenewMs, (std::min)(lifetimeSeconds, kMaxLifetimeSeconds) * 500u);
}

} // namespace

Gatherer::Gatherer(Inputs inputs, Send send, Random random)
    : inputs_(std::move(inputs)), send_(std::move(send)), random_(std::move(random)) {
    for (const NetAddress& server : inputs_.stunServers) {
        if (stun_.size() >= kStunServers) {
            break;
        }
        const bool repeat = std::any_of(stun_.begin(), stun_.end(), [&](const Stun& s) { return s.server == server; });
        if (server.IsV4() && server.port != 0 && !repeat) {
            stun_.push_back(Stun{ server, stun::BindingClient() });
        }
    }
}

void Gatherer::Start(uint32_t now) {
    started_ = true;
    startedAt_ = now;
    for (Stun& s : stun_) {
        stun::TransactionId txid{};
        random_(txid.data(), txid.size());
        s.client.Start(now, txid);
    }
    random_(nonce_.data(), nonce_.size()); // one nonce for the mapping's life: renewal and deletion name it
    if (inputs_.upnpExternal) {
        upnpRenewAt_ = now + RenewDelayMs(inputs_.upnpLeaseSeconds); // the router holds the port already
    } else if (inputs_.gateway && inputs_.localToGateway && inputs_.localPort != 0) {
        StartMapping(now, kMappingSeconds);
    }
    Update(now);
}

void Gatherer::StartMapping(uint32_t now, uint32_t lifetime) {
    portmap::MapRequest request;
    request.client = IpOf(*inputs_.localToGateway);
    request.internalPort = inputs_.localPort;
    request.suggestedExternalPort = mappedPort_ != 0 ? mappedPort_ : inputs_.localPort;
    request.lifetimeSeconds = lifetime;
    map_ = portmap::MapClient();
    map_.Start(now, request, nonce_);
    mapping_ = true;
}

void Gatherer::Update(uint32_t now) {
    if (!started_ || released_) {
        return;
    }
    for (Stun& s : stun_) {
        if (std::optional<std::vector<uint8_t>> request = s.client.Pump(now)) {
            send_(s.server, request->data(), request->size());
        }
    }
    if (inputs_.upnpExternal && inputs_.upnpLeaseSeconds != 0 && static_cast<int32_t>(now - upnpRenewAt_) >= 0) {
        upnpRenewAt_ = now + RenewDelayMs(inputs_.upnpLeaseSeconds);
        if (inputs_.upnpRenew) {
            inputs_.upnpRenew();
        }
    }
    if (mapping_) {
        if (mapped_ && map_.state() == portmap::MapClient::State::Mapped && static_cast<int32_t>(now - renewAt_) >= 0) {
            StartMapping(now, kMappingSeconds);
        }
        if (std::optional<std::vector<uint8_t>> request = map_.Pump(now)) {
            NetAddress to = *inputs_.gateway;
            to.port = portmap::kGatewayPort;
            send_(to, request->data(), request->size());
        }
    }
}

bool Gatherer::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t now) {
    if (!started_) {
        return false;
    }
    for (Stun& s : stun_) {
        if (from == s.server) {
            s.client.OnDatagram(data, len, now);
            return true; // a STUN server is never a room peer
        }
    }
    if (mapping_ && inputs_.gateway && from.port == portmap::kGatewayPort && from.ip == inputs_.gateway->ip) {
        if (map_.OnDatagram(data, len, now) && map_.state() == portmap::MapClient::State::Mapped && !released_) {
            mapped_ = true;
            mappedPort_ = map_.external().port;
            NetAddress external;
            if (AddressOf(map_.external(), external)) {
                mappedExternal_ = external;
            }
            renewAt_ = now + RenewDelayMs(map_.lifetimeSeconds());
        }
        return true;
    }
    return false;
}

bool Gatherer::Done(uint32_t now) const {
    if (!started_) {
        return false;
    }
    if (static_cast<uint32_t>(now - startedAt_) >= kGatherMs) {
        return true;
    }
    for (const Stun& s : stun_) {
        if (s.client.state() == stun::BindingClient::State::Waiting) {
            return false;
        }
    }
    return !mapping_ || mapped_ || map_.state() != portmap::MapClient::State::Waiting;
}

void Gatherer::Release(uint32_t now) {
    if (released_) {
        return;
    }
    released_ = true;
    if (inputs_.upnpExternal && inputs_.upnpRelease) {
        inputs_.upnpRelease();
    }
    if (!mapped_ || !inputs_.gateway || !inputs_.localToGateway) {
        return;
    }
    mapped_ = false;
    portmap::MapRequest request;
    request.client = IpOf(*inputs_.localToGateway);
    request.internalPort = inputs_.localPort;
    request.lifetimeSeconds = 0; // a lifetime of 0 deletes the mapping under either protocol
    const std::vector<uint8_t> bytes = map_.via() == portmap::MapClient::Via::NatPmp
                                           ? portmap::BuildNatPmpMapRequest(request)
                                           : portmap::BuildPcpMapRequest(request, nonce_);
    NetAddress to = *inputs_.gateway;
    to.port = portmap::kGatewayPort;
    send_(to, bytes.data(), bytes.size());
    (void)now;
}

std::optional<NetAddress> Gatherer::PublicAddress() const {
    for (const Stun& s : stun_) {
        NetAddress a;
        if (s.client.state() == stun::BindingClient::State::Done && AddressOf(s.client.address(), a)) {
            return a;
        }
    }
    return std::nullopt;
}

std::vector<directory::Endpoint> Gatherer::Endpoints() const {
    std::vector<directory::Endpoint> list;
    const auto add = [&list](const NetAddress& address, directory::How how) {
        if (list.size() >= directory::kEndpointsMax || address.port == 0) {
            return;
        }
        const directory::Endpoint endpoint = listing::EndpointOf(address, how);
        const bool valid = endpoint.family == directory::Family::V4 ? directory::IsIpv4(endpoint.addr)
                                                                    : directory::IsIpv6(endpoint.addr);
        const bool repeat = std::any_of(list.begin(), list.end(), [&](const directory::Endpoint& e) {
            return e.family == endpoint.family && e.addr == endpoint.addr && e.port == endpoint.port;
        });
        if (valid && !repeat) {
            list.push_back(endpoint);
        }
    };
    if (inputs_.advertise) {
        add(*inputs_.advertise, directory::How::Manual);
    }
    if (inputs_.upnpExternal && !released_) {
        add(*inputs_.upnpExternal, directory::How::Upnp);
    }
    if (mapped_) {
        // NAT-PMP grants a port only; the public address is the one STUN saw.
        std::optional<NetAddress> external = mappedExternal_ ? mappedExternal_ : PublicAddress();
        if (external) {
            external->port = mappedPort_;
            add(*external, map_.via() == portmap::MapClient::Via::Pcp ? directory::How::Pcp : directory::How::Natpmp);
        }
    }
    for (const Stun& s : stun_) {
        NetAddress a;
        if (s.client.state() == stun::BindingClient::State::Done && AddressOf(s.client.address(), a)) {
            add(a, directory::How::Stun);
        }
    }
    if (inputs_.dualStack) {
        for (const NetAddress& local : inputs_.localAddresses) {
            if (!local.IsV4() && portmap::IsGlobalUnicastIPv6(local.ip.data())) {
                NetAddress a = local;
                a.port = inputs_.localPort;
                add(a, directory::How::Ipv6);
            }
        }
    }
    return list;
}

portmap::Reachability Gatherer::Checks() const {
    portmap::Evidence evidence;
    evidence.portMapped = Mapped();
    if (inputs_.dualStack) {
        for (const NetAddress& local : inputs_.localAddresses) {
            evidence.localAddresses.push_back(IpOf(local));
        }
    }
    for (const Stun& s : stun_) {
        if (s.client.state() == stun::BindingClient::State::Done) {
            evidence.stun.push_back(portmap::StunSample{ inputs_.localPort, s.client.address().port });
        }
    }
    return portmap::CheckReachability(evidence);
}

bool Gatherer::Reachable() const {
    return inputs_.advertise.has_value() || Checks().open();
}

} // namespace gdx::net::gather
