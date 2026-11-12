// Standalone harness for the O2 connection channel (port/tests/gdx_net_channel_tests.cpp).
//
// Two channels talk over a simulated link. The link drops, duplicates and delays datagrams with a
// seeded xorshift RNG, and time is a counter this file advances, so every run replays identically.
// No socket is opened. The channel only sees the send callback and the Receive/Update calls.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "../net/gdx_net_channel.h"

namespace {

using gdx::net::Channel;
using gdx::net::ChannelConfig;
using gdx::net::ChannelStats;
using gdx::net::CloseReason;
using gdx::net::EncodeChallenge;
using gdx::net::EncodeVersionRefuse;
using gdx::net::kHelloBytes;
using gdx::net::ParseHello;
using gdx::net::kInboxCap;
using gdx::net::kMaxDatagramBytes;
using gdx::net::kMaxMessagePayload;
using gdx::net::kProtocolVersion;
using gdx::net::kReliableWindow;
using gdx::net::kUdpIpv4HeaderBytes;
using gdx::net::kUdpIpv6HeaderBytes;
using gdx::net::kUnreliableOutCap;
using gdx::net::LinkState;
using gdx::net::Message;
using gdx::net::Role;
using gdx::net::QueueResult;

int gChecks = 0;
int gFails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        gChecks++;                                                         \
        if (!(cond)) {                                                     \
            gFails++;                                                      \
            std::printf("    [x] line %d: %s\n", __LINE__, #cond);        \
        }                                                                  \
    } while (0)

// Application message type shared by every stream in this file.
constexpr uint8_t kStreamType = 7;

// xorshift32. Same seed, same sequence, on every platform; std::random_device would break replay.
class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed != 0 ? seed : 1u) {
    }

    uint32_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Returns a value in [0, n). n must be nonzero.
    uint32_t Below(uint32_t n) {
        return Next() % n;
    }

    bool Percent(uint32_t p) {
        return Below(100) < p;
    }

private:
    uint32_t state_;
};

ChannelConfig ClientConfig(uint32_t nonce, uint16_t initialSeq = 0) {
    ChannelConfig cfg;
    cfg.role = Role::Client;
    cfg.connectionNonce = nonce;
    cfg.initialSequence = initialSeq;
    return cfg;
}

ChannelConfig ServerConfig(uint16_t initialSeq = 0) {
    ChannelConfig cfg;
    cfg.role = Role::Server;
    cfg.initialSequence = initialSeq;
    return cfg;
}

void PutCounter(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

// Returns the counter a stream message carries, or 0xFFFFFFFF when it is not one.
uint32_t GetCounter(const Message& m) {
    if (m.payload.size() != 4) {
        return 0xFFFFFFFFu;
    }
    return static_cast<uint32_t>(m.payload[0]) | (static_cast<uint32_t>(m.payload[1]) << 8) |
           (static_cast<uint32_t>(m.payload[2]) << 16) | (static_cast<uint32_t>(m.payload[3]) << 24);
}

// One datagram in flight on the simulated link.
struct Flight {
    uint32_t atMs;
    bool toServer;
    uint32_t sendNo; // order the datagram was sent in, so arrivals out of that order can be counted
    std::vector<uint8_t> bytes;
};

// Two channels joined by a lossy link. The send callbacks feed the link. Step() delivers what is due,
// then runs both timers, so a datagram sent at tick t can arrive at tick t + delay.
class Net {
public:
    Net(uint32_t seed, const ChannelConfig& clientCfg, const ChannelConfig& serverCfg)
        : client(clientCfg, [this](const uint8_t* data, size_t len) { Enqueue(true, data, len); }),
          server(serverCfg, [this](const uint8_t* data, size_t len) { Enqueue(false, data, len); }),
          rng_(seed) {
    }

    Channel client;
    Channel server;
    uint32_t now = 0;
    uint32_t lossPct = 0;
    uint32_t dupPct = 0;
    uint32_t minLatMs = 5;
    uint32_t maxLatMs = 5;
    bool blackhole = false;    // drops everything, both directions, while set
    size_t maxDatagram = 0;    // largest datagram seen on the link, for the MTU check
    std::vector<std::vector<uint8_t>> captured; // recent client-to-server datagrams, for the fuzz test
    uint32_t reordered = 0;                     // datagrams that arrived after a datagram sent later
    // What the channels sent and what reached them, by direction: index 1 is client to server, 0 the reverse.
    // A datagram counts as sent even when the link drops it, and each duplicate counts as its own arrival.
    uint64_t linkBytes[2] = { 0, 0 };
    uint32_t linkDatagrams[2] = { 0, 0 };
    uint64_t arrivedBytes[2] = { 0, 0 };
    uint32_t arrivedDatagrams[2] = { 0, 0 };

    void Step(uint32_t stepMs) {
        now += stepMs;
        Deliver();
        client.Update(now);
        server.Update(now);
    }

    void Run(uint32_t durationMs) {
        const uint32_t end = now + durationMs;
        while (now < end) {
            Step(5);
        }
    }

    // Steps until pred(*this) holds. Returns false if limitMs passes first.
    template <typename Pred>
    bool RunUntil(uint32_t limitMs, Pred pred) {
        const uint32_t end = now + limitMs;
        while (!pred(*this)) {
            if (now >= end) {
                return false;
            }
            Step(5);
        }
        return true;
    }

private:
    void Enqueue(bool toServer, const uint8_t* data, size_t len) {
        maxDatagram = std::max(maxDatagram, len);
        linkBytes[toServer ? 1 : 0] += len;
        linkDatagrams[toServer ? 1 : 0]++;
        if (toServer) {
            captured.emplace_back(data, data + len);
            if (captured.size() > 512) {
                captured.erase(captured.begin());
            }
        }
        if (blackhole || rng_.Percent(lossPct)) {
            return;
        }
        const uint32_t sendNo = nextSendNo_++;
        const int copies = rng_.Percent(dupPct) ? 2 : 1;
        for (int i = 0; i < copies; i++) {
            const uint32_t delay = minLatMs + rng_.Below(maxLatMs - minLatMs + 1);
            wire_.push_back(Flight{now + delay, toServer, sendNo, std::vector<uint8_t>(data, data + len)});
        }
    }

    void Deliver() {
        std::vector<Flight> due;
        std::vector<Flight> later;
        for (Flight& f : wire_) {
            if (f.atMs <= now) {
                due.push_back(std::move(f));
            } else {
                later.push_back(std::move(f));
            }
        }
        wire_.swap(later);
        // Earliest first, so a jittered copy really arrives after the datagram it trails.
        std::stable_sort(due.begin(), due.end(),
                         [](const Flight& a, const Flight& b) { return a.atMs < b.atMs; });
        for (Flight& f : due) {
            uint32_t& last = lastArrived_[f.toServer ? 1 : 0];
            if (f.sendNo < last) {
                reordered++;
            } else {
                last = f.sendNo;
            }
            Channel& dst = f.toServer ? server : client;
            arrivedBytes[f.toServer ? 1 : 0] += f.bytes.size();
            arrivedDatagrams[f.toServer ? 1 : 0]++;
            dst.Receive(f.bytes.data(), f.bytes.size(), now);
        }
    }

    Rng rng_;
    std::vector<Flight> wire_;
    uint32_t nextSendNo_ = 1;
    uint32_t lastArrived_[2] = {0, 0}; // newest send order that arrived, per direction
};

bool BothConnected(const Net& n) {
    return n.client.State() == LinkState::Connected && n.server.State() == LinkState::Connected;
}

bool ConnectPair(Net& net, uint32_t limitMs) {
    if (!net.client.Connect(net.now)) {
        return false;
    }
    return net.RunUntil(limitMs, BothConnected);
}

void ApplyLossy(Net& net) {
    net.lossPct = 20;
    net.dupPct = 10;
    net.minLatMs = 5;
    net.maxLatMs = 60;
}

struct StreamTally {
    uint32_t delivered = 0;  // matched the next expected counter, in order
    uint32_t mismatched = 0; // out of order, repeated, or not a stream message
    uint32_t extra = 0;      // arrived after the whole stream had been delivered
};

// Sends count reliable messages, up to perStep per tick, and checks each popped message against the next
// counter. perStep 1 gives one message per tick, so the packet count tracks the message count and a long
// run crosses the 16-bit packet sequence. Once they all arrive, it runs 2 s more and counts any stray copy.
StreamTally StreamReliable(Net& net, uint32_t count, uint32_t perStep, uint32_t limitMs) {
    StreamTally tally;
    uint32_t sent = 0;
    const uint32_t end = net.now + limitMs;
    Message m;
    while (tally.delivered < count && net.now < end) {
        for (uint32_t k = 0; k < perStep && sent < count; k++) {
            uint8_t body[4];
            PutCounter(sent, body);
            if (net.client.SendReliable(kStreamType, body, sizeof(body)) != QueueResult::Ok) {
                break;
            }
            sent++;
        }
        net.Step(5);
        while (net.server.PopMessage(m)) {
            if (m.reliable && m.type == kStreamType && GetCounter(m) == tally.delivered) {
                tally.delivered++;
            } else {
                tally.mismatched++;
            }
        }
    }
    net.Run(2000);
    while (net.server.PopMessage(m)) {
        tally.extra++;
    }
    return tally;
}

// Builds one hostile datagram: a replayed real packet, a bit-flipped or truncated copy, or random bytes.
std::vector<uint8_t> MakeFuzzPacket(Rng& rng, const std::vector<std::vector<uint8_t>>& captured) {
    const uint32_t mode = rng.Below(5);
    if (mode == 4 || captured.empty()) {
        // Random sizes run past the 1200-byte cap on purpose.
        std::vector<uint8_t> noise(rng.Below(1400));
        for (uint8_t& b : noise) {
            b = static_cast<uint8_t>(rng.Next());
        }
        if (noise.size() >= 2 && rng.Percent(50)) {
            noise[0] = 'G';
            noise[1] = 'D';
        }
        return noise;
    }
    std::vector<uint8_t> buf = captured[rng.Below(static_cast<uint32_t>(captured.size()))];
    if (mode == 1 || mode == 3) {
        const uint32_t flips = 1 + rng.Below(4);
        for (uint32_t i = 0; i < flips && !buf.empty(); i++) {
            buf[rng.Below(static_cast<uint32_t>(buf.size()))] ^= static_cast<uint8_t>(1u << rng.Below(8));
        }
    }
    if (mode == 2 || mode == 3) {
        buf.resize(rng.Below(static_cast<uint32_t>(buf.size()) + 1));
    }
    return buf;
}

void CaseHandshakeConnects() {
    Net net(1, ClientConfig(0xC0FFEE01u), ServerConfig());
    CHECK(net.client.State() == LinkState::Disconnected);
    CHECK(net.client.Connect(net.now));
    CHECK(net.client.State() == LinkState::Connecting);
    CHECK(!net.client.Connect(net.now)); // a second Connect while connecting is refused
    CHECK(net.RunUntil(1000, BothConnected));
    CHECK(net.client.Reason() == CloseReason::None);
    CHECK(net.server.Reason() == CloseReason::None);
    CHECK(!net.server.Connect(net.now)); // only a client dials out
}

void CaseVersionMismatchRefused() {
    ChannelConfig outdated = ClientConfig(0xBADC0DE0u);
    outdated.version = kProtocolVersion + 1;
    Net net(2, outdated, ServerConfig());
    CHECK(net.client.Connect(net.now));
    net.Run(2000);
    CHECK(net.client.State() == LinkState::Disconnected);
    CHECK(net.client.Reason() == CloseReason::VersionMismatch);
    CHECK(net.server.State() == LinkState::Disconnected); // the refused peer never took the slot
    CHECK(net.server.Stats().packetsIn > 0);
}

// Captured wire replies from one server fed by two clients, plus a retransmitted hello.
// Byte layout: [0..1] magic, [2] type, [3..6] nonce, [7] refuse reason.
void CaseBusyRefusalAndHelloRepeat() {
    std::vector<std::vector<uint8_t>> toA;
    std::vector<std::vector<uint8_t>> toB;
    std::vector<std::vector<uint8_t>> fromServer;
    Channel a(ClientConfig(0x11111111u), [&toA](const uint8_t* d, size_t n) { toA.emplace_back(d, d + n); });
    Channel b(ClientConfig(0x22222222u), [&toB](const uint8_t* d, size_t n) { toB.emplace_back(d, d + n); });
    Channel server(ServerConfig(), [&fromServer](const uint8_t* d, size_t n) { fromServer.emplace_back(d, d + n); });
    CHECK(a.Connect(0));
    CHECK(b.Connect(0));
    CHECK(toA.size() == 1 && toB.size() == 1);
    server.Receive(toA[0].data(), toA[0].size(), 0); // the first hello takes the slot
    server.Receive(toB[0].data(), toB[0].size(), 0); // the second peer is refused
    CHECK(server.State() == LinkState::Connected);
    CHECK(fromServer.size() == 2);
    CHECK(fromServer.size() == 2 && fromServer[0][2] == 1);
    CHECK(fromServer.size() == 2 && fromServer[1][2] == 2);
    CHECK(fromServer.size() == 2 && fromServer[1][7] == static_cast<uint8_t>(CloseReason::Busy));
    CHECK(fromServer.size() == 2 && fromServer[1][3] == 0x22); // refusal echoes the refused nonce
    a.Update(250); // no accept arrived, so a resends its hello
    CHECK(toA.size() == 2);
    server.Receive(toA[1].data(), toA[1].size(), 250);
    CHECK(fromServer.size() == 3 && fromServer[2][2] == 1); // same nonce: accept again
    CHECK(server.State() == LinkState::Connected);
}

void CaseReliableOrderedExactlyOnce() {
    Net net(3, ClientConfig(0x1234ABCDu), ServerConfig());
    ApplyLossy(net);
    CHECK(ConnectPair(net, 3000));
    const StreamTally t = StreamReliable(net, 500, 8, 120000);
    CHECK(t.delivered == 500);
    CHECK(t.mismatched == 0);
    CHECK(t.extra == 0);
    CHECK(net.client.Stats().resends > 0);    // losses forced resends
    CHECK(net.server.Stats().duplicates > 0); // duplicates were seen and absorbed
    CHECK(net.reordered > 0);                 // jitter delivered datagrams out of send order
    CHECK(net.client.State() == LinkState::Connected);
}

void CaseUnreliableNeverDuplicated() {
    constexpr uint32_t kCount = 1000;
    Net net(4, ClientConfig(0x0FACADEu), ServerConfig());
    ApplyLossy(net);
    CHECK(ConnectPair(net, 3000));
    std::vector<uint8_t> seen(kCount, 0);
    uint32_t delivered = 0;
    uint32_t repeats = 0;
    uint32_t strays = 0;
    Message m;
    for (uint32_t i = 0; i < kCount + 400; i++) { // the extra ticks drain the tail
        if (i < kCount) {
            uint8_t body[4];
            PutCounter(i, body);
            CHECK(net.client.SendUnreliable(kStreamType, body, sizeof(body)) == QueueResult::Ok);
        }
        net.Step(5);
        while (net.server.PopMessage(m)) {
            const uint32_t v = GetCounter(m);
            if (m.reliable || v >= kCount) {
                strays++;
            } else if (seen[v] != 0) {
                repeats++;
            } else {
                seen[v] = 1;
                delivered++;
            }
        }
    }
    CHECK(strays == 0);
    CHECK(repeats == 0);                     // a duplicated packet must not deliver twice
    CHECK(delivered > 700 && delivered < kCount); // lossy, so some are gone, but not most
    CHECK(net.server.Stats().duplicates > 0); // duplicated packets were actually seen
}

void CaseTimeoutFiresOnSilentLink() {
    Net net(5, ClientConfig(0x5117E000u), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    net.blackhole = true; // the link goes silent in both directions
    // The last packet arrived at or just before the blackout, so the 10 s silence timer fires within
    // about 10 s of it. The checks sit at 8 s (still up) and 10.5 s (down).
    net.Run(8000);
    CHECK(net.client.State() == LinkState::Connected);
    net.Run(2500);
    CHECK(net.client.State() == LinkState::Disconnected);
    CHECK(net.client.Reason() == CloseReason::Timeout);
    CHECK(net.server.State() == LinkState::Disconnected);
    CHECK(net.server.Reason() == CloseReason::Timeout);
}

void CaseKeepaliveHoldsIdleLink() {
    Net net(6, ClientConfig(0x0FFEE5u), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    const uint32_t before = net.client.Stats().packetsOut;
    net.Run(60000); // a minute with no application traffic, far past the 10 s timeout
    CHECK(net.client.State() == LinkState::Connected);
    CHECK(net.server.State() == LinkState::Connected);
    CHECK(net.client.Reason() == CloseReason::None);
    CHECK(net.client.Stats().packetsOut - before >= 50); // about one keepalive per second
}

void CaseSequenceAndIdWrap() {
    // Both counters start 6 short of 65536, so the packet sequence and the message id wrap almost at once.
    Net wrapNet(7, ClientConfig(0x0BADF00Du, 65530), ServerConfig(65530));
    ApplyLossy(wrapNet);
    CHECK(ConnectPair(wrapNet, 3000));
    const StreamTally early = StreamReliable(wrapNet, 300, 8, 60000);
    CHECK(early.delivered == 300);
    CHECK(early.mismatched == 0);
    CHECK(early.extra == 0);

    // A long run from zero, one message per tick: 70000 messages and at least 65536 packets, so the
    // packet sequence crosses the 16-bit limit naturally. Losses stretch the run to a few hundred seconds.
    Net longRun(8, ClientConfig(0x600DF00Du), ServerConfig());
    ApplyLossy(longRun);
    CHECK(ConnectPair(longRun, 3000));
    const StreamTally late = StreamReliable(longRun, 70000, 1, 1200000);
    CHECK(late.delivered == 70000);
    CHECK(late.mismatched == 0);
    CHECK(late.extra == 0);
    CHECK(longRun.client.Stats().packetsOut > 65536u);
}

void CaseOversizeRefusedAndLargestFits() {
    Net net(9, ClientConfig(0x0B16u), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    std::vector<uint8_t> tooBig(kMaxMessagePayload + 1, 0x5A);
    CHECK(net.client.SendReliable(kStreamType, tooBig.data(), tooBig.size()) == QueueResult::TooLarge);
    CHECK(net.client.SendUnreliable(kStreamType, tooBig.data(), tooBig.size()) == QueueResult::TooLarge);
    std::vector<uint8_t> largest(kMaxMessagePayload, 0xA5);
    CHECK(net.client.SendReliable(kStreamType, largest.data(), largest.size()) == QueueResult::Ok);
    CHECK(net.client.SendReliable(kStreamType, nullptr, 3) == QueueResult::Invalid);
    net.Run(500);
    Message m;
    CHECK(net.server.PopMessage(m));
    CHECK(m.reliable && m.type == kStreamType);
    CHECK(m.payload == largest); // the largest allowed message arrives intact
    CHECK(!net.server.PopMessage(m));
    CHECK(net.maxDatagram == kMaxDatagramBytes); // the largest message fills one datagram exactly
}

void CaseQueueCapsHold() {
    Net net(10, ClientConfig(0xCA9u), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    net.blackhole = true; // nothing gets through, so nothing is ever acked
    const uint8_t one[1] = {1};
    for (size_t i = 0; i < kReliableWindow; i++) {
        CHECK(net.client.SendReliable(kStreamType, one, 1) == QueueResult::Ok);
    }
    CHECK(net.client.SendReliable(kStreamType, one, 1) == QueueResult::QueueFull);
    for (size_t i = 0; i < kUnreliableOutCap; i++) {
        CHECK(net.client.SendUnreliable(kStreamType, one, 1) == QueueResult::Ok);
    }
    CHECK(net.client.SendUnreliable(kStreamType, one, 1) == QueueResult::QueueFull);
    net.Step(5); // Update flushes the unreliable outbox; the reliable window stays full until acked
    CHECK(net.client.SendUnreliable(kStreamType, one, 1) == QueueResult::Ok);
    CHECK(net.client.SendReliable(kStreamType, one, 1) == QueueResult::QueueFull);
    net.Run(3000); // resends must not grow the window
    CHECK(net.client.SendReliable(kStreamType, one, 1) == QueueResult::QueueFull);
    net.Run(10000); // silence gets the link dropped, which frees everything
    CHECK(net.client.State() == LinkState::Disconnected);
    CHECK(net.client.SendReliable(kStreamType, one, 1) == QueueResult::NotConnected);
}

void CaseInboxOverflowClosesLink() {
    Net net(11, ClientConfig(0x0F10B0Au), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    // The server never pops, so its inbox fills. The next reliable delivery must close the link,
    // because a reliable message cannot be dropped without the sender ever knowing.
    uint32_t sent = 0;
    for (uint32_t i = 0; i < 1000 && net.server.State() == LinkState::Connected; i++) {
        if (net.client.SendReliable(kStreamType, nullptr, 0) == QueueResult::Ok) {
            sent++;
        }
        net.Step(5);
    }
    CHECK(net.server.State() == LinkState::Disconnected);
    CHECK(net.server.Reason() == CloseReason::Overflow);
    CHECK(sent >= kInboxCap);
    net.Run(500);
    CHECK(net.client.Reason() == CloseReason::Overflow); // the peer was told why
}

void CaseExplicitDisconnectCarriesReason() {
    Net net(12, ClientConfig(0xD15Cu), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    net.client.Disconnect(net.now);
    CHECK(net.client.State() == LinkState::Disconnected);
    CHECK(net.client.Reason() == CloseReason::Closed);
    net.Run(100);
    CHECK(net.server.State() == LinkState::Disconnected);
    CHECK(net.server.Reason() == CloseReason::Closed);
}

void CaseRttTracksLatency() {
    Net net(13, ClientConfig(0x0477u), ServerConfig());
    net.minLatMs = 20;
    net.maxLatMs = 20;
    CHECK(ConnectPair(net, 1000));
    const uint8_t one[1] = {1};
    for (int i = 0; i < 50; i++) {
        net.client.SendReliable(kStreamType, one, 1);
        net.Run(20);
    }
    net.Run(500);
    // 20 ms each way is a 40 ms round trip; the sampled smoothing should sit close to it.
    const uint32_t rtt = net.client.Stats().rttMs;
    CHECK(rtt >= 35 && rtt <= 50);
}

// Each malformed or hostile packet is counted and dropped. The link stays up, and a replay is harmless.
void CaseDefensiveCountersMoveOnBadPackets() {
    Net net(14, ClientConfig(0xDEFE0001u), ServerConfig());
    CHECK(ConnectPair(net, 1000));
    net.captured.clear();
    CHECK(net.client.SendReliable(kStreamType, nullptr, 0) == QueueResult::Ok);
    net.Step(5);
    CHECK(!net.captured.empty());
    const std::vector<uint8_t> good = net.captured.back();
    const ChannelStats before = net.server.Stats();

    std::vector<uint8_t> magic = good;
    magic[0] ^= 0xFF;
    std::vector<uint8_t> nonce = good;
    nonce[3] ^= 0x01;
    const std::vector<uint8_t> cut(good.begin(), good.begin() + 9);
    std::vector<uint8_t> channel = good;
    channel[16] = 2; // channel byte must be 0 or 1; byte 16 is the first message's channel
    std::vector<uint8_t> length = good;
    length[19] = 0xFF; // first message's length field, set past the payload cap
    length[20] = 0xFF;

    net.server.Receive(magic.data(), magic.size(), net.now);
    net.server.Receive(nonce.data(), nonce.size(), net.now);
    net.server.Receive(cut.data(), cut.size(), net.now);
    net.server.Receive(channel.data(), channel.size(), net.now);
    net.server.Receive(length.data(), length.size(), net.now);
    const ChannelStats& s = net.server.Stats();
    CHECK(s.badMagic == before.badMagic + 1);
    CHECK(s.wrongNonce == before.wrongNonce + 1);
    CHECK(s.malformed == before.malformed + 3);
    CHECK(net.server.State() == LinkState::Connected);
    Message m;
    net.server.Receive(good.data(), good.size(), net.now); // the real packet, delivered once
    CHECK(net.server.PopMessage(m) && m.reliable);
    const uint32_t dupBefore = s.duplicates;
    net.server.Receive(good.data(), good.size(), net.now); // the same datagram again is a duplicate
    CHECK(s.duplicates == dupBefore + 1);
    CHECK(!net.server.PopMessage(m));
}

// A reliable id past the reorder window is rejected with its whole packet: nothing is delivered and
// nothing is acked, so the sender keeps the message. Byte layout of a data packet with one reliable
// message: [7..8] seq, [9..10] ack, [11..14] ackBits, [15] type, [16] channel, [17..18] id.
void CaseOutOfWindowPacketIsNotAcked() {
    std::vector<std::vector<uint8_t>> toServer;
    std::vector<std::vector<uint8_t>> toClient;
    Channel client(ClientConfig(0x0DD1D000u),
                   [&toServer](const uint8_t* d, size_t n) { toServer.emplace_back(d, d + n); });
    Channel server(ServerConfig(), [&toClient](const uint8_t* d, size_t n) { toClient.emplace_back(d, d + n); });
    CHECK(client.Connect(0));
    CHECK(toServer.size() == 1);
    server.Receive(toServer[0].data(), toServer[0].size(), 0);
    CHECK(toClient.size() == 1);
    client.Receive(toClient[0].data(), toClient[0].size(), 0);
    CHECK(client.State() == LinkState::Connected && server.State() == LinkState::Connected);

    const uint8_t one[1] = {1};
    CHECK(client.SendReliable(kStreamType, one, 1) == QueueResult::Ok);
    client.Update(1);
    CHECK(toServer.size() == 2);
    const std::vector<uint8_t> good = toServer.back();
    CHECK(good.size() > 18 && good[16] == 1);
    std::vector<uint8_t> ahead = good;
    ahead[17] = static_cast<uint8_t>(kReliableWindow + 36); // id 100, past the 64-id window from 0
    ahead[18] = 0;

    const size_t repliesBefore = toClient.size();
    server.Receive(ahead.data(), ahead.size(), 1);
    CHECK(server.Stats().outOfWindow == 1);
    server.Update(2); // an owed ack would go out here; the keepalive is not due for another second
    CHECK(toClient.size() == repliesBefore);
    Message m;
    CHECK(!server.PopMessage(m));
    CHECK(server.State() == LinkState::Connected);

    server.Receive(good.data(), good.size(), 3); // the real packet still lands, and is acked
    server.Update(4);
    CHECK(toClient.size() == repliesBefore + 1);
    CHECK(server.PopMessage(m) && m.reliable && m.payload.size() == 1);
}

// Real client packets give the fuzzer valid datagrams to corrupt. The handshake packets are dropped from
// the corpus first: a replayed hello would connect the fresh channel, which is valid protocol, not a fault.
// The clock stays frozen during the fuzz, so no timeout can fire.
void CaseFuzzNeverCrashes() {
    Net net(15, ClientConfig(0xF022u), ServerConfig());
    ApplyLossy(net);
    CHECK(ConnectPair(net, 3000));
    net.captured.clear();
    Message m;
    for (int i = 0; i < 40; i++) {
        uint8_t body[4];
        PutCounter(static_cast<uint32_t>(i), body);
        net.client.SendReliable(kStreamType, body, sizeof(body));
        net.client.SendUnreliable(kStreamType, body, sizeof(body));
        net.Step(5);
        while (net.server.PopMessage(m)) {
        }
    }
    CHECK(!net.captured.empty());
    Channel fresh(ServerConfig(), [](const uint8_t*, size_t) {});
    Rng rng(0xFEEDBEEFu);
    const uint32_t frozen = net.now;
    const ChannelStats before = net.server.Stats();
    for (int i = 0; i < 50000; i++) {
        const std::vector<uint8_t> buf = MakeFuzzPacket(rng, net.captured);
        net.server.Receive(buf.data(), buf.size(), frozen);
        fresh.Receive(buf.data(), buf.size(), frozen);
        while (net.server.PopMessage(m)) {
        }
    }
    const ChannelStats& s = net.server.Stats();
    CHECK(s.packetsIn == before.packetsIn + 50000);
    CHECK(s.malformed > before.malformed);
    CHECK(s.badMagic > before.badMagic);
    CHECK(s.wrongNonce > before.wrongNonce);
    CHECK(s.duplicates > before.duplicates);
    CHECK(net.server.State() == LinkState::Connected);
    const ChannelStats& f = fresh.Stats();
    CHECK(f.packetsIn == 50000);
    CHECK(f.badMagic + f.malformed + f.ignored > 0);
    CHECK(fresh.State() == LinkState::Disconnected);
}

// The four sent-byte parts add up to bytesSent. Message bytes are split by kind, and the rest is framing.
bool SplitAddsUp(const ChannelStats& s) {
    return s.bytesSentReliable + s.bytesSentUnreliable + s.bytesSentRetransmit + s.bytesSentAckKeepalive ==
           s.bytesSent;
}

// The byte counters agree with the datagrams the link carried, in both directions, on a lossy link. The link
// counts each datagram at the same call that the channel counts it, so the identities hold at any instant.
void CaseWireCountersMatchTheLink() {
    Net net(0x51A7u, ClientConfig(0xB17E5u), ServerConfig());
    CHECK(ConnectPair(net, 2000));
    ApplyLossy(net);
    const StreamTally tally = StreamReliable(net, 300, 3, 60000);
    CHECK(tally.delivered == 300);
    // Unreliable traffic from the server, so both sides have a split to check.
    const uint8_t ping[16] = {};
    for (uint32_t i = 0; i < 40; i++) {
        CHECK(net.server.SendUnreliable(kStreamType, ping, sizeof(ping)) == QueueResult::Ok);
        net.Run(20);
    }
    net.Run(3000);

    const ChannelStats& c = net.client.Stats();
    const ChannelStats& s = net.server.Stats();
    const uint64_t header = kUdpIpv4HeaderBytes;
    CHECK(c.datagramsSent == net.linkDatagrams[1] && c.packetsOut == c.datagramsSent);
    CHECK(c.bytesSent == net.linkBytes[1] + header * net.linkDatagrams[1]);
    CHECK(s.datagramsSent == net.linkDatagrams[0] && s.packetsOut == s.datagramsSent);
    CHECK(s.bytesSent == net.linkBytes[0] + header * net.linkDatagrams[0]);
    CHECK(s.datagramsReceived == net.arrivedDatagrams[1] && s.packetsIn == s.datagramsReceived);
    CHECK(s.bytesReceived == net.arrivedBytes[1] + header * net.arrivedDatagrams[1]);
    CHECK(c.datagramsReceived == net.arrivedDatagrams[0] && c.packetsIn == c.datagramsReceived);
    CHECK(c.bytesReceived == net.arrivedBytes[0] + header * net.arrivedDatagrams[0]);

    CHECK(SplitAddsUp(c) && SplitAddsUp(s));
    CHECK(c.bytesSentReliable > 0 && c.bytesSentRetransmit > 0); // 20 percent loss forced resends
    CHECK(c.bytesSentUnreliable == 0 && c.bytesSentAckKeepalive > 0);
    CHECK(s.bytesSentUnreliable > 0 && s.bytesSentReliable == 0 && s.bytesSentRetransmit == 0);
    // A resent message still carries its 6-byte reliable header, so the retransmit bytes are at least that per resend.
    CHECK(c.bytesSentRetransmit >= 6 * static_cast<uint64_t>(c.resends));
}

// The header estimate belongs to each channel: 28 by default, 48 once set, and each side counts with its own.
void CaseHeaderEstimateIsSettable() {
    CHECK(kUdpIpv4HeaderBytes == 28 && kUdpIpv6HeaderBytes == 48);
    Net fresh(0x5E7u, ClientConfig(0xA11CEu), ServerConfig());
    CHECK(fresh.client.HeaderEstimateBytes() == 28 && fresh.server.HeaderEstimateBytes() == 28);

    Net net(0x5E8u, ClientConfig(0xA11CEu), ServerConfig());
    net.client.SetHeaderEstimateBytes(kUdpIpv6HeaderBytes);
    CHECK(net.client.HeaderEstimateBytes() == 48 && net.server.HeaderEstimateBytes() == 28);
    CHECK(ConnectPair(net, 2000));
    CHECK(StreamReliable(net, 50, 5, 20000).delivered == 50);
    net.Run(1000);

    const ChannelStats& c = net.client.Stats();
    const ChannelStats& s = net.server.Stats();
    CHECK(c.datagramsSent == net.linkDatagrams[1]);
    CHECK(c.bytesSent == net.linkBytes[1] + 48u * net.linkDatagrams[1]);
    CHECK(s.bytesSent == net.linkBytes[0] + 28u * net.linkDatagrams[0]);
    CHECK(s.bytesReceived == net.arrivedBytes[1] + 28u * net.arrivedDatagrams[1]);
    CHECK(c.bytesReceived == net.arrivedBytes[0] + 48u * net.arrivedDatagrams[0]);
    CHECK(SplitAddsUp(c) && SplitAddsUp(s));
}

// A datagram that cannot be parsed still crossed the wire, so the receive counters include it.
void CaseReceiveCountsEveryDatagram() {
    Channel ch(ServerConfig(), [](const uint8_t*, size_t) {});
    const uint8_t junk[3] = { 1, 2, 3 };
    ch.Receive(junk, sizeof(junk), 0);
    ch.Receive(nullptr, 0, 0);
    const ChannelStats& s = ch.Stats();
    CHECK(s.packetsIn == 2 && s.datagramsReceived == 2);
    CHECK(s.malformed == 2);
    CHECK(s.bytesReceived == (3u + 28u) + 28u);
    CHECK(s.bytesSent == 0 && s.datagramsSent == 0 && SplitAddsUp(s));
}

} // namespace

// The stateless gate's packets (decision 34). A client answers a challenge by sending its hello again at once,
// carrying the cookie; a challenge with another nonce, or one sent to a server, changes nothing.
void CaseChallengeEchoedInNextHello() {
    std::vector<std::vector<uint8_t>> sent;
    Channel client(ClientConfig(0x0A0B0C0Du), [&sent](const uint8_t* d, size_t n) { sent.emplace_back(d, d + n); });
    CHECK(client.Connect(0));
    CHECK(sent.size() == 1 && sent[0].size() == kHelloBytes);
    uint32_t nonce = 0;
    uint16_t version = 0;
    uint64_t cookie = 1;
    CHECK(ParseHello(sent[0].data(), sent[0].size(), nonce, version, cookie));
    CHECK(nonce == 0x0A0B0C0Du && version == kProtocolVersion && cookie == 0);

    const std::vector<uint8_t> wrongNonce = EncodeChallenge(0x0A0B0C0Eu, kProtocolVersion, 0x1122334455667788ull);
    client.Receive(wrongNonce.data(), wrongNonce.size(), 10);
    CHECK(sent.size() == 1 && client.Stats().wrongNonce == 1);

    const std::vector<uint8_t> challenge = EncodeChallenge(0x0A0B0C0Du, kProtocolVersion, 0x1122334455667788ull);
    CHECK(challenge.size() == kHelloBytes); // never larger than the hello it answers
    client.Receive(challenge.data(), challenge.size(), 20);
    CHECK(sent.size() == 2);
    CHECK(sent.size() == 2 && ParseHello(sent[1].data(), sent[1].size(), nonce, version, cookie));
    CHECK(cookie == 0x1122334455667788ull && client.State() == LinkState::Connecting);
    client.Update(20 + 250); // the retry carries the cookie too
    CHECK(sent.size() == 3 && ParseHello(sent[2].data(), sent[2].size(), nonce, version, cookie));
    CHECK(cookie == 0x1122334455667788ull);

    // A fresh Connect forgets the cookie: the server binds it to one nonce.
    client.Disconnect(300);
    Channel again(ClientConfig(0x0A0B0C0Fu), [&sent](const uint8_t* d, size_t n) { sent.emplace_back(d, d + n); });
    CHECK(again.Connect(400));
    CHECK(ParseHello(sent.back().data(), sent.back().size(), nonce, version, cookie) && cookie == 0);

    // A server ignores a challenge; the refusal for another version is no larger than a hello.
    Channel server(ServerConfig(), [&sent](const uint8_t* d, size_t n) { sent.emplace_back(d, d + n); });
    const size_t before = sent.size();
    server.Receive(challenge.data(), challenge.size(), 500);
    CHECK(sent.size() == before && server.Stats().ignored == 1);
    CHECK(EncodeVersionRefuse(1, kProtocolVersion).size() <= kHelloBytes);
    // ParseHello reads hellos only.
    CHECK(!ParseHello(challenge.data(), challenge.size(), nonce, version, cookie));
}

int main() {
    struct {
        const char* name;
        void (*fn)();
    } cases[] = {
        { "handshake connects", CaseHandshakeConnects },
        { "version mismatch refused with its reason", CaseVersionMismatchRefused },
        { "busy refusal, refused nonce echoed, hello repeat accepted", CaseBusyRefusalAndHelloRepeat },
        { "500 reliable messages: in order, exactly once, under loss", CaseReliableOrderedExactlyOnce },
        { "unreliable messages never delivered twice", CaseUnreliableNeverDuplicated },
        { "timeout fires when the link goes silent", CaseTimeoutFiresOnSilentLink },
        { "keepalive keeps an idle link alive", CaseKeepaliveHoldsIdleLink },
        { "sequence and message id wraparound", CaseSequenceAndIdWrap },
        { "oversize message refused, largest message intact", CaseOversizeRefusedAndLargestFits },
        { "send queue caps hold", CaseQueueCapsHold },
        { "inbox overflow closes the link with a reason", CaseInboxOverflowClosesLink },
        { "explicit disconnect carries its reason", CaseExplicitDisconnectCarriesReason },
        { "RTT tracks link latency", CaseRttTracksLatency },
        { "malformed and hostile packets are counted and dropped", CaseDefensiveCountersMoveOnBadPackets },
        { "out-of-window reliable id: packet dropped unacked", CaseOutOfWindowPacketIsNotAcked },
        { "50000 fuzzed packets: no crash, counters move", CaseFuzzNeverCrashes },
        { "wire counters agree with the link, both directions, under loss", CaseWireCountersMatchTheLink },
        { "header estimate: 28 by default, settable to 48 per channel", CaseHeaderEstimateIsSettable },
        { "every received datagram is counted, parsed or not", CaseReceiveCountsEveryDatagram },
        { "a challenge is echoed in the next hello (O5.1)", CaseChallengeEchoedInNextHello },
    };
    const int numCases = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
    int failedCases = 0;

    std::printf("=== G-Diffuser O2 connection channel tests (port/net/gdx_net_channel.cpp) ===\n\n");
    for (int i = 0; i < numCases; i++) {
        const int failsBefore = gFails;
        const int checksBefore = gChecks;
        std::printf("-- %s\n", cases[i].name);
        cases[i].fn();
        if (gFails > failsBefore) {
            failedCases++;
            std::printf("[FAIL] %s (%d/%d sub-checks failed)\n\n", cases[i].name, gFails - failsBefore,
                        gChecks - checksBefore);
        } else {
            std::printf("[PASS] %s (%d sub-checks)\n\n", cases[i].name, gChecks - checksBefore);
        }
    }
    std::printf("=== Summary: %d/%d cases passed (%d/%d sub-checks passed) ===\n", numCases - failedCases,
                numCases, gChecks - gFails, gChecks);
    return failedCases ? 1 : 0;
}
