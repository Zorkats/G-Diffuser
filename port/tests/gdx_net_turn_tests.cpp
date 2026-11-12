// port/tests/gdx_net_turn_tests.cpp: the TURN codec and client (G-NET decisions 69, 72).
// Copyright (c) Zorkats
//
// Usage: gdx_net_turn_tests [fuzz rounds, default 3000]
//
// The codec is held to the RFC 5769 samples: it parses all four, checks their MESSAGE-INTEGRITY and FINGERPRINT,
// reads their addresses, and builds the long-term request of section 2.4 byte for byte. The client then runs
// against a fake TURN server written on that codec: the 401 and the signed Allocate, Refresh before the lifetime
// runs out, a stale nonce, ChannelBind and ChannelData both ways, the CreatePermission fallback, loss, a forged
// answer, refused credentials and the closing Refresh.

#include "gdx_net_turn.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using gdx::net::NetAddress;
namespace turn = gdx::net::turn;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Bytes Hex(const char* text) {
    Bytes out;
    int high = -1;
    for (const char* p = text; *p != '\0'; ++p) {
        const int v = HexValue(*p);
        if (v < 0) continue;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<uint8_t>(high * 16 + v));
            high = -1;
        }
    }
    return out;
}

Bytes Text(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

// ---- RFC 5769 ----

const char* kSample21 =
    "00010058 2112a442 b7e7a701 bc34d686 fa87dfae 80220010 5354554e 20746573 7420636c 69656e74 00240004 6e0001ff "
    "80290008 932ff9b1 51263b36 00060009 6576746a 3a683676 59202020 00080014 9aeaa70c bfd8cb56 781ef2b5 b2d3f249 "
    "c1b571a2 80280004 e57a3bcf";
const char* kSample22 =
    "0101003c 2112a442 b7e7a701 bc34d686 fa87dfae 8022000b 74657374 20766563 746f7220 00200008 0001a147 e112a643 "
    "00080014 2b91f599 fd9e90c3 8c7489f9 2af9ba53 f06be7d7 80280004 c07d4c96";
const char* kSample23 =
    "01010048 2112a442 b7e7a701 bc34d686 fa87dfae 8022000b 74657374 20766563 746f7220 00200014 0002a147 0113a9fa "
    "a5d3f179 bc25f4b5 bed2b9d9 00080014 a382954e 4be67bf1 1784c97c 8292c275 bfe3ed41 80280004 c8fb0b4c";
const char* kSample24 =
    "00010060 2112a442 78ad3433 c6ad72c0 29da412e 00060012 e3839ee3 8388e383 aae38383 e382afe3 82b90000 0015001c "
    "662f2f34 39396b39 35346436 4f4c3334 6f4c3946 53547679 36347341 0014000b 6578616d 706c652e 6f726700 00080014 "
    "f6702465 6dd64a3e 02b8e071 2e85c9a2 8ca89666";

void CheckRfc5769() {
    std::printf("codec: the RFC 5769 samples\n");
    const Bytes shortTerm = Text("VOkJxbRl1RmTxUk/WvJxBt");
    const Bytes s21 = Hex(kSample21);
    const Bytes s22 = Hex(kSample22);
    const Bytes s23 = Hex(kSample23);
    const Bytes s24 = Hex(kSample24);

    turn::Parsed p;
    Check(turn::Parse(s21.data(), s21.size(), p), "2.1 parses (FINGERPRINT checked)");
    Check(p.type == 0x0001 && p.hasIntegrity, "2.1 is a signed Binding request");
    Check(turn::IntegrityOk(s21.data(), p, shortTerm.data(), shortTerm.size()), "2.1 MESSAGE-INTEGRITY");
    Check(turn::ReadText(s21.data(), p, turn::kAttrUsername) == "evtj:h6vY", "2.1 USERNAME");
    const Bytes wrongKey = Text("VOkJxbRl1RmTxUk/WvJxBu");
    Check(!turn::IntegrityOk(s21.data(), p, wrongKey.data(), wrongKey.size()), "2.1 refuses another key");

    Check(turn::Parse(s22.data(), s22.size(), p), "2.2 parses");
    Check(turn::IntegrityOk(s22.data(), p, shortTerm.data(), shortTerm.size()), "2.2 MESSAGE-INTEGRITY");
    NetAddress mapped;
    Check(turn::ReadAddress(s22.data(), p, turn::kAttrMappedAddress, mapped) &&
              mapped == NetAddress::FromV4(0xC0000201u, 32853),
          "2.2 XOR-MAPPED-ADDRESS is 192.0.2.1:32853");

    Check(turn::Parse(s23.data(), s23.size(), p), "2.3 parses");
    Check(turn::IntegrityOk(s23.data(), p, shortTerm.data(), shortTerm.size()), "2.3 MESSAGE-INTEGRITY");
    const Bytes v6 = Hex("20010db8 12345678 00112233 44556677");
    Check(turn::ReadAddress(s23.data(), p, turn::kAttrMappedAddress, mapped) && mapped.port == 32853 &&
              std::memcmp(mapped.ip.data(), v6.data(), 16) == 0,
          "2.3 XOR-MAPPED-ADDRESS is 2001:db8:1234:5678:11:2233:4455:6677 port 32853");

    const std::string username = std::string("\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9");
    const std::array<uint8_t, 16> key = turn::LongTermKey(username, "example.org", "TheMatrIX");
    Check(turn::Parse(s24.data(), s24.size(), p), "2.4 parses");
    Check(turn::IntegrityOk(s24.data(), p, key.data(), key.size()), "2.4 MESSAGE-INTEGRITY under the long-term key");

    turn::TransactionId txid{};
    std::memcpy(txid.data(), s24.data() + 8, txid.size());
    turn::Builder builder(0x0001, txid);
    builder.AddText(turn::kAttrUsername, username);
    builder.AddText(turn::kAttrNonce, "f//499k954d6OL34oL9FSTvy64sA");
    builder.AddText(turn::kAttrRealm, "example.org");
    Check(builder.Finish(key.data(), key.size(), false) == s24, "the builder writes 2.4 byte for byte");

    // Refusals.
    Bytes bad = s21;
    bad[bad.size() - 1] ^= 1u;
    Check(!turn::Parse(bad.data(), bad.size(), p), "a wrong FINGERPRINT is refused");
    bad = s22;
    bad[30] ^= 1u; // a byte of the SOFTWARE value, covered by the integrity and the fingerprint
    Check(!turn::Parse(bad.data(), bad.size(), p), "a changed byte breaks the FINGERPRINT");
    bad = s24;
    bad[30] ^= 1u;
    Check(turn::Parse(bad.data(), bad.size(), p) && !turn::IntegrityOk(bad.data(), p, key.data(), key.size()),
          "a changed byte breaks MESSAGE-INTEGRITY");
    Check(!turn::Parse(s21.data(), s21.size() - 4, p), "a short message is refused");
    bad = s21;
    bad.push_back(0);
    Check(!turn::Parse(bad.data(), bad.size(), p), "a trailing byte is refused");
    bad = s21;
    bad[4] ^= 1u;
    Check(!turn::Parse(bad.data(), bad.size(), p), "a wrong cookie is refused");
    const Bytes channel = Hex("40000002 abcd");
    Check(turn::IsChannelData(channel.data(), channel.size()) && !turn::IsChannelData(s21.data(), s21.size()),
          "ChannelData is told from STUN by its first two bits");
}

// ---- A fake TURN server ----

const NetAddress kRelay = NetAddress::FromV4(0xCB007105u, 50000); // 203.0.113.5
const NetAddress kMapped = NetAddress::FromV4(0xC6336402u, 41000); // 198.51.100.2
const NetAddress kPeerA = NetAddress::FromV4(0xCB007106u, 50002);
const NetAddress kPeerB = NetAddress::FromV4(0xCB007107u, 50004);

struct Server {
    std::string username = "1700000000:gnet";
    std::string password = "secret";
    std::string realm = "example.org";
    std::string nonce = "nonce-1";
    std::array<uint8_t, 16> key{};
    uint32_t lifetime = 600;

    int dropNext = 0;        // client datagrams lost on the way
    bool staleNext = false;  // answer the next signed request with 438 and a new nonce
    bool refuseBind = false; // answer ChannelBind with 403
    bool refuseAll = false;  // answer every signed request with 401

    std::vector<Bytes> got;      // every client datagram that arrived
    std::vector<Bytes> answers;  // what the server sends back, for the test to hand to the client
    std::vector<std::pair<uint16_t, NetAddress>> binds;
    std::vector<NetAddress> permits;
    std::vector<std::pair<uint16_t, Bytes>> channelData;
    std::vector<std::pair<NetAddress, Bytes>> sendIndications;
    std::vector<uint32_t> refreshLifetimes;

    Server() {
        key = turn::LongTermKey(username, realm, password);
    }

    void Answer(uint16_t type, const turn::TransactionId& txid, int code, bool sign,
                const std::function<void(turn::Builder&)>& fill) {
        turn::Builder b(type, txid);
        if (code != 0) {
            const uint8_t value[4] = {0, 0, static_cast<uint8_t>(code / 100), static_cast<uint8_t>(code % 100)};
            b.Add(turn::kAttrErrorCode, value, sizeof(value));
        }
        if (fill) {
            fill(b);
        }
        answers.push_back(sign ? b.Finish(key.data(), key.size(), true) : b.Finish(nullptr, 0, true));
    }

    void Take(const uint8_t* data, size_t length) {
        if (dropNext > 0) {
            dropNext--;
            return;
        }
        got.emplace_back(data, data + length);
        if (turn::IsChannelData(data, length)) {
            const uint16_t ch = static_cast<uint16_t>((data[0] << 8) | data[1]);
            const size_t n = static_cast<size_t>((data[2] << 8) | data[3]);
            channelData.emplace_back(ch, Bytes(data + 4, data + 4 + n));
            return;
        }
        turn::Parsed p;
        if (!turn::Parse(data, length, p)) {
            return;
        }
        if (p.type == (turn::kSend | turn::kIndication)) {
            NetAddress peer;
            const turn::Attr* d = p.Find(turn::kAttrData);
            if (turn::ReadAddress(data, p, turn::kAttrPeerAddress, peer) && d != nullptr) {
                sendIndications.emplace_back(peer, Bytes(data + d->at, data + d->at + d->length));
            }
            return;
        }
        const uint16_t method = p.type;
        const auto realmNonce = [&](turn::Builder& b) {
            b.AddText(turn::kAttrRealm, realm);
            b.AddText(turn::kAttrNonce, nonce);
        };
        if (!p.hasIntegrity) {
            Answer(static_cast<uint16_t>(method | turn::kError), p.txid, 401, false, realmNonce);
            return;
        }
        if (refuseAll || turn::ReadText(data, p, turn::kAttrUsername) != username ||
            !turn::IntegrityOk(data, p, key.data(), key.size())) {
            Answer(static_cast<uint16_t>(method | turn::kError), p.txid, 401, false, realmNonce);
            return;
        }
        if (staleNext || turn::ReadText(data, p, turn::kAttrNonce) != nonce) {
            staleNext = false;
            nonce += "x";
            Answer(static_cast<uint16_t>(method | turn::kError), p.txid, 438, false, realmNonce);
            return;
        }
        if (method == turn::kAllocate) {
            Answer(static_cast<uint16_t>(method | turn::kSuccess), p.txid, 0, true, [&](turn::Builder& b) {
                b.AddAddress(turn::kAttrRelayedAddress, kRelay);
                b.AddAddress(turn::kAttrMappedAddress, kMapped);
                b.AddU32(turn::kAttrLifetime, lifetime);
            });
        } else if (method == turn::kRefresh) {
            uint32_t asked = 0;
            turn::ReadU32(data, p, turn::kAttrLifetime, asked);
            refreshLifetimes.push_back(asked);
            Answer(static_cast<uint16_t>(method | turn::kSuccess), p.txid, 0, true,
                   [&](turn::Builder& b) { b.AddU32(turn::kAttrLifetime, asked == 0 ? 0 : lifetime); });
        } else if (method == turn::kChannelBind) {
            uint32_t number = 0;
            NetAddress peer;
            turn::ReadU32(data, p, turn::kAttrChannelNumber, number);
            turn::ReadAddress(data, p, turn::kAttrPeerAddress, peer);
            binds.emplace_back(static_cast<uint16_t>(number >> 16), peer);
            if (refuseBind) {
                Answer(static_cast<uint16_t>(method | turn::kError), p.txid, 403, false, nullptr);
            } else {
                Answer(static_cast<uint16_t>(method | turn::kSuccess), p.txid, 0, true, nullptr);
            }
        } else if (method == turn::kCreatePermission) {
            NetAddress peer;
            turn::ReadAddress(data, p, turn::kAttrPeerAddress, peer);
            permits.push_back(peer);
            Answer(static_cast<uint16_t>(method | turn::kSuccess), p.txid, 0, true, nullptr);
        }
    }
};

struct Rig {
    Server server;
    std::vector<std::pair<NetAddress, Bytes>> delivered;
    turn::Client client;
    uint64_t now = 1000;

    Rig()
        : client([this](const uint8_t* d, size_t n) { server.Take(d, n); },
                 [this](const NetAddress& peer, const uint8_t* d, size_t n) { delivered.emplace_back(peer, Bytes(d, d + n)); }) {}

    // Hands the client everything the server answered, until neither has more to say.
    void Flush() {
        for (int guard = 0; guard < 32 && !server.answers.empty(); guard++) {
            std::vector<Bytes> answers;
            answers.swap(server.answers);
            for (const Bytes& a : answers) {
                client.Receive(a.data(), a.size(), now);
            }
        }
    }

    void Advance(uint64_t ms) {
        const uint64_t until = now + ms;
        while (now < until) {
            now = std::min(until, now + 50);
            client.Update(now);
            Flush();
        }
    }

    void Start() {
        turn::TransactionId seed{};
        for (size_t i = 0; i < seed.size(); i++) {
            seed[i] = static_cast<uint8_t>(0x10 + i);
        }
        client.Start(turn::Credentials{server.username, server.password}, seed, now);
        Flush();
    }
};

turn::Parsed ParseOrEmpty(const Bytes& bytes) {
    turn::Parsed p;
    turn::Parse(bytes.data(), bytes.size(), p);
    return p;
}

bool EndsWithFingerprint(const Bytes& bytes) {
    return bytes.size() >= 8 && bytes[bytes.size() - 8] == 0x80 && bytes[bytes.size() - 7] == 0x28;
}

void CheckAllocate() {
    std::printf("client: the 401, the signed Allocate and the relayed address\n");
    Rig rig;
    rig.Start();
    Check(rig.server.got.size() == 2, "two Allocates: one to learn the realm, one signed");
    const turn::Parsed first = ParseOrEmpty(rig.server.got[0]);
    uint32_t transport = 0;
    Check(first.type == turn::kAllocate && !first.hasIntegrity && EndsWithFingerprint(rig.server.got[0]) &&
              turn::ReadU32(rig.server.got[0].data(), first, turn::kAttrRequestedTransport, transport) &&
              transport == 0x11000000u,
          "the first Allocate is unsigned, asks for UDP and carries FINGERPRINT");
    const turn::Parsed second = ParseOrEmpty(rig.server.got[1]);
    Check(second.type == turn::kAllocate && second.hasIntegrity && second.txid != first.txid &&
              turn::ReadText(rig.server.got[1].data(), second, turn::kAttrRealm) == "example.org" &&
              turn::ReadText(rig.server.got[1].data(), second, turn::kAttrNonce) == "nonce-1",
          "the second is signed with the realm and nonce, under a new transaction");
    Check(rig.client.state() == turn::Client::State::Ready, "the client is ready");
    Check(rig.client.relayed() == kRelay && rig.client.mapped() == kMapped && rig.client.lifetimeS() == 600,
          "relayed and mapped addresses and the lifetime are read");
}

void CheckChannels() {
    std::printf("client: ChannelBind, ChannelData both ways, Data indications\n");
    Rig rig;
    rig.client.SetPeers({kPeerA}, rig.now); // before the allocation: bound once it is ready
    rig.Start();
    Check(rig.server.binds.size() == 1 && rig.server.binds[0].first == turn::kFirstChannel &&
              rig.server.binds[0].second == kPeerA,
          "the peer set before the allocation is bound to channel 0x4000");
    Check(rig.client.Bound(kPeerA), "the peer is bound");
    const Bytes pose = Text("pose");
    Check(rig.client.SendTo(kPeerA, pose.data(), pose.size()), "a send to a bound peer goes");
    Check(!rig.server.channelData.empty() && rig.server.channelData.back().first == turn::kFirstChannel &&
              rig.server.channelData.back().second == pose && rig.server.got.back().size() == 4 + pose.size(),
          "it goes as ChannelData, four bytes of overhead");
    Check(!rig.client.SendTo(kPeerB, pose.data(), pose.size()), "a send to an unknown peer does not");

    const Bytes in = Hex("40000003 616263 00");
    Check(rig.client.Receive(in.data(), in.size(), rig.now), "ChannelData is TURN traffic");
    Check(rig.delivered.size() == 1 && rig.delivered[0].first == kPeerA && rig.delivered[0].second == Text("abc"),
          "ChannelData on the peer's channel is handed up as from the peer, padding dropped");
    const Bytes unknownChannel = Hex("40010003 616263 00");
    rig.client.Receive(unknownChannel.data(), unknownChannel.size(), rig.now);
    const Bytes overlong = Hex("40000009 616263 00");
    rig.client.Receive(overlong.data(), overlong.size(), rig.now);
    Check(rig.delivered.size() == 1, "an unknown channel or a length past the datagram is dropped");

    turn::TransactionId txid{};
    txid[0] = 7;
    turn::Builder data(static_cast<uint16_t>(turn::kData | turn::kIndication), txid);
    data.AddAddress(turn::kAttrPeerAddress, kPeerA);
    data.Add(turn::kAttrData, pose.data(), pose.size());
    const Bytes indication = data.Finish(nullptr, 0, false);
    rig.client.Receive(indication.data(), indication.size(), rig.now);
    Check(rig.delivered.size() == 2 && rig.delivered[1].first == kPeerA && rig.delivered[1].second == pose,
          "a Data indication from the peer is handed up");
    turn::Builder stranger(static_cast<uint16_t>(turn::kData | turn::kIndication), txid);
    stranger.AddAddress(turn::kAttrPeerAddress, kPeerB);
    stranger.Add(turn::kAttrData, pose.data(), pose.size());
    const Bytes fromStranger = stranger.Finish(nullptr, 0, false);
    rig.client.Receive(fromStranger.data(), fromStranger.size(), rig.now);
    Check(rig.delivered.size() == 2, "a Data indication from a peer not set is dropped");

    rig.client.SetPeers({kPeerA, kPeerB}, rig.now);
    rig.Flush();
    Check(rig.server.binds.size() == 2 && rig.server.binds[1].first == turn::kFirstChannel + 1 &&
              rig.client.Bound(kPeerB),
          "a second peer gets the next channel");
    rig.client.SetPeers({kPeerB}, rig.now);
    Check(!rig.client.SendTo(kPeerA, pose.data(), pose.size()), "a peer let go is no longer sent to");
    rig.client.Receive(in.data(), in.size(), rig.now);
    Check(rig.delivered.size() == 2, "nor taken from");

    const size_t bindsBefore = rig.server.binds.size();
    rig.Advance(turn::kRebindMs - 100);
    Check(rig.server.binds.size() == bindsBefore, "no rebind before four minutes");
    rig.Advance(200);
    Check(rig.server.binds.size() == bindsBefore + 1 && rig.server.binds.back().first == turn::kFirstChannel + 1 &&
              rig.server.binds.back().second == kPeerB,
          "the channel is rebound on the same number before the permission's five minutes run out");
}

void CheckRefreshAndStaleNonce() {
    std::printf("client: Refresh a minute before the lifetime ends, through a stale nonce\n");
    Rig rig;
    rig.Start();
    rig.Advance(539000);
    Check(rig.server.refreshLifetimes.empty(), "no Refresh before 540 s");
    rig.server.staleNext = true;
    rig.Advance(1100);
    Check(rig.server.refreshLifetimes.size() == 1 && rig.server.refreshLifetimes[0] == 600,
          "after a 438 the Refresh is resent with the new nonce and accepted");
    const turn::Parsed last = ParseOrEmpty(rig.server.got.back());
    Check(turn::ReadText(rig.server.got.back().data(), last, turn::kAttrNonce) == "nonce-1x",
          "the retry carries the server's new nonce");
    Check(rig.client.state() == turn::Client::State::Ready, "the allocation stays");
    rig.Advance(540000);
    Check(rig.server.refreshLifetimes.size() == 2, "and is refreshed again a lifetime later");

    rig.client.Close();
    Check(rig.client.state() == turn::Client::State::Idle, "closing returns to idle");
    const turn::Parsed close = ParseOrEmpty(rig.server.got.back());
    uint32_t lifetime = 99;
    Check(close.type == turn::kRefresh && close.hasIntegrity &&
              turn::ReadU32(rig.server.got.back().data(), close, turn::kAttrLifetime, lifetime) && lifetime == 0,
          "and sends one signed Refresh of lifetime 0");
}

void CheckPermissionFallback() {
    std::printf("client: a refused ChannelBind falls back to CreatePermission and Send indications\n");
    Rig rig;
    rig.server.refuseBind = true;
    rig.Start();
    rig.client.SetPeers({kPeerA}, rig.now);
    rig.Flush();
    Check(rig.server.binds.size() == 1 && rig.server.permits.size() == 1 && rig.server.permits[0] == kPeerA,
          "the 403 to ChannelBind is followed by CreatePermission");
    Check(rig.client.Permitted(kPeerA) && !rig.client.Bound(kPeerA), "the peer is permitted, not bound");
    const Bytes pose = Text("pose");
    Check(rig.client.SendTo(kPeerA, pose.data(), pose.size()) && rig.server.sendIndications.size() == 1 &&
              rig.server.sendIndications[0].first == kPeerA && rig.server.sendIndications[0].second == pose,
          "sends go as Send indications to the peer");
    rig.Advance(turn::kRebindMs + 100);
    Check(rig.server.permits.size() == 2 && rig.server.binds.size() == 1,
          "the permission is renewed, ChannelBind is not tried again");
}

void CheckLossAndForgery() {
    std::printf("client: loss, a forged answer, refused credentials\n");
    {
        Rig rig;
        rig.server.dropNext = 2;
        rig.Start();
        Check(rig.client.state() == turn::Client::State::Allocating, "nothing arrives while the requests are lost");
        rig.Advance(800);
        Check(rig.client.state() == turn::Client::State::Ready, "the retransmission gets through");
    }
    {
        Rig rig;
        rig.server.dropNext = 1000;
        rig.Start();
        rig.Advance(20000);
        Check(rig.server.got.empty() && rig.client.state() == turn::Client::State::Failed &&
                  rig.client.failure() == turn::Failure::Timeout,
              "with every request lost the allocation fails as a timeout");
    }
    {
        Rig rig;
        rig.server.dropNext = 0;
        // Let the 401 through, then forge the success to the signed Allocate under another key.
        turn::TransactionId seed{};
        rig.client.Start(turn::Credentials{rig.server.username, rig.server.password}, seed, rig.now);
        const Bytes unauth = rig.server.answers.at(0);
        rig.server.answers.clear();
        rig.server.dropNext = 1; // the signed Allocate is lost on its way
        rig.client.Receive(unauth.data(), unauth.size(), rig.now);
        const turn::Parsed signedAllocate = ParseOrEmpty(rig.server.got.back());
        Check(signedAllocate.type == turn::kAllocate && !signedAllocate.hasIntegrity, "(the signed Allocate was lost)");
        turn::TransactionId txid = seed;
        txid[11] ^= 1u; // the second transaction of this seed
        turn::Builder forged(static_cast<uint16_t>(turn::kAllocate | turn::kSuccess), txid);
        forged.AddAddress(turn::kAttrRelayedAddress, kPeerB);
        const Bytes wrongKey = Text("not the key");
        const Bytes fake = forged.Finish(wrongKey.data(), wrongKey.size(), true);
        rig.client.Receive(fake.data(), fake.size(), rig.now);
        Check(rig.client.state() == turn::Client::State::Allocating, "a success under another key is ignored");
        turn::Builder unsignedSuccess(static_cast<uint16_t>(turn::kAllocate | turn::kSuccess), txid);
        unsignedSuccess.AddAddress(turn::kAttrRelayedAddress, kPeerB);
        const Bytes fake2 = unsignedSuccess.Finish(nullptr, 0, true);
        rig.client.Receive(fake2.data(), fake2.size(), rig.now);
        Check(rig.client.state() == turn::Client::State::Allocating, "so is one with no MESSAGE-INTEGRITY");
        turn::Builder other(static_cast<uint16_t>(turn::kAllocate | turn::kSuccess), seed);
        other.AddAddress(turn::kAttrRelayedAddress, kPeerB);
        const Bytes fake3 = other.Finish(rig.server.key.data(), rig.server.key.size(), true);
        rig.client.Receive(fake3.data(), fake3.size(), rig.now);
        Check(rig.client.state() == turn::Client::State::Allocating, "and one for a transaction already answered");
        rig.Advance(300);
        Check(rig.client.state() == turn::Client::State::Ready && rig.client.relayed() == kRelay,
              "the retransmitted request gets the real answer");
    }
    {
        Rig rig;
        rig.server.refuseAll = true;
        rig.Start();
        Check(rig.client.state() == turn::Client::State::Failed && rig.client.failure() == turn::Failure::Unauthorized,
              "a 401 to the signed Allocate fails as refused credentials");
        rig.client.Update(rig.now + 60000);
        Check(rig.server.got.size() == 2, "and sends nothing more");
    }
    {
        Rig rig;
        rig.Start();
        rig.server.refuseAll = true;
        rig.Advance(541000);
        Check(rig.client.state() == turn::Client::State::Failed && rig.client.failure() == turn::Failure::Unauthorized,
              "expired credentials end the allocation at its Refresh");
        const Bytes pose = Text("pose");
        Check(!rig.client.SendTo(kRelay, pose.data(), pose.size()), "a failed client sends nothing");
    }
}

// Changes bytes of real answers at random and hands them to the client: it must never crash, and never become
// ready on a success whose signed part was changed.
void Fuzz(int rounds) {
    std::printf("client: %d changed answers\n", rounds);
    uint32_t state = 12345;
    const auto next = [&]() {
        state = state * 1103515245u + 12345u;
        return state >> 8;
    };
    for (int round = 0; round < rounds; round++) {
        Rig rig;
        turn::TransactionId seed{};
        rig.client.Start(turn::Credentials{rig.server.username, rig.server.password}, seed, rig.now);
        Bytes answer = rig.server.answers.at(0);
        rig.server.answers.clear();
        rig.client.Receive(answer.data(), answer.size(), rig.now);
        if (rig.server.answers.empty()) {
            continue;
        }
        const Bytes original = rig.server.answers.at(0);
        Bytes success = original;
        rig.server.answers.clear();
        const int flips = 1 + static_cast<int>(next() % 3);
        for (int f = 0; f < flips; f++) {
            success[next() % success.size()] ^= static_cast<uint8_t>(1u << (next() % 8));
        }
        if (next() % 4 == 0) {
            success.resize(next() % (success.size() + 1));
        }
        rig.client.Receive(success.data(), success.size(), rig.now);
        // Only the FINGERPRINT follows MESSAGE-INTEGRITY. A change there that leaves an unknown attribute after
        // the integrity is ignored (RFC 8489 section 14.5), so the client may be ready: but only on the signed
        // bytes as the server wrote them.
        const size_t signedEnd = original.size() - 8;
        if (rig.client.state() == turn::Client::State::Ready &&
            (success.size() < signedEnd || !std::equal(original.begin(), original.begin() + signedEnd, success.begin()) ||
             rig.client.relayed() != kRelay)) {
            Check(false, "a change to the signed part of a success made the client ready");
        }
    }
    Check(true, "no crash on changed answers");
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 3000;
    CheckRfc5769();
    CheckAllocate();
    CheckChannels();
    CheckRefreshAndStaleNonce();
    CheckPermissionFallback();
    CheckLossAndForgery();
    Fuzz(rounds > 0 ? rounds : 3000);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
