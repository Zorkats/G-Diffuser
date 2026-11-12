// port/net/gdx_net_portmap.h -- NAT-PMP and PCP port mappings, and the reachability rule for an Open room (G-NET O5.7, O5.8).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no random source. The lobby worker owns the socket and the clock, passes nowMs,
// sends the bytes these functions return to the gateway on kGatewayPort, and feeds back what comes back.
// UPnP is not here: miniupnpc is integrated in its own slice and reports a plain success flag to the
// reachability rule.
//
// Layouts follow RFC 6886 (NAT-PMP) and RFC 6887 (PCP). PCP is tried first and NAT-PMP is the fallback: many
// home gateways speak only NAT-PMP, and a gateway that answers PCP with a version-0 reply or UNSUPP_VERSION
// is telling the client to switch. Every decoder checks length, version, opcode and result before it trusts
// a field, and leaves its output unchanged unless it returns Ok. The one exception is resultCode: a refused
// reply sets it so the caller can say what the gateway said.

#pragma once

#include "gdx_net_stun.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gdx::net::portmap {

using stun::IpAddress;
using stun::MappedAddress;

// NAT-PMP and PCP both listen on this port on the gateway (RFC 6886 section 3.2, RFC 6887 section 19.2).
constexpr uint16_t kGatewayPort = 5351;
// Mappings here are UDP only. The number is the IANA protocol value that PCP carries; NAT-PMP's UDP opcode
// says the same thing in its own words.
constexpr uint8_t kProtocolUdp = 17;

// RFC 6886 section 3.1 retransmits at 250 ms and doubles. This slice stops after kMaxTries sends and counts
// the request as timed out one more interval after the last send. PCP's own timing (RFC 6887 section 8.1.1)
// starts at 3 s; the lobby cannot wait that long before it tries the next protocol, so PCP takes this one too.
constexpr uint64_t kInitialRtoMs = 250;
constexpr int kMaxTries = 4;

constexpr size_t kNonceBytes = 12; // RFC 6887 section 11.2
using Nonce = std::array<uint8_t, kNonceBytes>;

// A mapping as the caller asks for it. A lifetime of 0 deletes the mapping under either protocol, so a
// close sends the same request with 0. A suggested port of 0 and a family-0 suggested address mean no
// preference. NAT-PMP has no field for a suggested address and ignores it.
struct MapRequest {
    IpAddress client;                // this host's address on the gateway's network, family 4 or 6
    uint16_t internalPort = 0;
    uint16_t suggestedExternalPort = 0;
    IpAddress suggestedExternal;
    uint32_t lifetimeSeconds = 0;
};

// PCP result codes this file acts on (RFC 6887 section 7.4). Every other code is reported, not retried.
constexpr uint16_t kPcpUnsuppVersion = 1;

enum class Reply {
    Ok,
    Truncated,     // shorter than the fixed size of the reply
    Trailing,      // longer than the fixed size of the reply
    NotReply,      // PCP: the R bit is clear, so this is a request
    WrongVersion,  // not the version this reply is read as; a NAT-PMP reply to a PCP request ends here
    WrongOp,       // an opcode other than the one asked
    WrongNonce,    // PCP: the nonce is not the one sent
    WrongMapping,  // the internal port or the protocol is not the one asked
    Refused,       // a nonzero result code; the code is in resultCode
    BadAddress,    // PCP: a success whose external address is all zeros, so there is nothing to connect to
};

const char* ReplyName(Reply reply);

// ---- NAT-PMP (RFC 6886) ----

// Requests: an external address request is two bytes; a mapping request is twelve, and lifetime 0 deletes.
std::vector<uint8_t> BuildNatPmpExternalRequest();
std::vector<uint8_t> BuildNatPmpMapRequest(const MapRequest& request);

// The gateway's address. resultCode is set on Refused.
struct NatPmpExternal {
    uint32_t epoch = 0;
    IpAddress address;
    uint16_t resultCode = 0;
};

// NAT-PMP's mapping reply carries the external port but not the external address, which comes from the
// external address request. resultCode is set on Refused.
struct NatPmpMapping {
    uint32_t epoch = 0;
    uint16_t externalPort = 0;
    uint32_t lifetimeSeconds = 0;
    uint16_t resultCode = 0;
};

Reply ParseNatPmpExternal(const uint8_t* bytes, size_t length, NatPmpExternal& out);
Reply ParseNatPmpMapping(const uint8_t* bytes, size_t length, uint16_t internalPort, NatPmpMapping& out);

// ---- PCP (RFC 6887), MAP opcode ----

// A MAP request: the 24-byte common header and the 36-byte MAP payload, 60 bytes in all. The nonce ties the
// reply to this request, and a deletion carries the nonce of the mapping it removes.
std::vector<uint8_t> BuildPcpMapRequest(const MapRequest& request, const Nonce& nonce);

// The gateway's assigned mapping. resultCode is set on Refused; a refused reply may be the 24-byte common
// header alone (UNSUPP_VERSION is sent that way).
struct PcpMapping {
    uint32_t epoch = 0;
    uint32_t lifetimeSeconds = 0;
    MappedAddress external;
    uint16_t resultCode = 0;
};

Reply ParsePcpMapping(const uint8_t* bytes, size_t length, const Nonce& nonce, uint16_t internalPort, PcpMapping& out);

// ---- Retransmission ----

// One request, retransmitted on the schedule above until the caller stops it or the schedule runs out. It
// holds no socket: the caller sends what Pump returns.
class Exchange {
public:
    void Start(std::vector<uint8_t> request, uint64_t nowMs);
    // The request to send now, if one is due. After the last try has had its wait, timedOut() turns true.
    std::optional<std::vector<uint8_t>> Pump(uint64_t nowMs);
    // The caller matched a reply: nothing more is sent.
    void Stop() { waiting_ = false; }
    bool waiting() const { return waiting_; }
    bool timedOut() const { return timedOut_; }
    int tries() const { return tries_; }

private:
    std::vector<uint8_t> request_;
    uint64_t nextSendMs_ = 0;
    uint64_t failAtMs_ = 0;
    int tries_ = 0;
    bool waiting_ = false;
    bool timedOut_ = false;
};

// Asks the gateway for a UDP mapping: PCP first, then NAT-PMP when the gateway answers PCP with a version-0
// reply or UNSUPP_VERSION, or does not answer PCP at all. A refusal from either protocol ends the attempt
// with its code. The client holds no socket, like the others here.
class MapClient {
public:
    enum class State { Idle, Waiting, Mapped, Failed };
    enum class Via { Pcp, NatPmp };

    void Start(uint64_t nowMs, const MapRequest& request, const Nonce& nonce);
    // The request to send now, if one is due.
    std::optional<std::vector<uint8_t>> Pump(uint64_t nowMs);
    // True when the datagram was this attempt's answer; the mapping is then known.
    bool OnDatagram(const uint8_t* bytes, size_t length, uint64_t nowMs);

    State state() const { return state_; }
    // The protocol in use, or the one that granted the mapping.
    Via via() const { return via_; }
    // Valid once Mapped. Under NAT-PMP the address is family 0: the external address is a separate request.
    const MappedAddress& external() const { return external_; }
    uint32_t lifetimeSeconds() const { return lifetime_; }
    // Set when Failed by a refusal: the code the gateway sent. Zero when the attempt timed out.
    uint16_t resultCode() const { return resultCode_; }

private:
    void StartNatPmp(uint64_t nowMs);
    void Succeed(const MappedAddress& external, uint32_t lifetime);
    void Fail(uint16_t code);

    MapRequest request_;
    Nonce nonce_{};
    Exchange exchange_;
    State state_ = State::Idle;
    Via via_ = Via::Pcp;
    MappedAddress external_;
    uint32_t lifetime_ = 0;
    uint16_t resultCode_ = 0;
};

// ---- Reachability (decision 33) ----

// Which check let an Open room pass. None means the room fails every check.
enum class Reason {
    None,
    PortMapping,  // a UPnP, NAT-PMP or PCP mapping was granted
    GlobalIPv6,   // the host has a global unicast IPv6 address
    StunAgreement // two STUN servers saw the same mapped port for the same local port
};

const char* ReasonName(Reason reason);

// One STUN answer: the port the socket left from and the port the server saw. One entry per server.
struct StunSample {
    uint16_t localPort = 0;
    uint16_t mappedPort = 0;
};

struct Evidence {
    bool portMapped = false;
    std::vector<IpAddress> localAddresses; // every address the host has; only family-6 ones count
    std::vector<StunSample> stun;
};

// Every check is evaluated, so the page can say which ones failed. reason is the first check in the order of
// Reason that passed, and open() is true when there is one.
struct Reachability {
    Reason reason = Reason::None;
    bool portMapping = false;
    bool globalIPv6 = false;
    bool stunAgreement = false;

    bool open() const { return reason != Reason::None; }
};

// True for an address in 2000::/3 that is not unique-local (fc00::/7), link-local (fe80::/10) or
// documentation (2001:db8::/32). The 16 bytes are in network order.
bool IsGlobalUnicastIPv6(const uint8_t* bytes);

Reachability CheckReachability(const Evidence& evidence);

} // namespace gdx::net::portmap
