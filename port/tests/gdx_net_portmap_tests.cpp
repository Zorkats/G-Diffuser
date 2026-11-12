// port/tests/gdx_net_portmap_tests.cpp -- NAT-PMP and PCP codecs, the retransmission schedule, the mapping client and the reachability rule (G-NET O5.7, O5.8).
// Copyright (c) Zorkats
//
// Usage: gdx_net_portmap_tests [fuzz rounds, default 3000]
//
// Every expected byte sequence here is written out field by field from RFC 6886 section 3 and RFC 6887
// section 7, so an offset mistake in the codec shows up as a byte mismatch instead of a round trip that
// agrees with itself. The schedule and the client take explicit times; nothing here opens a socket or reads
// a clock.

#include "gdx_net_portmap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
namespace portmap = gdx::net::portmap;
using portmap::IpAddress;
using portmap::Nonce;
using portmap::Reply;
using State = portmap::MapClient::State;
using Via = portmap::MapClient::Via;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void Put16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v & 0xFF);
}

void Put32(Bytes& b, size_t at, uint32_t v) {
    Put16(b, at, static_cast<uint16_t>(v >> 16));
    Put16(b, at + 2, static_cast<uint16_t>(v & 0xFFFF));
}

Bytes Cat(Bytes head, const Bytes& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

IpAddress V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    IpAddress ip;
    ip.family = 4;
    ip.bytes[0] = a;
    ip.bytes[1] = b;
    ip.bytes[2] = c;
    ip.bytes[3] = d;
    return ip;
}

// A 16-byte address: the leading bytes, zeros, and last in the final byte.
Bytes V6(std::initializer_list<uint8_t> head, uint8_t last) {
    Bytes b(head);
    b.resize(15, 0);
    b.push_back(last);
    return b;
}

IpAddress Ip6(const Bytes& sixteen) {
    IpAddress ip;
    ip.family = 6;
    std::copy(sixteen.begin(), sixteen.end(), ip.bytes);
    return ip;
}

bool SameIp(const IpAddress& a, const IpAddress& b) {
    return a.family == b.family && std::equal(a.bytes, a.bytes + 16, b.bytes);
}

// The PCP form of an IPv4 address: ::ffff:a.b.c.d (RFC 4291 section 2.5.5.2).
Bytes MappedV4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    Bytes out(16, 0);
    out[10] = 0xFF;
    out[11] = 0xFF;
    out[12] = a;
    out[13] = b;
    out[14] = c;
    out[15] = d;
    return out;
}

const Nonce kNonce = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B };
const Nonce kOtherNonce = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B };

// Sentinels: a parser must leave these fields alone unless it returns Ok (or, for resultCode, Refused).
const uint32_t kSentinelEpoch = 0xDEADBEEFu;
const uint16_t kSentinelPort = 0x1234;
const uint32_t kSentinelLifetime = 0x4242u;
const uint16_t kSentinelCode = 0xBEEF;

IpAddress SentinelIp() {
    IpAddress ip;
    ip.family = 9;
    std::fill(ip.bytes, ip.bytes + 16, static_cast<uint8_t>(0xAA));
    return ip;
}

portmap::NatPmpExternal ExternalSentinel() {
    portmap::NatPmpExternal s;
    s.epoch = kSentinelEpoch;
    s.address = SentinelIp();
    s.resultCode = kSentinelCode;
    return s;
}

portmap::NatPmpMapping MappingSentinel() {
    portmap::NatPmpMapping s;
    s.epoch = kSentinelEpoch;
    s.externalPort = kSentinelPort;
    s.lifetimeSeconds = kSentinelLifetime;
    s.resultCode = kSentinelCode;
    return s;
}

portmap::PcpMapping PcpSentinel() {
    portmap::PcpMapping s;
    s.epoch = kSentinelEpoch;
    s.lifetimeSeconds = kSentinelLifetime;
    s.external.ip = SentinelIp();
    s.external.port = kSentinelPort;
    s.resultCode = kSentinelCode;
    return s;
}

bool ExternalUnchanged(const portmap::NatPmpExternal& e) {
    return e.epoch == kSentinelEpoch && SameIp(e.address, SentinelIp());
}

bool MappingUnchanged(const portmap::NatPmpMapping& m) {
    return m.epoch == kSentinelEpoch && m.externalPort == kSentinelPort && m.lifetimeSeconds == kSentinelLifetime;
}

bool PcpUnchanged(const portmap::PcpMapping& p) {
    return p.epoch == kSentinelEpoch && p.lifetimeSeconds == kSentinelLifetime && p.external.port == kSentinelPort &&
           SameIp(p.external.ip, SentinelIp());
}

portmap::MapRequest Request(uint32_t lifetime = 3600) {
    portmap::MapRequest r;
    r.client = V4(192, 168, 1, 20);
    r.internalPort = 5000;
    r.suggestedExternalPort = 5000;
    r.lifetimeSeconds = lifetime;
    return r;
}

// ---- Frames, laid out from the RFCs ----

// RFC 6886 section 3.2.2: version, op 128, result, epoch, external address.
Bytes NatPmpExternalReply(uint16_t result, uint32_t epoch, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    Bytes r(12, 0);
    r[1] = 0x80;
    Put16(r, 2, result);
    Put32(r, 4, epoch);
    r[8] = a;
    r[9] = b;
    r[10] = c;
    r[11] = d;
    return r;
}

// RFC 6886 section 3.3: version, op 129, result, epoch, internal port, external port, lifetime.
Bytes NatPmpMappingReply(uint16_t result, uint32_t epoch, uint16_t internal, uint16_t external, uint32_t lifetime) {
    Bytes r(16, 0);
    r[1] = 0x81;
    Put16(r, 2, result);
    Put32(r, 4, epoch);
    Put16(r, 8, internal);
    Put16(r, 10, external);
    Put32(r, 12, lifetime);
    return r;
}

// The MAP payload that follows the 24-byte header (RFC 6887 section 11.1 for a request, 11.2 for a reply).
// Offsets here are relative to byte 24: nonce 0, protocol 12, internal port 16, external port 18, address 20.
Bytes MapPayload(const Nonce& nonce, uint8_t protocol, uint16_t internal, uint16_t external, const Bytes& address) {
    Bytes p(36, 0);
    std::copy(nonce.begin(), nonce.end(), p.begin());
    p[12] = protocol;
    Put16(p, 16, internal);
    Put16(p, 18, external);
    std::copy(address.begin(), address.end(), p.begin() + 20);
    return p;
}

// A PCP reply: the 24-byte header, then whatever body the caller passes (empty for the bare error form).
Bytes PcpReply(uint8_t version, uint8_t result, uint32_t lifetime, uint32_t epoch, const Bytes& body) {
    Bytes r(24, 0);
    r[0] = version;
    r[1] = 0x81;
    r[3] = result;
    Put32(r, 4, lifetime);
    Put32(r, 8, epoch);
    return Cat(r, body);
}

Bytes PcpGrant(const Bytes& external) {
    return PcpReply(2, 0, 3600, 1000, MapPayload(kNonce, 17, 5000, 40000, external));
}

// The request Request(3600) produces under PCP: 24-byte header, 36-byte MAP payload, no suggested address.
Bytes ExpectedPcpRequest() {
    Bytes e = {
        0x02, 0x01, 0x00, 0x00,                                                         // version 2, MAP, reserved
        0x00, 0x00, 0x0E, 0x10,                                                         // lifetime 3600
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 192, 168, 1, 20, // client ::ffff:192.168.1.20
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,         // nonce
        0x11, 0x00, 0x00, 0x00,                                                         // UDP, reserved
        0x13, 0x88,                                                                     // internal port 5000
        0x13, 0x88,                                                                     // suggested external port 5000
    };
    e.resize(60, 0); // suggested external address: all zeros, no preference
    return e;
}

// ---- Names ----

void CheckNames() {
    for (int i = 0; i <= static_cast<int>(Reply::BadAddress); i++) {
        Check(std::strcmp(portmap::ReplyName(static_cast<Reply>(i)), "unknown") != 0, "every reply has a name");
    }
    for (int i = 0; i <= static_cast<int>(portmap::Reason::StunAgreement); i++) {
        Check(std::strcmp(portmap::ReasonName(static_cast<portmap::Reason>(i)), "unknown") != 0, "every reason has a name");
    }
}

// ---- NAT-PMP requests ----

void CheckNatPmpRequests() {
    Check(portmap::BuildNatPmpExternalRequest() == Bytes({ 0x00, 0x00 }),
          "NAT-PMP external address request is version 0, op 0");

    portmap::MapRequest req = Request(3600);
    const Bytes map = portmap::BuildNatPmpMapRequest(req);
    Check(map == Bytes({ 0x00, 0x01, 0x00, 0x00, 0x13, 0x88, 0x13, 0x88, 0x00, 0x00, 0x0E, 0x10 }),
          "NAT-PMP UDP map request: internal port at 4, suggested port at 6, lifetime at 8");

    req.lifetimeSeconds = 0;
    const Bytes remove = portmap::BuildNatPmpMapRequest(req);
    Check(remove.size() == 12 && remove[1] == 0x01 &&
              std::all_of(remove.begin() + 8, remove.end(), [](uint8_t b) { return b == 0; }),
          "a NAT-PMP lifetime of 0 is a deletion with the same layout");
    Check(std::equal(remove.begin(), remove.begin() + 8, map.begin()),
          "a NAT-PMP deletion differs from the mapping only in its lifetime");
}

// ---- NAT-PMP replies ----

void CheckNatPmpReplies() {
    {
        const Bytes ok = NatPmpExternalReply(0, 256, 192, 0, 2, 1);
        portmap::NatPmpExternal out = ExternalSentinel();
        Check(portmap::ParseNatPmpExternal(ok.data(), ok.size(), out) == Reply::Ok, "NAT-PMP external address reply parses");
        Check(out.epoch == 256 && SameIp(out.address, V4(192, 0, 2, 1)) && out.resultCode == 0,
              "NAT-PMP external address reply carries the epoch and the address");
    }
    {
        const Bytes refused = NatPmpExternalReply(2, 256, 192, 0, 2, 1);
        portmap::NatPmpExternal out = ExternalSentinel();
        Check(portmap::ParseNatPmpExternal(refused.data(), refused.size(), out) == Reply::Refused && out.resultCode == 2,
              "a nonzero NAT-PMP result is Refused with its code");
        Check(ExternalUnchanged(out), "a refused NAT-PMP external reply leaves the address and epoch alone");
    }

    const Bytes good = NatPmpExternalReply(0, 256, 192, 0, 2, 1);
    Bytes wrongOp = good;
    wrongOp[1] = 0x82;
    Bytes requestOp = good;
    requestOp[1] = 0x00;
    Bytes wrongVersion = good;
    wrongVersion[0] = 1;
    Bytes trailing = good;
    trailing.push_back(0);
    struct Case {
        const char* name;
        Bytes bytes;
        Reply expect;
    };
    const std::vector<Case> cases = {
        { "empty", Bytes(), Reply::Truncated },
        { "11 bytes", Bytes(good.begin(), good.begin() + 11), Reply::Truncated },
        { "13 bytes", trailing, Reply::Trailing },
        { "reply op 2", wrongOp, Reply::WrongOp },
        { "a request, not a reply", requestOp, Reply::WrongOp },
        { "version 1", wrongVersion, Reply::WrongVersion },
    };
    for (const Case& c : cases) {
        portmap::NatPmpExternal out = ExternalSentinel();
        const Reply r = portmap::ParseNatPmpExternal(c.bytes.data(), c.bytes.size(), out);
        Check(r == c.expect && ExternalUnchanged(out),
              std::string("NAT-PMP external reply, ") + c.name + ": " + portmap::ReplyName(r));
    }

    {
        const Bytes ok = NatPmpMappingReply(0, 512, 5000, 40000, 3600);
        portmap::NatPmpMapping out = MappingSentinel();
        Check(portmap::ParseNatPmpMapping(ok.data(), ok.size(), 5000, out) == Reply::Ok,
              "NAT-PMP mapping reply parses");
        Check(out.externalPort == 40000 && out.lifetimeSeconds == 3600 && out.epoch == 512 && out.resultCode == 0,
              "NAT-PMP mapping reply carries the external port, lifetime and epoch");
    }
    {
        const Bytes refused = NatPmpMappingReply(3, 512, 5000, 0, 0);
        portmap::NatPmpMapping out = MappingSentinel();
        Check(portmap::ParseNatPmpMapping(refused.data(), refused.size(), 5000, out) == Reply::Refused && out.resultCode == 3,
              "a NAT-PMP mapping refusal carries the gateway's code");
        Check(MappingUnchanged(out), "a refused NAT-PMP mapping leaves the port, lifetime and epoch alone");
    }
    {
        // A refusal for some other internal port is not ours to act on: it is WrongMapping, not Refused.
        const Bytes other = NatPmpMappingReply(3, 512, 5001, 0, 0);
        portmap::NatPmpMapping out = MappingSentinel();
        Check(portmap::ParseNatPmpMapping(other.data(), other.size(), 5000, out) == Reply::WrongMapping,
              "a refusal for another internal port is WrongMapping");
        Check(MappingUnchanged(out) && out.resultCode == kSentinelCode, "a WrongMapping reply changes nothing");
    }

    const Bytes mapping = NatPmpMappingReply(0, 512, 5000, 40000, 3600);
    Bytes mapOp = mapping;
    mapOp[1] = 0x80;
    Bytes mapVersion = mapping;
    mapVersion[0] = 1;
    Bytes mapTrailing = mapping;
    mapTrailing.push_back(0);
    Bytes otherPort = NatPmpMappingReply(0, 512, 5001, 40000, 3600);
    const std::vector<Case> mapCases = {
        { "empty", Bytes(), Reply::Truncated },
        { "15 bytes", Bytes(mapping.begin(), mapping.begin() + 15), Reply::Truncated },
        { "17 bytes", mapTrailing, Reply::Trailing },
        { "external-address op", mapOp, Reply::WrongOp },
        { "version 1", mapVersion, Reply::WrongVersion },
        { "other internal port", otherPort, Reply::WrongMapping },
    };
    for (const Case& c : mapCases) {
        portmap::NatPmpMapping out = MappingSentinel();
        const Reply r = portmap::ParseNatPmpMapping(c.bytes.data(), c.bytes.size(), 5000, out);
        Check(r == c.expect && MappingUnchanged(out),
              std::string("NAT-PMP mapping reply, ") + c.name + ": " + portmap::ReplyName(r));
    }
}

// ---- PCP requests ----

void CheckPcpRequest() {
    const Bytes request = portmap::BuildPcpMapRequest(Request(3600), kNonce);
    Check(request.size() == 60, "a PCP MAP request is 60 bytes");
    Check(request == ExpectedPcpRequest(), "PCP MAP request matches RFC 6887 section 11.1 field by field");

    portmap::MapRequest prefer = Request(3600);
    prefer.suggestedExternalPort = 6000;
    prefer.suggestedExternal = V4(198, 51, 100, 9);
    const Bytes wish = portmap::BuildPcpMapRequest(prefer, kNonce);
    Check(wish[42] == 0x17 && wish[43] == 0x70, "the suggested external port sits at offset 42");
    Check(std::equal(wish.begin() + 44, wish.end(), MappedV4(198, 51, 100, 9).begin()),
          "a suggested IPv4 address is written in mapped form at offset 44");

    portmap::MapRequest six = Request(3600);
    const Bytes addr6 = V6({ 0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x78, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, 0x77);
    six.client = Ip6(addr6);
    const Bytes sixBytes = portmap::BuildPcpMapRequest(six, kNonce);
    Check(std::equal(sixBytes.begin() + 8, sixBytes.begin() + 24, addr6.begin()),
          "an IPv6 client address fills the 16-byte field at offset 8");

    const Bytes del = portmap::BuildPcpMapRequest(Request(0), kNonce);
    Bytes expectDel = request;
    std::fill(expectDel.begin() + 4, expectDel.begin() + 8, static_cast<uint8_t>(0));
    Check(del == expectDel, "a PCP deletion is the same request with lifetime 0 and the same nonce");
}

// ---- PCP replies ----

void CheckPcpReplies() {
    {
        const Bytes grant = PcpGrant(MappedV4(203, 0, 113, 7));
        portmap::PcpMapping out = PcpSentinel();
        Check(portmap::ParsePcpMapping(grant.data(), grant.size(), kNonce, 5000, out) == Reply::Ok, "a PCP MAP grant parses");
        Check(out.epoch == 1000 && out.lifetimeSeconds == 3600 && out.external.port == 40000 &&
                  SameIp(out.external.ip, V4(203, 0, 113, 7)) && out.resultCode == 0,
              "a PCP grant carries the epoch, lifetime, and external address and port");
    }
    {
        const Bytes addr6 = V6({ 0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x78, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, 0x77);
        const Bytes grant = PcpGrant(addr6);
        portmap::PcpMapping out = PcpSentinel();
        Check(portmap::ParsePcpMapping(grant.data(), grant.size(), kNonce, 5000, out) == Reply::Ok &&
                  out.external.ip.family == 6 && SameIp(out.external.ip, Ip6(addr6)),
              "a PCP grant with an IPv6 external address keeps the 16 bytes as they are");
    }
    {
        const Bytes unsupported = PcpReply(2, 1, 0, 0, {});
        portmap::PcpMapping out = PcpSentinel();
        Check(portmap::ParsePcpMapping(unsupported.data(), unsupported.size(), kNonce, 5000, out) == Reply::Refused &&
                  out.resultCode == 1 && PcpUnchanged(out),
              "UNSUPP_VERSION as the bare 24-byte header is Refused with code 1");
    }

    const Bytes grant = PcpGrant(MappedV4(203, 0, 113, 7));
    Bytes requestBit = grant;
    requestBit[1] = 0x01;
    Bytes wrongOp = grant;
    wrongOp[1] = 0x82;
    Bytes versionZero = grant;
    versionZero[0] = 0;
    const Bytes wrongNonce = PcpReply(2, 0, 3600, 1000, MapPayload(kOtherNonce, 17, 5000, 40000, MappedV4(203, 0, 113, 7)));
    const Bytes wrongPort = PcpReply(2, 0, 3600, 1000, MapPayload(kNonce, 17, 5001, 40000, MappedV4(203, 0, 113, 7)));
    const Bytes wrongProto = PcpReply(2, 0, 3600, 1000, MapPayload(kNonce, 6, 5000, 40000, MappedV4(203, 0, 113, 7)));
    const Bytes zeroAddress = PcpReply(2, 0, 3600, 1000, MapPayload(kNonce, 17, 5000, 40000, Bytes(16, 0)));
    const Bytes refusedWrongNonce = PcpReply(2, 2, 0, 0, MapPayload(kOtherNonce, 17, 5000, 0, Bytes(16, 0)));
    const Bytes refusedShort = Cat(PcpReply(2, 2, 0, 0, {}), Bytes(6, 0));
    const Bytes refusedLong = Cat(PcpReply(2, 2, 0, 0, MapPayload(kNonce, 17, 5000, 0, Bytes(16, 0))), Bytes(1, 0));
    const Bytes grantShort(grant.begin(), grant.begin() + 59);
    const Bytes grantLong = Cat(grant, Bytes(1, 0));
    const Bytes natPmpAnswer = NatPmpExternalReply(0, 1, 1, 2, 3, 4);
    struct Case {
        const char* name;
        Bytes bytes;
        Reply expect;
    };
    const std::vector<Case> cases = {
        { "empty", Bytes(), Reply::Truncated },
        { "23 bytes", Bytes(grant.begin(), grant.begin() + 23), Reply::Truncated },
        { "version 0 header", versionZero, Reply::WrongVersion },
        { "a NAT-PMP answer", natPmpAnswer, Reply::WrongVersion },
        { "a request, not a reply", requestBit, Reply::NotReply },
        { "reply op 2", wrongOp, Reply::WrongOp },
        { "wrong nonce", wrongNonce, Reply::WrongNonce },
        { "wrong internal port", wrongPort, Reply::WrongMapping },
        { "wrong protocol", wrongProto, Reply::WrongMapping },
        { "all-zero external address", zeroAddress, Reply::BadAddress },
        { "grant 59 bytes", grantShort, Reply::Truncated },
        { "grant 61 bytes", grantLong, Reply::Trailing },
        { "refusal with another nonce", refusedWrongNonce, Reply::WrongNonce },
        { "refusal 30 bytes", refusedShort, Reply::Truncated },
        { "refusal 61 bytes", refusedLong, Reply::Trailing },
    };
    for (const Case& c : cases) {
        portmap::PcpMapping out = PcpSentinel();
        const Reply r = portmap::ParsePcpMapping(c.bytes.data(), c.bytes.size(), kNonce, 5000, out);
        Check(r == c.expect && PcpUnchanged(out), std::string("PCP reply, ") + c.name + ": " + portmap::ReplyName(r));
    }

    {
        const Bytes refused = PcpReply(2, 2, 0, 0, MapPayload(kNonce, 17, 5000, 0, Bytes(16, 0)));
        portmap::PcpMapping out = PcpSentinel();
        Check(portmap::ParsePcpMapping(refused.data(), refused.size(), kNonce, 5000, out) == Reply::Refused && out.resultCode == 2,
              "NOT_AUTHORIZED with the echoed request is Refused with code 2");
        Check(PcpUnchanged(out), "a refused PCP reply leaves the grant fields alone");
    }
}

// ---- Retransmission ----

void CheckExchangeSchedule() {
    portmap::Exchange ex;
    const Bytes request = { 1, 2, 3 };
    ex.Start(request, 0);
    const auto first = ex.Pump(0);
    Check(first.has_value() && *first == request, "the first try goes out at once with the request bytes");
    Check(!ex.Pump(249).has_value(), "no resend before the 250 ms wait ends");
    Check(ex.Pump(250).has_value(), "the second try goes out at 250 ms");
    Check(!ex.Pump(749).has_value(), "no third try before 500 ms after the second");
    Check(ex.Pump(750).has_value(), "the third try goes out at 750 ms");
    Check(!ex.Pump(1749).has_value(), "no fourth try before 1000 ms after the third");
    Check(ex.Pump(1750).has_value(), "the fourth and last try goes out at 1750 ms");
    Check(ex.tries() == 4, "four tries in all");
    Check(!ex.Pump(3749).has_value() && !ex.timedOut() && ex.waiting(),
          "the last try gets its 2 s wait before the request is given up");
    Check(!ex.Pump(3750).has_value() && ex.timedOut() && !ex.waiting(), "the request times out at 3750 ms");
    Check(!ex.Pump(9999).has_value() && ex.tries() == 4, "nothing is sent after the timeout");

    portmap::Exchange stopped;
    stopped.Start(Bytes{ 9 }, 0);
    Check(stopped.Pump(0).has_value(), "a stoppable exchange sends its first try");
    stopped.Stop();
    Check(!stopped.Pump(250).has_value() && !stopped.waiting() && !stopped.timedOut(),
          "Stop ends the schedule without counting a timeout");
}

// ---- The mapping client ----

portmap::MapRequest Req() {
    return Request(3600);
}

void CheckMapClient() {
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        const auto first = c.Pump(0);
        Check(first.has_value() && *first == ExpectedPcpRequest(), "the client opens with the PCP MAP request");
        const Bytes grant = PcpGrant(MappedV4(203, 0, 113, 7));
        Check(c.OnDatagram(grant.data(), grant.size(), 100), "a PCP grant is accepted");
        Check(c.state() == State::Mapped && c.via() == Via::Pcp, "a PCP grant ends the attempt under PCP");
        Check(SameIp(c.external().ip, V4(203, 0, 113, 7)) && c.external().port == 40000 && c.lifetimeSeconds() == 3600,
              "the client reports the granted address, port and lifetime");
        Check(!c.Pump(500).has_value() && !c.OnDatagram(grant.data(), grant.size(), 500),
              "a mapped client sends and accepts nothing more");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes stale = PcpReply(2, 0, 3600, 1000, MapPayload(kOtherNonce, 17, 5000, 40000, MappedV4(203, 0, 113, 7)));
        Check(!c.OnDatagram(stale.data(), stale.size(), 100) && c.state() == State::Waiting && c.via() == Via::Pcp,
              "a grant for another nonce is ignored");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes unsupported = PcpReply(2, 1, 0, 0, {});
        Check(!c.OnDatagram(unsupported.data(), unsupported.size(), 100), "UNSUPP_VERSION is not a grant");
        Check(c.via() == Via::NatPmp && c.state() == State::Waiting, "UNSUPP_VERSION switches the attempt to NAT-PMP");
        const auto natSend = c.Pump(100);
        Check(natSend.has_value() && *natSend == portmap::BuildNatPmpMapRequest(Req()),
              "the NAT-PMP request goes out at once with the same mapping");
        const Bytes grant = NatPmpMappingReply(0, 512, 5000, 40000, 3600);
        Check(c.OnDatagram(grant.data(), grant.size(), 150), "a NAT-PMP grant is accepted");
        Check(c.state() == State::Mapped && c.via() == Via::NatPmp && c.external().port == 40000 &&
                  c.external().ip.family == 0 && c.lifetimeSeconds() == 3600,
              "a NAT-PMP grant gives the port and leaves the address to the external address request");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes natPmp = NatPmpExternalReply(0, 1, 203, 0, 113, 7);
        Check(!c.OnDatagram(natPmp.data(), natPmp.size(), 100) && c.via() == Via::NatPmp && c.state() == State::Waiting,
              "a version-0 reply is NAT-PMP answering and switches the attempt");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes refused = PcpReply(2, 2, 0, 0, MapPayload(kNonce, 17, 5000, 0, Bytes(16, 0)));
        Check(!c.OnDatagram(refused.data(), refused.size(), 100) && c.state() == State::Failed && c.resultCode() == 2 &&
                  c.via() == Via::Pcp,
              "NOT_AUTHORIZED ends the attempt with its code and does not fall back");
        Check(!c.Pump(5000).has_value(), "a failed attempt sends nothing more");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        Check(c.Pump(0).has_value(), "PCP try 1 at 0 ms");
        Check(c.Pump(250).has_value(), "PCP try 2 at 250 ms");
        Check(c.Pump(750).has_value(), "PCP try 3 at 750 ms");
        Check(c.Pump(1750).has_value(), "PCP try 4 at 1750 ms");
        Check(!c.Pump(3749).has_value() && c.via() == Via::Pcp && c.state() == State::Waiting, "PCP keeps waiting until 3750 ms");
        const auto natFirst = c.Pump(3750);
        Check(natFirst.has_value() && natFirst->size() == 12 && c.via() == Via::NatPmp,
              "PCP silence for 3750 ms starts NAT-PMP at once");
        Check(c.Pump(4000).has_value(), "NAT-PMP try 2 at 4000 ms");
        Check(c.Pump(4500).has_value(), "NAT-PMP try 3 at 4500 ms");
        Check(c.Pump(5500).has_value(), "NAT-PMP try 4 at 5500 ms");
        Check(!c.Pump(7499).has_value() && c.state() == State::Waiting, "NAT-PMP keeps waiting until 7500 ms");
        Check(!c.Pump(7500).has_value() && c.state() == State::Failed && c.resultCode() == 0,
              "silence from both protocols fails the attempt with no gateway code");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes empty = PcpGrant(Bytes(16, 0));
        Check(!c.OnDatagram(empty.data(), empty.size(), 100) && c.state() == State::Failed && c.resultCode() == 0,
              "a grant with no external address fails the attempt rather than waiting for a better one");
    }
    {
        portmap::MapClient c;
        c.Start(0, Req(), kNonce);
        (void)c.Pump(0);
        const Bytes unsupported = PcpReply(2, 1, 0, 0, {});
        (void)c.OnDatagram(unsupported.data(), unsupported.size(), 100);
        (void)c.Pump(100);
        const Bytes otherPort = NatPmpMappingReply(0, 512, 5001, 40000, 3600);
        Check(!c.OnDatagram(otherPort.data(), otherPort.size(), 120) && c.state() == State::Waiting,
              "a NAT-PMP reply for another internal port is ignored");
        const Bytes refused = NatPmpMappingReply(3, 512, 5000, 0, 0);
        Check(!c.OnDatagram(refused.data(), refused.size(), 150) && c.state() == State::Failed && c.resultCode() == 3 &&
                  c.via() == Via::NatPmp,
              "a NAT-PMP refusal ends the attempt with its code");
    }
}

// ---- Reachability ----

void CheckIPv6Table() {
    struct Row {
        const char* name;
        Bytes addr;
        bool global;
    };
    const std::vector<Row> rows = {
        { "2000::1", V6({ 0x20, 0x00 }, 0x01), true },
        { "2001:db9::1", V6({ 0x20, 0x01, 0x0D, 0xB9 }, 0x01), true },
        { "2001:db8::1 (documentation)", V6({ 0x20, 0x01, 0x0D, 0xB8 }, 0x01), false },
        { "2001:db8:ffff::ff (documentation)", V6({ 0x20, 0x01, 0x0D, 0xB8, 0xFF }, 0xFF), false },
        { "2001::1 (Teredo)", V6({ 0x20, 0x01, 0x00, 0x00 }, 0x01), false },
        { "2001:0:ffff::ff (Teredo)", V6({ 0x20, 0x01, 0x00, 0x00, 0xFF, 0xFF }, 0xFF), false },
        { "2001:1::1 (past Teredo)", V6({ 0x20, 0x01, 0x00, 0x01 }, 0x01), true },
        { "2002:c000:201::1 (6to4)", V6({ 0x20, 0x02, 0xC0, 0x00, 0x02, 0x01 }, 0x01), false },
        { "2003::1 (past 6to4)", V6({ 0x20, 0x03 }, 0x01), true },
        { "2400:cb00::1", V6({ 0x24, 0x00, 0xCB, 0x00 }, 0x01), true },
        { "3fff:ffff::ff (top of 2000::/3)", V6({ 0x3F, 0xFF, 0xFF, 0xFF }, 0xFF), true },
        { "4000::1 (outside 2000::/3)", V6({ 0x40, 0x00 }, 0x01), false },
        { "1fff::1 (outside 2000::/3)", V6({ 0x1F, 0xFF }, 0x01), false },
        { "fc00::1 (unique local)", V6({ 0xFC, 0x00 }, 0x01), false },
        { "fd12:3456:789a::1 (unique local)", V6({ 0xFD, 0x12, 0x34, 0x56, 0x78, 0x9A }, 0x01), false },
        { "fe80::1 (link-local)", V6({ 0xFE, 0x80 }, 0x01), false },
        { "febf::1 (link-local)", V6({ 0xFE, 0xBF }, 0x01), false },
        { "fec0::1 (site-local, outside 2000::/3)", V6({ 0xFE, 0xC0 }, 0x01), false },
        { "ff02::1 (multicast)", V6({ 0xFF, 0x02 }, 0x01), false },
        { "::1 (loopback)", V6({}, 0x01), false },
        { ":: (unspecified)", V6({}, 0x00), false },
        { "::ffff:192.0.2.1 (mapped IPv4)", MappedV4(192, 0, 2, 1), false },
    };
    for (const Row& row : rows) {
        Check(portmap::IsGlobalUnicastIPv6(row.addr.data()) == row.global, std::string("IsGlobalUnicastIPv6: ") + row.name);
    }
}

portmap::Evidence PortMappedOnly() {
    portmap::Evidence e;
    e.portMapped = true;
    return e;
}

portmap::Evidence Addresses(std::initializer_list<IpAddress> ips) {
    portmap::Evidence e;
    e.localAddresses.assign(ips.begin(), ips.end());
    return e;
}

portmap::Evidence Stun(std::initializer_list<portmap::StunSample> samples) {
    portmap::Evidence e;
    e.stun.assign(samples.begin(), samples.end());
    return e;
}

portmap::Evidence Combined(bool mapped) {
    portmap::Evidence e = Stun({ { 7000, 40000 }, { 7000, 40000 } });
    e.portMapped = mapped;
    e.localAddresses.push_back(Ip6(V6({ 0x24, 0x00 }, 0x01)));
    return e;
}

void CheckReachability() {
    using R = portmap::Reason;
    struct Row {
        const char* name;
        portmap::Evidence evidence;
        bool global;
        bool stun;
        R reason;
    };
    const std::vector<Row> rows = {
        { "no evidence", portmap::Evidence{}, false, false, R::None },
        { "port mapped", PortMappedOnly(), false, false, R::PortMapping },
        { "global IPv6", Addresses({ Ip6(V6({ 0x24, 0x00 }, 0x01)) }), true, false, R::GlobalIPv6 },
        { "IPv4 address only", Addresses({ V4(192, 168, 1, 20) }), false, false, R::None },
        { "link-local IPv6 only", Addresses({ Ip6(V6({ 0xFE, 0x80 }, 0x01)) }), false, false, R::None },
        { "documentation IPv6 only", Addresses({ Ip6(V6({ 0x20, 0x01, 0x0D, 0xB8 }, 0x01)) }), false, false, R::None },
        { "two servers agree", Stun({ { 7000, 40000 }, { 7000, 40000 } }), false, true, R::StunAgreement },
        { "mapped ports differ", Stun({ { 7000, 40000 }, { 7000, 40001 } }), false, false, R::None },
        { "local ports differ", Stun({ { 7000, 40000 }, { 7001, 40000 } }), false, false, R::None },
        { "one server only", Stun({ { 7000, 40000 } }), false, false, R::None },
        { "agreement among three", Stun({ { 7000, 40000 }, { 7000, 40001 }, { 7000, 40001 } }), false, true, R::StunAgreement },
        { "all three pass", Combined(true), true, true, R::PortMapping },
        { "global and STUN pass", Combined(false), true, true, R::GlobalIPv6 },
    };
    for (const Row& row : rows) {
        const portmap::Reachability r = portmap::CheckReachability(row.evidence);
        Check(r.reason == row.reason, std::string("reason: ") + row.name);
        Check(r.open() == (row.reason != R::None), std::string("open follows the reason: ") + row.name);
        Check(r.portMapping == row.evidence.portMapped && r.globalIPv6 == row.global && r.stunAgreement == row.stun,
              std::string("every check is evaluated: ") + row.name);
    }
}

// ---- Fuzz ----

uint64_t gRng = 0x51A3E0F1DCB2A7ull;

uint64_t Next() {
    gRng += 0x9E3779B97F4A7C15ull;
    uint64_t z = gRng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Mutate(Bytes& data) {
    switch (Next() % 5) {
    case 0: {
        const int flips = 1 + static_cast<int>(Next() % 3);
        for (int i = 0; i < flips && !data.empty(); i++) {
            data[static_cast<size_t>(Next() % data.size())] ^= static_cast<uint8_t>(1u << static_cast<unsigned>(Next() % 8));
        }
        break;
    }
    case 1:
        data.resize(static_cast<size_t>(Next() % (data.size() + 1)));
        break;
    case 2: {
        const size_t extra = static_cast<size_t>(Next() % 9);
        for (size_t i = 0; i < extra; i++) {
            data.push_back(static_cast<uint8_t>(Next()));
        }
        break;
    }
    case 3:
        if (data.size() >= 2) {
            const size_t at = static_cast<size_t>(Next() % (data.size() - 1));
            Put16(data, at, static_cast<uint16_t>(Next()));
        }
        break;
    default:
        data.assign(static_cast<size_t>(Next() % 80), 0);
        for (uint8_t& b : data) {
            b = static_cast<uint8_t>(Next());
        }
        break;
    }
}

void Fuzz(int rounds) {
    const std::vector<Bytes> seeds = {
        NatPmpExternalReply(0, 256, 192, 0, 2, 1),
        NatPmpMappingReply(0, 512, 5000, 40000, 3600),
        PcpGrant(MappedV4(203, 0, 113, 7)),
        PcpReply(2, 1, 0, 0, {}),
        PcpReply(2, 2, 0, 0, MapPayload(kNonce, 17, 5000, 0, Bytes(16, 0))),
    };
    int parsed = 0;
    for (int round = 0; round < rounds; round++) {
        Bytes data = seeds[static_cast<size_t>(Next() % seeds.size())];
        Mutate(data);
        // Random bytes rarely reach the nonce and port checks, so some rounds re-impose the fixed fields.
        if (data.size() >= 60 && (Next() & 1u)) {
            data[0] = 2;
            data[1] = 0x81;
            data[3] = static_cast<uint8_t>(Next() % 6);
            std::copy(kNonce.begin(), kNonce.end(), data.begin() + 24);
            data[36] = 17;
            Put16(data, 40, 5000);
        }
        if (data.size() >= 16 && (Next() & 1u)) {
            data[1] = 0x81;
            Put16(data, 8, 5000);
        }

        portmap::NatPmpExternal ext = ExternalSentinel();
        const Reply rExt = portmap::ParseNatPmpExternal(data.data(), data.size(), ext);
        if (rExt == Reply::Ok) {
            parsed++;
            Check(ext.address.family == 4, "a parsed NAT-PMP external reply is IPv4");
        } else {
            Check(ExternalUnchanged(ext), "a refused NAT-PMP external reply leaves the address and epoch alone");
        }

        portmap::NatPmpMapping map = MappingSentinel();
        const Reply rMap = portmap::ParseNatPmpMapping(data.data(), data.size(), 5000, map);
        if (rMap == Reply::Ok) {
            parsed++;
        } else {
            Check(MappingUnchanged(map), "a refused NAT-PMP mapping leaves the port, lifetime and epoch alone");
        }

        portmap::PcpMapping pcp = PcpSentinel();
        const Reply rPcp = portmap::ParsePcpMapping(data.data(), data.size(), kNonce, 5000, pcp);
        if (rPcp == Reply::Ok) {
            parsed++;
            Check(pcp.external.ip.family == 4 || pcp.external.ip.family == 6, "a parsed PCP grant carries a usable address");
        } else {
            Check(PcpUnchanged(pcp), "a refused PCP reply leaves the grant fields alone");
        }

        portmap::MapClient client;
        client.Start(0, Req(), kNonce);
        (void)client.Pump(0);
        (void)client.OnDatagram(data.data(), data.size(), 1);
        if (client.state() == State::Mapped) {
            Check(client.via() == Via::Pcp && rPcp == Reply::Ok,
                  "a client is mapped on its first datagram only by a PCP grant it parsed");
        }
    }
    std::printf("fuzz: %d rounds, %d parsed\n", rounds, parsed);
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 3000;
    CheckNames();
    CheckNatPmpRequests();
    CheckNatPmpReplies();
    CheckPcpRequest();
    CheckPcpReplies();
    CheckExchangeSchedule();
    CheckMapClient();
    CheckIPv6Table();
    CheckReachability();
    Fuzz(rounds > 0 ? rounds : 3000);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
