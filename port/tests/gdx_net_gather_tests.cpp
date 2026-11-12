// port/tests/gdx_net_gather_tests.cpp: a host's endpoints and reachability from STUN, PCP, UPnP, IPv6 and an
// advertised address (G-NET O5.6 to O5.8, O5.11, decision 58).
// Copyright (c) Zorkats
//
// The servers and the gateway are played by the test: it reads the transaction id or nonce out of each request the
// gatherer sends, and answers with packets built here byte by byte from RFC 8489 and RFC 6887.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/gdx_net_gather.h"
#include "net/gdx_net_listing.h"

#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond)

namespace {

namespace dir = gdx::net::directory;
namespace ga = gdx::net::gather;
using gdx::net::NetAddress;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

const NetAddress kStunA = NetAddress::FromV4(0xA29FCF00u, 3478); // 162.159.207.0
const NetAddress kStunB = NetAddress::FromV4(0xA29FCC00u, 3478); // 162.159.204.0
const NetAddress kGateway = NetAddress::FromV4(0xC0A80101u, 0);  // 192.168.1.1
const NetAddress kLocal = NetAddress::FromV4(0xC0A80114u, 0);    // 192.168.1.20
const uint32_t kPublic = 0xCB007107u;                              // 203.0.113.7
constexpr uint16_t kPort = 24302;

void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = static_cast<uint8_t>(x >> 8);
    v[at + 1] = static_cast<uint8_t>(x);
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v[at + i] = static_cast<uint8_t>(x >> (24 - 8 * i));
    }
}

// A Binding Success Response to request, with XOR-MAPPED-ADDRESS ip:port (RFC 8489 sections 6 and 14.2).
std::vector<uint8_t> StunReply(const std::vector<uint8_t>& request, uint32_t ip, uint16_t port) {
    std::vector<uint8_t> v(32, 0);
    Put16(v, 0, 0x0101);
    Put16(v, 2, 12);
    Put32(v, 4, 0x2112A442u);
    std::memcpy(&v[8], &request[8], 12);
    Put16(v, 20, 0x0020);
    Put16(v, 22, 8);
    v[25] = 0x01;
    Put16(v, 26, static_cast<uint16_t>(port ^ 0x2112u));
    Put32(v, 28, ip ^ 0x2112A442u);
    return v;
}

// A PCP MAP success response to request: external ip:port for lifetime seconds (RFC 6887 sections 7.2 and 11.1).
std::vector<uint8_t> PcpReply(const std::vector<uint8_t>& request, uint32_t ip, uint16_t port, uint32_t lifetime) {
    std::vector<uint8_t> v(60, 0);
    v[0] = 2;
    v[1] = 0x81;
    Put32(v, 4, lifetime);
    Put32(v, 8, 1234);
    std::memcpy(&v[24], &request[24], 12); // the nonce
    v[36] = 17;
    v[40] = request[40];
    v[41] = request[41]; // the internal port
    Put16(v, 42, port);
    v[54] = 0xFF;
    v[55] = 0xFF;
    Put32(v, 56, ip);
    return v;
}

struct Sent {
    NetAddress to;
    std::vector<uint8_t> bytes;
};

struct Rig {
    std::vector<Sent> sent;
    uint32_t seed = 7;
    ga::Gatherer g;

    explicit Rig(ga::Inputs inputs)
        : g(std::move(inputs),
            [this](const NetAddress& to, const uint8_t* data, size_t len) {
                sent.push_back(Sent{ to, std::vector<uint8_t>(data, data + len) });
            },
            [this](uint8_t* out, size_t n) {
                for (size_t i = 0; i < n; i++) {
                    seed = seed * 1103515245u + 12345u;
                    out[i] = static_cast<uint8_t>(seed >> 16);
                }
            }) {}

    const Sent* LastTo(const NetAddress& to) const {
        for (size_t i = sent.size(); i-- > 0;) {
            if (sent[i].to == to) {
                return &sent[i];
            }
        }
        return nullptr;
    }
};

ga::Inputs BaseInputs() {
    ga::Inputs in;
    in.stunServers = { kStunA, kStunB };
    in.localPort = kPort;
    in.localAddresses = { kLocal };
    return in;
}

NetAddress GatewayPcp() {
    NetAddress to = kGateway;
    to.port = 5351;
    return to;
}

void TestStunAgreement() {
    std::printf("two STUN servers agree\n");
    Rig r(BaseInputs());
    r.g.Start(0);
    CHECK(r.sent.size() == 2 && r.sent[0].to == kStunA && r.sent[1].to == kStunB);
    CHECK(!r.g.Done(0));
    CHECK(r.g.Receive(kStunA, StunReply(r.sent[0].bytes, kPublic, 40000).data(), 32, 50));
    CHECK(!r.g.Done(50));
    CHECK(r.g.Receive(kStunB, StunReply(r.sent[1].bytes, kPublic, 40000).data(), 32, 60));
    CHECK(r.g.Done(60));
    const std::vector<dir::Endpoint> list = r.g.Endpoints();
    CHECK(list.size() == 1 && list[0].addr == "203.0.113.7" && list[0].port == 40000 && list[0].how == dir::How::Stun);
    CHECK(r.g.Reachable() && r.g.Checks().stunAgreement && r.g.Checks().reason == gdx::net::portmap::Reason::StunAgreement);
    CHECK(r.g.PublicAddress() && *r.g.PublicAddress() == NetAddress::FromV4(kPublic, 40000));
}

void TestStunDisagree() {
    std::printf("two STUN servers see different ports\n");
    Rig r(BaseInputs());
    r.g.Start(0);
    r.g.Receive(kStunA, StunReply(r.sent[0].bytes, kPublic, 40000).data(), 32, 50);
    r.g.Receive(kStunB, StunReply(r.sent[1].bytes, kPublic, 40001).data(), 32, 60);
    CHECK(r.g.Done(60));
    CHECK(r.g.Endpoints().size() == 2);
    CHECK(!r.g.Reachable() && !r.g.Checks().open());

    // One answer only: no agreement either.
    Rig one(BaseInputs());
    one.g.Start(0);
    one.g.Receive(kStunA, StunReply(one.sent[0].bytes, kPublic, kPort).data(), 32, 50);
    CHECK(!one.g.Done(50));
    for (uint32_t t = 100; t <= 4000; t += 100) {
        one.g.Update(t);
    }
    CHECK(one.g.Done(4000)); // server B gave up at 3 s
    CHECK(one.g.Endpoints().size() == 1 && !one.g.Reachable());
}

void TestServersPicked() {
    std::printf("server choice and foreign datagrams\n");
    ga::Inputs in = BaseInputs();
    NetAddress six;
    gdx::net::ParseAddress("[2606:4700::1]:3478", six);
    in.stunServers = { six, kStunA, kStunA, NetAddress::FromV4(0x01020304u, 0), kStunB, NetAddress::FromV4(0x05060708u, 3478) };
    Rig r(in);
    r.g.Start(0);
    CHECK(r.sent.size() == 2 && r.sent[0].to == kStunA && r.sent[1].to == kStunB);
    // A datagram from anyone else is the room's.
    const uint8_t junk[4] = { 1, 2, 3, 4 };
    CHECK(!r.g.Receive(NetAddress::FromV4(0x0A000002u, 24302), junk, 4, 10));
    // A server's stray datagram is never the room's, answer or not.
    CHECK(r.g.Receive(kStunA, junk, 4, 10));
    CHECK(!r.g.Done(10));

    Rig idle(BaseInputs());
    CHECK(!idle.g.Receive(kStunA, junk, 4, 0)); // not started
    CHECK(!idle.g.Done(100000));
}

void TestPcpMapping() {
    std::printf("PCP mapping, renewal and release\n");
    ga::Inputs in = BaseInputs();
    in.gateway = kGateway;
    in.localToGateway = kLocal;
    Rig r(in);
    r.g.Start(0);
    const Sent* request = r.LastTo(GatewayPcp());
    CHECK(request != nullptr && request->bytes.size() == 60 && request->bytes[0] == 2 && request->bytes[1] == 1);
    CHECK(request->bytes[40] == (kPort >> 8) && request->bytes[41] == (kPort & 0xFF));
    const std::vector<uint8_t> first = request->bytes;
    r.g.Receive(kStunA, StunReply(r.sent[0].bytes, kPublic, 40000).data(), 32, 20);
    r.g.Receive(kStunB, StunReply(r.sent[1].bytes, kPublic, 40001).data(), 32, 30);
    CHECK(!r.g.Done(30)); // the gateway has not answered
    const std::vector<uint8_t> reply = PcpReply(first, kPublic, 50000, 3600);
    CHECK(r.g.Receive(GatewayPcp(), reply.data(), reply.size(), 40));
    CHECK(r.g.Done(40) && r.g.Mapped());
    const std::vector<dir::Endpoint> list = r.g.Endpoints();
    CHECK(list.size() == 3 && list[0].how == dir::How::Pcp && list[0].port == 50000 && list[0].addr == "203.0.113.7");
    CHECK(r.g.Reachable() && r.g.Checks().reason == gdx::net::portmap::Reason::PortMapping);

    // Renewed at half the granted life, with the same nonce.
    const size_t before = r.sent.size();
    r.g.Update(40 + 1800000 - 1);
    CHECK(r.sent.size() == before);
    r.g.Update(40 + 1800000);
    request = r.LastTo(GatewayPcp());
    CHECK(r.sent.size() == before + 1 && request != nullptr && std::memcmp(&request->bytes[24], &first[24], 12) == 0);
    CHECK(request->bytes[42] == (50000 >> 8) && request->bytes[43] == (50000 & 0xFF)); // asks for the port it holds

    // Released with lifetime 0 and the same nonce; then silence.
    r.g.Release(1900000);
    request = r.LastTo(GatewayPcp());
    CHECK(request != nullptr && request->bytes[4] == 0 && request->bytes[5] == 0 && request->bytes[6] == 0 &&
          request->bytes[7] == 0);
    CHECK(std::memcmp(&request->bytes[24], &first[24], 12) == 0);
    CHECK(!r.g.Mapped());
    const size_t after = r.sent.size();
    r.g.Update(5000000);
    r.g.Release(5000001);
    CHECK(r.sent.size() == after);
}

void TestNoGatewayAnswer() {
    std::printf("the gateway never answers\n");
    ga::Inputs in = BaseInputs();
    in.gateway = kGateway;
    in.localToGateway = kLocal;
    in.stunServers.clear();
    Rig r(in);
    r.g.Start(0);
    uint32_t t = 0;
    while (!r.g.Done(t) && t < 20000) {
        t += 50;
        r.g.Update(t);
    }
    CHECK(t <= ga::kGatherMs);
    CHECK(!r.g.Mapped() && r.g.Endpoints().empty() && !r.g.Reachable());
    size_t toGateway = 0;
    for (const Sent& s : r.sent) {
        toGateway += s.to == GatewayPcp() ? 1 : 0;
    }
    CHECK(toGateway >= 2); // PCP, then NAT-PMP
    // Release without a mapping sends nothing.
    const size_t before = r.sent.size();
    r.g.Release(t);
    CHECK(r.sent.size() == before);
}

// Decision 58: a UPnP mapping made before the gathering is listed, makes the room reachable, keeps PCP and NAT-PMP
// quiet, is renewed at half its lease, and is deleted once.
void TestUpnpMapping() {
    std::printf("UPnP mapping, renewal and release\n");
    ga::Inputs in = BaseInputs();
    in.stunServers.clear();
    in.gateway = kGateway;
    in.localToGateway = kLocal;
    in.upnpExternal = NetAddress::FromV4(kPublic, 50002);
    in.upnpLeaseSeconds = 7200;
    int renewed = 0;
    int released = 0;
    in.upnpRenew = [&renewed]() { renewed++; };
    in.upnpRelease = [&released]() { released++; };
    Rig r(in);
    r.g.Start(0);
    CHECK(r.LastTo(GatewayPcp()) == nullptr); // the router holds the port already
    CHECK(r.g.Done(0) && r.g.Mapped());
    const std::vector<dir::Endpoint> list = r.g.Endpoints();
    CHECK(list.size() == 1 && list[0].how == dir::How::Upnp && list[0].port == 50002 && list[0].addr == "203.0.113.7");
    CHECK(r.g.Reachable() && r.g.Checks().reason == gdx::net::portmap::Reason::PortMapping);

    r.g.Update(3600000 - 1);
    CHECK(renewed == 0);
    r.g.Update(3600000);
    CHECK(renewed == 1);
    r.g.Update(3600001);
    CHECK(renewed == 1);
    r.g.Update(7200000);
    CHECK(renewed == 2);

    r.g.Release(7300000);
    r.g.Release(7300001);
    CHECK(released == 1 && renewed == 2);
    CHECK(!r.g.Mapped() && r.g.Endpoints().empty());
    r.g.Update(20000000);
    CHECK(renewed == 2 && r.sent.empty());
}

// A permanent UPnP mapping (lease 0, the router's error 725) is never renewed, and still deleted.
void TestUpnpPermanent() {
    std::printf("permanent UPnP mapping\n");
    ga::Inputs in = BaseInputs();
    in.upnpExternal = NetAddress::FromV4(kPublic, kPort);
    int renewed = 0;
    int released = 0;
    in.upnpRenew = [&renewed]() { renewed++; };
    in.upnpRelease = [&released]() { released++; };
    Rig r(in);
    r.g.Start(0);
    r.g.Update(100000000);
    CHECK(renewed == 0 && r.g.Reachable());
    r.g.Release(100000001);
    CHECK(released == 1);
}

void TestAdvertiseAndIpv6() {
    std::printf("an advertised address and global IPv6\n");
    ga::Inputs in = BaseInputs();
    in.stunServers.clear();
    in.advertise = NetAddress::FromV4(0x64400001u, 31000); // 100.64.0.1: a tunnel's address
    Rig r(in);
    r.g.Start(0);
    CHECK(r.g.Done(0));
    std::vector<dir::Endpoint> list = r.g.Endpoints();
    CHECK(list.size() == 1 && list[0].how == dir::How::Manual && list[0].addr == "100.64.0.1" && list[0].port == 31000);
    CHECK(r.g.Reachable() && !r.g.Checks().open()); // the player's word, not a check

    ga::Inputs v6 = BaseInputs();
    v6.stunServers.clear();
    NetAddress global;
    NetAddress linkLocal;
    gdx::net::ParseAddress("[2001:470::5]:0", global);
    gdx::net::ParseAddress("[fe80::1]:0", linkLocal);
    v6.localAddresses = { kLocal, linkLocal, global };
    v6.dualStack = true;
    Rig s(v6);
    s.g.Start(0);
    list = s.g.Endpoints();
    CHECK(list.size() == 1 && list[0].family == dir::Family::V6 && list[0].addr == "2001:470::5" && list[0].port == kPort &&
          list[0].how == dir::How::Ipv6);
    CHECK(s.g.Reachable() && s.g.Checks().reason == gdx::net::portmap::Reason::GlobalIPv6);

    v6.dualStack = false; // an IPv4-only socket cannot be reached on its IPv6 address
    Rig t(v6);
    t.g.Start(0);
    CHECK(t.g.Endpoints().empty() && !t.g.Reachable());
}

void TestEndpointCap() {
    std::printf("at most eight endpoints, no repeats\n");
    ga::Inputs in = BaseInputs();
    in.stunServers.clear();
    in.dualStack = true;
    in.advertise = NetAddress::FromV4(0x64400001u, 31000);
    for (int i = 0; i < 12; i++) {
        NetAddress a;
        const std::string text = "[2001:470::" + std::to_string(i + 1) + "]:0";
        gdx::net::ParseAddress(text.c_str(), a);
        in.localAddresses.push_back(a);
        in.localAddresses.push_back(a);
    }
    Rig r(in);
    r.g.Start(0);
    const std::vector<dir::Endpoint> list = r.g.Endpoints();
    CHECK(list.size() == dir::kEndpointsMax && list[0].how == dir::How::Manual);
    for (size_t i = 0; i < list.size(); i++) {
        for (size_t j = i + 1; j < list.size(); j++) {
            CHECK(list[i].addr != list[j].addr);
        }
    }
    dir::Draft draft;
    draft.protocol = 7;
    draft.hostKey = std::string(64, 'a');
    draft.endpoints = list;
    CHECK(dir::DraftValid(draft));
}

void TestConversions() {
    std::printf("address conversions\n");
    const gdx::net::stun::IpAddress ip = ga::IpOf(NetAddress::FromV4(0xC0A80114u, 9));
    CHECK(ip.family == 4 && ip.bytes[0] == 192 && ip.bytes[1] == 168 && ip.bytes[2] == 1 && ip.bytes[3] == 20);
    gdx::net::stun::MappedAddress mapped;
    mapped.ip = ip;
    mapped.port = 77;
    NetAddress back;
    CHECK(ga::AddressOf(mapped, back) && back == NetAddress::FromV4(0xC0A80114u, 77));
    NetAddress six;
    gdx::net::ParseAddress("[2001:db8::7]:5", six);
    mapped.ip = ga::IpOf(six);
    mapped.port = 5;
    CHECK(mapped.ip.family == 6 && ga::AddressOf(mapped, back) && back == six);
    mapped.ip.family = 0;
    CHECK(!ga::AddressOf(mapped, back));
}

} // namespace

int main() {
    TestConversions();
    TestStunAgreement();
    TestStunDisagree();
    TestServersPicked();
    TestPcpMapping();
    TestNoGatewayAnswer();
    TestUpnpMapping();
    TestUpnpPermanent();
    TestAdvertiseAndIpv6();
    TestEndpointCap();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
