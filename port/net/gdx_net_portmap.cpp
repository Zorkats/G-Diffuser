// port/net/gdx_net_portmap.cpp -- NAT-PMP and PCP codecs, the retransmission schedule and the reachability rule (G-NET O5.7, O5.8).
// Copyright (c) Zorkats

#include "gdx_net_portmap.h"

#include <algorithm>
#include <utility>

namespace gdx::net::portmap {
namespace {

// RFC 6886 section 3.2 (NAT-PMP version 0).
constexpr uint8_t kNatPmpVersion = 0;
constexpr uint8_t kNatPmpReplyBit = 0x80;
constexpr uint8_t kNatPmpOpExternal = 0;
constexpr uint8_t kNatPmpOpMapUdp = 1;
constexpr size_t kNatPmpMapRequestBytes = 12;
constexpr size_t kNatPmpExternalReplyBytes = 12;
constexpr size_t kNatPmpMapReplyBytes = 16;

// RFC 6887 section 7 (PCP version 2). The common header is 24 bytes; the MAP payload is 36 more.
constexpr uint8_t kPcpVersion = 2;
constexpr uint8_t kPcpReplyBit = 0x80;
constexpr uint8_t kPcpOpMap = 1;
constexpr size_t kPcpHeaderBytes = 24;
constexpr size_t kPcpMapBytes = 60;
constexpr size_t kPcpLifetimeAt = 4;
constexpr size_t kPcpEpochAt = 8;
constexpr size_t kPcpAddressAt = 8;       // the client address in a request
constexpr size_t kPcpNonceAt = 24;
constexpr size_t kPcpProtocolAt = 36;
constexpr size_t kPcpInternalPortAt = 40;
constexpr size_t kPcpExternalPortAt = 42;
constexpr size_t kPcpExternalAddressAt = 44;
constexpr size_t kPcpSuggestedAddressAt = 44;

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
}

uint32_t ReadU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteU16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value & 0xFFu);
}

void WriteU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(value & 0xFFu);
}

bool AllZero(const uint8_t* p, size_t length) {
    return std::all_of(p, p + length, [](uint8_t b) { return b == 0; });
}

// A PCP address is 16 bytes. An IPv4 address goes in as ::ffff:a.b.c.d (RFC 4291 section 2.5.5.2), so one
// field carries either family. An address with no family is all zeros, which means no preference.
void WriteAddress16(uint8_t* out, const IpAddress& ip) {
    std::fill(out, out + 16, static_cast<uint8_t>(0));
    if (ip.family == 4) {
        out[10] = 0xFF;
        out[11] = 0xFF;
        std::copy(ip.bytes, ip.bytes + 4, out + 12);
    } else if (ip.family == 6) {
        std::copy(ip.bytes, ip.bytes + 16, out);
    }
}

// The inverse of WriteAddress16. All zeros carry no address, so the result has family 0 and the caller
// decides what that means.
IpAddress ReadAddress16(const uint8_t* in) {
    IpAddress ip;
    if (AllZero(in, 16)) {
        return ip;
    }
    const bool mapped = AllZero(in, 10) && in[10] == 0xFF && in[11] == 0xFF;
    if (mapped) {
        ip.family = 4;
        std::copy(in + 12, in + 16, ip.bytes);
    } else {
        ip.family = 6;
        std::copy(in, in + 16, ip.bytes);
    }
    return ip;
}

// The wait after try k (0-based) is kInitialRtoMs << k, so the sends fall at 0, 250, 750 and 1750 ms, and
// the request times out one more wait after the last send, at 3750 ms.
uint64_t WaitAfterTry(int tryIndex) {
    return kInitialRtoMs << tryIndex;
}

} // namespace

const char* ReplyName(Reply reply) {
    switch (reply) {
    case Reply::Ok: return "ok";
    case Reply::Truncated: return "truncated";
    case Reply::Trailing: return "trailing bytes";
    case Reply::NotReply: return "not a reply";
    case Reply::WrongVersion: return "wrong protocol version";
    case Reply::WrongOp: return "wrong opcode";
    case Reply::WrongNonce: return "nonce does not match";
    case Reply::WrongMapping: return "mapping does not match";
    case Reply::Refused: return "gateway refused";
    case Reply::BadAddress: return "granted address is all zeros";
    }
    return "unknown";
}

const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::None: return "not reachable";
    case Reason::PortMapping: return "port mapping";
    case Reason::GlobalIPv6: return "global IPv6 address";
    case Reason::StunAgreement: return "two STUN servers agree";
    }
    return "unknown";
}

// ---- NAT-PMP ----

std::vector<uint8_t> BuildNatPmpExternalRequest() {
    return std::vector<uint8_t>{ kNatPmpVersion, kNatPmpOpExternal };
}

std::vector<uint8_t> BuildNatPmpMapRequest(const MapRequest& request) {
    std::vector<uint8_t> out(kNatPmpMapRequestBytes, 0);
    out[0] = kNatPmpVersion;
    out[1] = kNatPmpOpMapUdp;
    WriteU16(out.data() + 4, request.internalPort);
    WriteU16(out.data() + 6, request.suggestedExternalPort);
    WriteU32(out.data() + 8, request.lifetimeSeconds);
    return out;
}

Reply ParseNatPmpExternal(const uint8_t* bytes, size_t length, NatPmpExternal& out) {
    if (length == 0) {
        return Reply::Truncated;
    }
    if (bytes[0] != kNatPmpVersion) {
        return Reply::WrongVersion;
    }
    if (length < kNatPmpExternalReplyBytes) {
        return Reply::Truncated;
    }
    if (length > kNatPmpExternalReplyBytes) {
        return Reply::Trailing;
    }
    if (bytes[1] != static_cast<uint8_t>(kNatPmpReplyBit | kNatPmpOpExternal)) {
        return Reply::WrongOp;
    }
    const uint16_t result = ReadU16(bytes + 2);
    if (result != 0) {
        out.resultCode = result;
        return Reply::Refused;
    }
    out.epoch = ReadU32(bytes + 4);
    out.address = IpAddress{};
    out.address.family = 4;
    std::copy(bytes + 8, bytes + 12, out.address.bytes);
    out.resultCode = 0;
    return Reply::Ok;
}

Reply ParseNatPmpMapping(const uint8_t* bytes, size_t length, uint16_t internalPort, NatPmpMapping& out) {
    if (length == 0) {
        return Reply::Truncated;
    }
    if (bytes[0] != kNatPmpVersion) {
        return Reply::WrongVersion;
    }
    if (length < kNatPmpMapReplyBytes) {
        return Reply::Truncated;
    }
    if (length > kNatPmpMapReplyBytes) {
        return Reply::Trailing;
    }
    if (bytes[1] != static_cast<uint8_t>(kNatPmpReplyBit | kNatPmpOpMapUdp)) {
        return Reply::WrongOp;
    }
    // The echoed internal port is what ties this reply to our request. It is checked before the result, so a
    // refusal for some other mapping cannot end ours.
    if (ReadU16(bytes + 8) != internalPort) {
        return Reply::WrongMapping;
    }
    const uint16_t result = ReadU16(bytes + 2);
    if (result != 0) {
        out.resultCode = result;
        return Reply::Refused;
    }
    out.epoch = ReadU32(bytes + 4);
    out.externalPort = ReadU16(bytes + 10);
    out.lifetimeSeconds = ReadU32(bytes + 12);
    out.resultCode = 0;
    return Reply::Ok;
}

// ---- PCP ----

std::vector<uint8_t> BuildPcpMapRequest(const MapRequest& request, const Nonce& nonce) {
    std::vector<uint8_t> out(kPcpMapBytes, 0);
    out[0] = kPcpVersion;
    out[1] = kPcpOpMap; // R bit clear: a request
    WriteU32(out.data() + kPcpLifetimeAt, request.lifetimeSeconds);
    WriteAddress16(out.data() + kPcpAddressAt, request.client);
    std::copy(nonce.begin(), nonce.end(), out.begin() + kPcpNonceAt);
    out[kPcpProtocolAt] = kProtocolUdp;
    WriteU16(out.data() + kPcpInternalPortAt, request.internalPort);
    WriteU16(out.data() + kPcpExternalPortAt, request.suggestedExternalPort);
    WriteAddress16(out.data() + kPcpSuggestedAddressAt, request.suggestedExternal);
    return out;
}

Reply ParsePcpMapping(const uint8_t* bytes, size_t length, const Nonce& nonce, uint16_t internalPort, PcpMapping& out) {
    if (length == 0) {
        return Reply::Truncated;
    }
    // A NAT-PMP reply (version 0) to a PCP request lands here, and so does any other version.
    if (bytes[0] != kPcpVersion) {
        return Reply::WrongVersion;
    }
    if (length < kPcpHeaderBytes) {
        return Reply::Truncated;
    }
    if ((bytes[1] & kPcpReplyBit) == 0) {
        return Reply::NotReply;
    }
    if (static_cast<uint8_t>(bytes[1] & 0x7Fu) != kPcpOpMap) {
        return Reply::WrongOp;
    }
    const uint8_t result = bytes[3];
    const bool bodyPresent = length >= kPcpMapBytes;
    const bool nonceEchoed = bodyPresent && std::equal(nonce.begin(), nonce.end(), bytes + kPcpNonceAt);
    if (result != 0) {
        // An error reply is the common header alone, or the whole MAP reply with the request echoed back.
        if (length != kPcpHeaderBytes && length != kPcpMapBytes) {
            return length > kPcpMapBytes ? Reply::Trailing : Reply::Truncated;
        }
        if (bodyPresent && !nonceEchoed) {
            return Reply::WrongNonce;
        }
        out.resultCode = result;
        return Reply::Refused;
    }
    if (length < kPcpMapBytes) {
        return Reply::Truncated;
    }
    if (length > kPcpMapBytes) {
        return Reply::Trailing;
    }
    if (!nonceEchoed) {
        return Reply::WrongNonce;
    }
    if (bytes[kPcpProtocolAt] != kProtocolUdp || ReadU16(bytes + kPcpInternalPortAt) != internalPort) {
        return Reply::WrongMapping;
    }
    const IpAddress external = ReadAddress16(bytes + kPcpExternalAddressAt);
    if (external.family == 0) {
        return Reply::BadAddress;
    }
    out.epoch = ReadU32(bytes + kPcpEpochAt);
    out.lifetimeSeconds = ReadU32(bytes + kPcpLifetimeAt);
    out.external.ip = external;
    out.external.port = ReadU16(bytes + kPcpExternalPortAt);
    out.resultCode = 0;
    return Reply::Ok;
}

// ---- Retransmission ----

void Exchange::Start(std::vector<uint8_t> request, uint64_t nowMs) {
    request_ = std::move(request);
    nextSendMs_ = nowMs;
    failAtMs_ = 0;
    tries_ = 0;
    waiting_ = true;
    timedOut_ = false;
}

std::optional<std::vector<uint8_t>> Exchange::Pump(uint64_t nowMs) {
    if (!waiting_) {
        return std::nullopt;
    }
    if (tries_ < kMaxTries) {
        if (nowMs < nextSendMs_) {
            return std::nullopt;
        }
        tries_++;
        nextSendMs_ = nowMs + WaitAfterTry(tries_ - 1);
        if (tries_ == kMaxTries) {
            failAtMs_ = nextSendMs_;
        }
        return request_;
    }
    if (nowMs >= failAtMs_) {
        waiting_ = false;
        timedOut_ = true;
    }
    return std::nullopt;
}

// ---- The client ----

void MapClient::Start(uint64_t nowMs, const MapRequest& request, const Nonce& nonce) {
    request_ = request;
    nonce_ = nonce;
    state_ = State::Waiting;
    via_ = Via::Pcp;
    external_ = MappedAddress{};
    lifetime_ = 0;
    resultCode_ = 0;
    exchange_.Start(BuildPcpMapRequest(request_, nonce_), nowMs);
}

void MapClient::StartNatPmp(uint64_t nowMs) {
    via_ = Via::NatPmp;
    exchange_.Start(BuildNatPmpMapRequest(request_), nowMs);
}

void MapClient::Succeed(const MappedAddress& external, uint32_t lifetime) {
    external_ = external;
    lifetime_ = lifetime;
    state_ = State::Mapped;
    exchange_.Stop();
}

void MapClient::Fail(uint16_t code) {
    resultCode_ = code;
    state_ = State::Failed;
    exchange_.Stop();
}

std::optional<std::vector<uint8_t>> MapClient::Pump(uint64_t nowMs) {
    if (state_ != State::Waiting) {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> due = exchange_.Pump(nowMs);
    if (exchange_.timedOut()) {
        if (via_ == Via::Pcp) {
            // A gateway that does not answer PCP may still speak NAT-PMP.
            StartNatPmp(nowMs);
            return exchange_.Pump(nowMs);
        }
        state_ = State::Failed;
        return std::nullopt;
    }
    return due;
}

bool MapClient::OnDatagram(const uint8_t* bytes, size_t length, uint64_t nowMs) {
    if (state_ != State::Waiting) {
        return false;
    }
    if (via_ == Via::Pcp) {
        PcpMapping reply;
        const Reply result = ParsePcpMapping(bytes, length, nonce_, request_.internalPort, reply);
        if (result == Reply::Ok) {
            Succeed(reply.external, reply.lifetimeSeconds);
            return true;
        }
        // Version 0 is NAT-PMP answering; UNSUPP_VERSION is PCP's own way of saying the gateway is not PCP.
        const bool natPmpReply = result == Reply::WrongVersion && bytes[0] == 0;
        const bool unsupported = natPmpReply || (result == Reply::Refused && reply.resultCode == kPcpUnsuppVersion);
        if (unsupported) {
            StartNatPmp(nowMs);
            return false;
        }
        if (result == Reply::Refused) {
            Fail(reply.resultCode);
        } else if (result == Reply::BadAddress) {
            Fail(0);
        }
        return false;
    }
    NatPmpMapping reply;
    const Reply result = ParseNatPmpMapping(bytes, length, request_.internalPort, reply);
    if (result == Reply::Ok) {
        MappedAddress external;
        external.port = reply.externalPort;
        Succeed(external, reply.lifetimeSeconds);
        return true;
    }
    if (result == Reply::Refused) {
        Fail(reply.resultCode);
    }
    return false;
}

// ---- Reachability ----

bool IsGlobalUnicastIPv6(const uint8_t* b) {
    // RFC 4291 section 2.5.4 and the IANA registry: global unicast is 2000::/3.
    if ((b[0] & 0xE0u) != 0x20u) {
        return false;
    }
    // RFC 4193: unique local, fc00::/7. Already outside 2000::/3; refused here too so that a later widening of
    // the global range above cannot admit it.
    if ((b[0] & 0xFEu) == 0xFCu) {
        return false;
    }
    // RFC 4291 section 2.5.6: link-local, fe80::/10.
    if (b[0] == 0xFE && (b[1] & 0xC0u) == 0x80u) {
        return false;
    }
    // RFC 3849: 2001:db8::/32 is for documentation and is never routed.
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) {
        return false;
    }
    // RFC 4380 Teredo, 2001::/32, and RFC 3056 6to4, 2002::/16: global by prefix, but tunnelled through an IPv4
    // relay that a peer's direct connection often cannot reach, so they are no evidence of a reachable host.
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00) {
        return false;
    }
    if (b[0] == 0x20 && b[1] == 0x02) {
        return false;
    }
    return true;
}

Reachability CheckReachability(const Evidence& evidence) {
    Reachability result;
    result.portMapping = evidence.portMapped;
    for (const IpAddress& ip : evidence.localAddresses) {
        if (ip.family == 6 && IsGlobalUnicastIPv6(ip.bytes)) {
            result.globalIPv6 = true;
            break;
        }
    }
    // Two entries must be two different servers' answers for the same local port, and the servers must have
    // seen the same mapped port. The caller keeps one entry per server.
    for (size_t i = 0; i < evidence.stun.size() && !result.stunAgreement; i++) {
        for (size_t j = i + 1; j < evidence.stun.size(); j++) {
            if (evidence.stun[i].localPort == evidence.stun[j].localPort &&
                evidence.stun[i].mappedPort == evidence.stun[j].mappedPort) {
                result.stunAgreement = true;
                break;
            }
        }
    }
    if (result.portMapping) {
        result.reason = Reason::PortMapping;
    } else if (result.globalIPv6) {
        result.reason = Reason::GlobalIPv6;
    } else if (result.stunAgreement) {
        result.reason = Reason::StunAgreement;
    }
    return result;
}

} // namespace gdx::net::portmap
