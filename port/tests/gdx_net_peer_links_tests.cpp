// port/tests/gdx_net_peer_links_tests.cpp: direct member links, the GDXP datagram and punch first (G-NET decisions
// 61 to 68).
// Copyright (c) Zorkats
//
// Usage: gdx_net_peer_links_tests
// Prints one line per failed check, then "<N> checks, <M> failed". Exit 0 only when M is 0.
//
// Every test runs on a fake clock and a fake network: each copy's sends land in a list, and the test decides which
// of them arrive.

#include "gdx_net_peer_links.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using gdx::net::NetAddress;
using gdx::net::lobby::Bytes32;
using gdx::net::lobby::EncodeState;
using gdx::net::lobby::PeerEndpoint;
using gdx::net::lobby::PeerEntry;
using gdx::net::lobby::RaceState;
using gdx::net::lobby::StatePose;
using namespace gdx::net::links;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

#define CHECK(expr) Check((expr), std::string(#expr) + " (line " + std::to_string(__LINE__) + ")")

NetAddress Addr(int a, int b, int c, int d, uint16_t port) {
    return NetAddress::FromV4((static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
                                  (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d),
                              port);
}

Bytes32 Secret(uint8_t fill) {
    Bytes32 s{};
    s.fill(fill);
    return s;
}

struct Packet {
    Route route = Route::Direct;
    NetAddress from;
    NetAddress to;
    std::vector<uint8_t> bytes;
};

// One copy: its links, and every datagram they sent, stamped with the address they leave from.
struct Copy {
    uint8_t id = 0;
    NetAddress self;     // what its direct datagrams come from
    NetAddress relay;    // what its relayed datagrams come from
    std::vector<Packet> sent;
    PeerLinks links;

    Copy(uint8_t id_, NetAddress self_, NetAddress relay_ = NetAddress())
        : id(id_), self(self_), relay(relay_), links([this](Route route, const NetAddress& to, const uint8_t* d,
                                                            size_t n) {
              Packet p;
              p.route = route;
              p.from = route == Route::Relay ? relay : self;
              p.to = to;
              p.bytes.assign(d, d + n);
              sent.push_back(std::move(p));
          }) {
        links.SetLocal(id, true, false);
    }

    size_t SentTo(const NetAddress& to, uint8_t kind) const {
        size_t n = 0;
        for (const Packet& p : sent) {
            if (p.to == to && p.bytes.size() > 4 && p.bytes[4] == kind) {
                n++;
            }
        }
        return n;
    }
};

// Hands every datagram a sent that is addressed to one of b's addresses to b, then clears a's list.
size_t Deliver(Copy& a, Copy& b, uint32_t nowMs, std::vector<StatePose>& poses) {
    size_t accepted = 0;
    for (const Packet& p : a.sent) {
        const bool toB = (p.route == Route::Direct && p.to == b.self) || (p.route == Route::Relay && p.to == b.relay);
        if (toB && b.links.Receive(p.route, p.from, p.bytes.data(), p.bytes.size(), nowMs, poses)) {
            accepted++;
        }
    }
    a.sent.clear();
    return accepted;
}

PeerEntry EntryFor(const Copy& c, const Bytes32& secret, bool pub = true, bool relay = false) {
    PeerEntry e;
    e.id = c.id;
    e.secret = secret;
    if (pub) {
        e.pub = EndpointOf(c.self);
    }
    if (relay) {
        e.relay = EndpointOf(c.relay);
    }
    return e;
}

StatePose Pose(uint8_t slot, uint32_t tick) {
    StatePose p;
    p.slot = slot;
    p.tick = tick;
    p.forward = { 0.0f, 0.0f, 1.0f };
    p.up = { 0.0f, 1.0f, 0.0f };
    return p;
}

// Two copies, ids 1 and 2, each told about the other under one secret, in race 7 with slots { 1, 2 }.
struct Pair {
    Copy a{ 1, Addr(203, 0, 113, 1, 24302), Addr(198, 51, 100, 1, 40001) };
    Copy b{ 2, Addr(203, 0, 113, 2, 24302), Addr(198, 51, 100, 2, 40002) };
    Bytes32 secret = Secret(0x5A);

    Pair() {
        a.links.Apply({ EntryFor(b, secret) }, 0);
        b.links.Apply({ EntryFor(a, secret) }, 0);
        a.links.SetRace(7, { 1, 2 }, 0);
        b.links.SetRace(7, { 1, 2 }, 0);
    }

    // Both punch at nowMs and both hear the other.
    void Exchange(uint32_t nowMs) {
        std::vector<StatePose> none;
        a.links.Update(nowMs);
        b.links.Update(nowMs);
        Deliver(a, b, nowMs, none);
        Deliver(b, a, nowMs, none);
    }
};

void TestPunchOpensLink() {
    std::printf("punch first, then poses\n");
    Pair p;
    PathKind kind = PathKind::Relay;
    CHECK(!p.a.links.Open(2, 0, kind));
    // Before the link opens, poses do not go out direct.
    p.a.links.SendPoses(7, { Pose(0, 10) }, 0);
    CHECK(p.a.SentTo(p.b.self, kKindPoses) == 0);
    p.Exchange(0);
    CHECK(p.a.links.Open(2, 0, kind) && kind == PathKind::Public);
    CHECK(p.b.links.Open(1, 0, kind) && kind == PathKind::Public);

    p.a.links.SendPoses(7, { Pose(0, 11) }, 10);
    std::vector<StatePose> got;
    CHECK(Deliver(p.a, p.b, 10, got) == 1 && got.size() == 1 && got[0].tick == 11 && got[0].slot == 0);
    CHECK(p.b.links.Stats().posesReceived[static_cast<size_t>(PathKind::Public)] == 1);
    // Poses for another race, or none, are not sent.
    p.a.links.SendPoses(8, { Pose(0, 12) }, 20);
    p.a.links.SendPoses(7, {}, 20);
    CHECK(p.a.sent.empty());
}

void TestPunchRateAndWindow() {
    std::printf("punch rate, the 15 s window and the never-answered cap\n");
    Copy a{ 1, Addr(203, 0, 113, 1, 24302) };
    const NetAddress stranger = Addr(192, 0, 2, 99, 5000);
    PeerEntry e;
    e.id = 2;
    e.secret = Secret(1);
    e.pub = EndpointOf(stranger);
    a.links.Apply({ e }, 0);
    for (uint32_t t = 0; t <= 20000; t += 10) {
        a.links.Update(t);
    }
    // One punch every 250 ms from 0 up to the end of the window, none after.
    CHECK(a.SentTo(stranger, kKindPunch) == kPunchWindowMs / kPunchIntervalMs);
    // A race start opens the window again.
    a.sent.clear();
    a.links.SetRace(1, { 1, 2 }, 20000);
    for (uint32_t t = 20000; t <= 40000; t += 10) {
        a.links.Update(t);
    }
    CHECK(a.SentTo(stranger, kKindPunch) == kPunchWindowMs / kPunchIntervalMs);
    // However often the window opens, an endpoint that never answered gets kUnansweredPunchCap in all.
    size_t total = 2 * (kPunchWindowMs / kPunchIntervalMs);
    a.sent.clear();
    uint32_t t = 40000;
    for (int race = 0; race < 10; race++) {
        a.links.SetRace(static_cast<uint16_t>(2 + race), { 1, 2 }, t);
        for (uint32_t end = t + 20000; t <= end; t += 10) {
            a.links.Update(t);
        }
    }
    total += a.SentTo(stranger, kKindPunch);
    CHECK(total == kUnansweredPunchCap);
}

void TestSilenceAndKeepalive() {
    std::printf("keepalive while open, back to punching after 3 s of silence\n");
    Pair p;
    p.Exchange(0);
    PathKind kind = PathKind::Relay;
    // Open, nothing else sent: a keepalive every second, not a punch every 250 ms.
    p.a.sent.clear();
    std::vector<StatePose> none;
    for (uint32_t t = 10; t <= 2000; t += 10) {
        p.a.links.Update(t);
        p.b.links.Update(t);
        Deliver(p.a, p.b, t, none);
        Deliver(p.b, p.a, t, none);
    }
    CHECK(p.a.links.Open(2, 2000, kind));
    CHECK(p.a.links.Stats().punchesSent == 3); // the first punch, then keepalives at 1 s and 2 s
    // b goes quiet: after kSilenceMs a closes the link and punches again.
    p.a.sent.clear();
    for (uint32_t t = 2010; t <= 2000 + kSilenceMs + 1000; t += 10) {
        p.a.links.Update(t);
        p.b.links.Update(t);
        p.b.sent.clear(); // lost
    }
    CHECK(!p.a.links.Open(2, 2000 + kSilenceMs + 1000, kind));
    CHECK(p.a.SentTo(p.b.self, kKindPunch) >= 4);
    // b answers again at the next punch both owe: open again.
    p.Exchange(6250);
    CHECK(p.a.links.Open(2, 6250, kind));
}

void TestRefusals() {
    std::printf("bad MAC, wrong pair, wrong slot, old race, truncated: dropped\n");
    Pair p;
    p.Exchange(0);
    RaceState state;
    state.raceIndex = 7;
    state.poses = { Pose(0, 50) };
    std::vector<uint8_t> body = EncodeState(state);
    std::vector<StatePose> got;
    const auto receive = [&](const std::vector<uint8_t>& bytes, Route route = Route::Direct,
                             NetAddress from = NetAddress()) {
        if (from == NetAddress()) {
            from = p.a.self;
        }
        got.clear();
        return p.b.links.Receive(route, from, bytes.data(), bytes.size(), 100, got);
    };
    const std::vector<uint8_t> good = BuildDatagram(p.secret, kKindPoses, 7, 1, 2, body.data(), body.size());
    CHECK(receive(good) && got.size() == 1);
    const uint32_t refusedBefore = p.b.links.Stats().refused;

    std::vector<uint8_t> bad = good;
    bad[kHeaderBytes + 5] ^= 1;
    CHECK(!receive(bad) && got.empty()); // the body changed under the MAC
    bad = good;
    bad.back() ^= 1;
    CHECK(!receive(bad)); // the MAC itself
    CHECK(!receive(BuildDatagram(Secret(0x77), kKindPoses, 7, 1, 2, body.data(), body.size()))); // another pair's key
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 7, 1, 3, body.data(), body.size())));      // not for us
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 7, 2, 2, body.data(), body.size())));      // from ourselves
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 7, 5, 2, body.data(), body.size())));      // from nobody known
    CHECK(!receive(BuildDatagram(p.secret, 3, 7, 1, 2, body.data(), body.size())));               // unknown kind
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 8, 1, 2, body.data(), body.size())));      // header and body disagree
    CHECK(!receive(BuildDatagram(p.secret, kKindPunch, 7, 1, 2, body.data(), body.size())));      // a punch with a body
    state.poses = { Pose(1, 51) };
    body = EncodeState(state);
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 7, 1, 2, body.data(), body.size()))); // b's slot, from a
    state.poses = { Pose(0, 52), Pose(3, 52) };
    body = EncodeState(state);
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 7, 1, 2, body.data(), body.size())) && got.empty());
    for (size_t n = 0; n < good.size(); n++) {
        const std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(!receive(cut));
    }
    CHECK(!receive(good, Route::Direct, Addr(203, 0, 113, 9, 24302))); // sound, from an unlisted address
    CHECK(!receive(good, Route::Relay, p.a.self));                     // the right address by the wrong route
    CHECK(p.b.links.Stats().refused > refusedBefore);

    // A pose of an earlier race still opens the link, but gives no pose.
    state.raceIndex = 6;
    state.poses = { Pose(0, 53) };
    body = EncodeState(state);
    CHECK(receive(BuildDatagram(p.secret, kKindPoses, 6, 1, 2, body.data(), body.size())) && got.empty());
    // A member who is not on the grid (a watcher) gives no pose.
    p.b.links.SetRace(9, { 2, 4 }, 200);
    state.raceIndex = 9;
    state.poses = { Pose(0, 54) };
    body = EncodeState(state);
    CHECK(!receive(BuildDatagram(p.secret, kKindPoses, 9, 1, 2, body.data(), body.size())));
}

void TestDirectOff() {
    std::printf("direct links off: no direct punch, no direct datagram taken\n");
    Pair p;
    p.a.links.SetLocal(1, false, false);
    for (uint32_t t = 0; t <= 3000; t += 10) {
        p.a.links.Update(t);
    }
    CHECK(p.a.sent.empty());
    std::vector<StatePose> got;
    const std::vector<uint8_t> punch = BuildDatagram(p.secret, kKindPunch, 7, 2, 1, nullptr, 0);
    CHECK(!p.a.links.Receive(Route::Direct, p.b.self, punch.data(), punch.size(), 3000, got));

    // An open link closes as soon as the setting goes off.
    Pair q;
    q.Exchange(0);
    PathKind kind = PathKind::Relay;
    CHECK(q.a.links.Open(2, 0, kind));
    q.a.links.SetLocal(1, false, false);
    CHECK(!q.a.links.Open(2, 0, kind));
    q.a.sent.clear();
    q.a.links.SendPoses(7, { Pose(0, 1) }, 10);
    CHECK(q.a.sent.empty());
}

void TestRelayAndBestPath() {
    std::printf("relay endpoints, and the best open path\n");
    Copy a{ 1, Addr(203, 0, 113, 1, 24302), Addr(198, 51, 100, 1, 40001) };
    Copy b{ 2, Addr(203, 0, 113, 2, 24302), Addr(198, 51, 100, 2, 40002) };
    const Bytes32 s = Secret(9);
    // b has direct links off: the host gives a its relay address only.
    a.links.SetLocal(1, true, true);
    b.links.SetLocal(2, false, true);
    a.links.Apply({ EntryFor(b, s, false, true) }, 0);
    b.links.Apply({ EntryFor(a, s, false, true) }, 0);
    a.links.SetRace(1, { 1, 2 }, 0);
    b.links.SetRace(1, { 1, 2 }, 0);
    std::vector<StatePose> none;
    a.links.Update(0);
    b.links.Update(0);
    for (const Packet& pk : a.sent) {
        CHECK(pk.route == Route::Relay && pk.to == b.relay);
    }
    Deliver(a, b, 0, none);
    Deliver(b, a, 0, none);
    PathKind kind = PathKind::Lan;
    CHECK(a.links.Open(2, 0, kind) && kind == PathKind::Relay);
    a.links.SendPoses(1, { Pose(0, 5) }, 5);
    std::vector<StatePose> got;
    CHECK(Deliver(a, b, 5, got) == 1 && got.size() == 1);
    CHECK(b.links.Stats().posesReceived[static_cast<size_t>(PathKind::Relay)] == 1);
    // Without its own allocation a copy does not use relay endpoints.
    Copy c{ 3, Addr(203, 0, 113, 3, 24302) };
    c.links.Apply({ EntryFor(b, s, false, true) }, 0);
    c.links.Update(0);
    CHECK(c.sent.empty());

    // Same router: LAN and public both open, poses take the LAN one.
    Copy d{ 4, Addr(203, 0, 113, 50, 24302) };
    Copy e{ 5, Addr(203, 0, 113, 50, 24303) };
    const NetAddress dLan = Addr(192, 168, 1, 4, 24302);
    const NetAddress eLan = Addr(192, 168, 1, 5, 24303);
    PeerEntry toE = EntryFor(e, s);
    toE.lan = { EndpointOf(eLan) };
    PeerEntry toD = EntryFor(d, s);
    toD.lan = { EndpointOf(dLan) };
    d.links.Apply({ toE }, 0);
    e.links.Apply({ toD }, 0);
    d.links.SetRace(2, { 4, 5 }, 0);
    e.links.SetRace(2, { 4, 5 }, 0);
    d.links.Update(0);
    e.links.Update(0);
    CHECK(d.SentTo(eLan, kKindPunch) == 1 && d.SentTo(e.self, kKindPunch) == 1);
    // Hand e's punches to d from both of e's addresses, as a router that loops back and a LAN would.
    for (const Packet& pk : e.sent) {
        const NetAddress from = pk.to == dLan ? eLan : e.self;
        d.links.Receive(pk.route, from, pk.bytes.data(), pk.bytes.size(), 0, none);
    }
    e.sent.clear();
    CHECK(d.links.Open(5, 0, kind) && kind == PathKind::Lan);
    d.sent.clear();
    d.links.SendPoses(2, { Pose(0, 1) }, 1);
    CHECK(d.sent.size() == 1 && d.sent[0].to == eLan);
}

void TestEntries() {
    std::printf("entries: replace, forget, keep\n");
    Pair p;
    p.Exchange(0);
    PathKind kind = PathKind::Relay;
    // The same entry again keeps the open link.
    p.a.links.Apply({ EntryFor(p.b, p.secret) }, 100);
    CHECK(p.a.links.Open(2, 100, kind));
    // A new secret starts over.
    p.a.links.Apply({ EntryFor(p.b, Secret(0x6B)) }, 200);
    CHECK(!p.a.links.Open(2, 200, kind) && p.a.links.Known(2));
    // A forget entry and Keep drop peers.
    PeerEntry forget;
    forget.id = 2;
    p.a.links.Apply({ forget }, 300);
    CHECK(!p.a.links.Known(2) && p.a.links.PeerCount() == 0);
    p.a.links.Apply({ EntryFor(p.b, p.secret) }, 400);
    p.a.links.Keep({ 0, 1, 3 });
    CHECK(!p.a.links.Known(2));
    // An entry about this copy itself is ignored.
    p.a.links.Apply({ EntryFor(p.a, p.secret) }, 500);
    CHECK(p.a.links.PeerCount() == 0);
}

void TestRelayAddresses() {
    std::printf("relay addresses: the TURN permissions\n");
    Copy a{ 1, Addr(203, 0, 113, 1, 24302), Addr(198, 51, 100, 1, 40001) };
    Copy b{ 2, Addr(203, 0, 113, 2, 24302), Addr(198, 51, 100, 2, 40002) };
    Copy c{ 3, Addr(203, 0, 113, 3, 24302), Addr(198, 51, 100, 3, 40003) };
    Copy d{ 4, Addr(203, 0, 113, 4, 24302) };
    const Bytes32 s = Secret(3);
    CHECK(a.links.RelayAddresses().empty());
    // Listed whether or not this copy has an allocation yet: the permissions come before the first send.
    a.links.Apply({ EntryFor(b, s, true, true), EntryFor(c, s, false, true), EntryFor(d, s) }, 0);
    CHECK(a.links.RelayAddresses() == std::vector<NetAddress>({ b.relay, c.relay }));
    a.links.SetLocal(1, true, true);
    CHECK(a.links.RelayAddresses() == std::vector<NetAddress>({ b.relay, c.relay }));
    // Two peers behind one relay address are listed once.
    PeerEntry same = EntryFor(d, s, true, false);
    same.relay = EndpointOf(b.relay);
    a.links.Apply({ same }, 10);
    CHECK(a.links.RelayAddresses() == std::vector<NetAddress>({ b.relay, c.relay }));
    // A forgotten peer drops out.
    a.links.Keep({ 1, 3, 4 });
    CHECK(a.links.RelayAddresses() == std::vector<NetAddress>({ c.relay, b.relay }));
    a.links.Clear();
    CHECK(a.links.RelayAddresses().empty());
}

} // namespace

int main() {
    TestPunchOpensLink();
    TestPunchRateAndWindow();
    TestSilenceAndKeepalive();
    TestRefusals();
    TestDirectOff();
    TestRelayAndBestPath();
    TestEntries();
    TestRelayAddresses();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
