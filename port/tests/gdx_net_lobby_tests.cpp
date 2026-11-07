// port/tests/gdx_net_lobby_tests.cpp: the O2 LAN lobby wire format, chat filter, host, client and room
// browser (port/net/gdx_net_lobby*.h).
//
// The session tests run every peer in this process on a fake network and a fake clock: datagrams go
// into a queue and arrive on the next Step, a broadcast reaches every endpoint on its port, and a test can
// cut an endpoint off to play a peer that vanished. No socket is opened.

#include "../net/gdx_net_lobby.h"
#include "../net/gdx_net_lobby_session.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace gdx::net;
using namespace gdx::net::lobby;

int gChecks = 0;
int gFails = 0;

#define CHECK(cond)                                                 \
    do {                                                            \
        gChecks++;                                                  \
        if (!(cond)) {                                              \
            gFails++;                                               \
            std::printf("    [x] line %d: %s\n", __LINE__, #cond); \
        }                                                           \
    } while (0)

const std::string kBuild = "1.1.0/abc123";

uint32_t Ip(int a, int b, int c, int d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

NetAddress Addr(uint32_t ip, uint16_t port) {
    NetAddress a;
    a.ip = ip;
    a.port = port;
    return a;
}

// ---------------------------------------------------------------------------------------------------------
// Fake network

class FakeNet {
public:
    using Deliver = std::function<void(const NetAddress& from, const uint8_t* data, size_t len)>;

    void Add(const NetAddress& addr, Deliver deliver) {
        endpoints_.push_back({ addr, std::move(deliver), true });
    }

    void Cut(const NetAddress& addr) {
        for (Endpoint& e : endpoints_) {
            if (e.addr == addr) {
                e.alive = false;
            }
        }
    }

    DatagramSend SenderFor(const NetAddress& from) {
        return [this, from](const NetAddress& to, const uint8_t* data, size_t len) {
            if (!Alive(from)) {
                return;
            }
            queue_.push_back({ from, to, std::vector<uint8_t>(data, data + len) });
            sent_++;
        };
    }

    // Delivers what is queued now; anything sent during delivery waits for the next Step.
    void Step() {
        std::vector<Datagram> now;
        now.swap(queue_);
        for (const Datagram& d : now) {
            const bool broadcast = d.to.ip == 0xFFFFFFFFu;
            for (Endpoint& e : endpoints_) {
                if (!e.alive || e.addr.port != d.to.port || (!broadcast && e.addr.ip != d.to.ip)) {
                    continue;
                }
                if (e.addr == d.from) {
                    continue;
                }
                e.deliver(d.from, d.bytes.data(), d.bytes.size());
            }
        }
    }

    size_t Sent() const {
        return sent_;
    }

private:
    struct Endpoint {
        NetAddress addr;
        Deliver deliver;
        bool alive;
    };

    struct Datagram {
        NetAddress from;
        NetAddress to;
        std::vector<uint8_t> bytes;
    };

    bool Alive(const NetAddress& addr) const {
        for (const Endpoint& e : endpoints_) {
            if (e.addr == addr) {
                return e.alive;
            }
        }
        return true;
    }

    std::vector<Endpoint> endpoints_;
    std::vector<Datagram> queue_;
    size_t sent_ = 0;
};

LocalProfile Profile(const std::string& name, uint32_t colour, uint8_t machine) {
    LocalProfile p;
    p.name = name;
    p.colour = colour;
    p.machine = machine;
    return p;
}

// One host and any number of clients on the fake network, ticked together every 16 ms.
struct Room {
    FakeNet net;
    uint32_t now = 1000;
    NetAddress hostAddr = Addr(Ip(192, 168, 1, 10), kDefaultPort);
    std::unique_ptr<LobbyHost> host;
    std::vector<std::unique_ptr<LobbyClient>> clients;
    std::vector<NetAddress> clientAddrs;
    std::vector<std::unique_ptr<LanBrowser>> browsers;

    explicit Room(const std::string& hostName = "Host") {
        host = std::make_unique<LobbyHost>(kBuild, Profile(hostName, 0xFF0000, 0), kDefaultPort,
                                           net.SenderFor(hostAddr));
        net.Add(hostAddr,
                [this](const NetAddress& from, const uint8_t* d, size_t n) { host->Receive(from, d, n, now); });
    }

    LobbyClient& AddClient(const std::string& name, const std::string& build = kBuild, uint32_t colour = 0x00FF00) {
        const size_t index = clients.size();
        const NetAddress addr =
            Addr(Ip(192, 168, 1, static_cast<int>(20 + index)), static_cast<uint16_t>(50000 + index));
        clients.push_back(std::make_unique<LobbyClient>(build, Profile(name, colour, 3), hostAddr,
                                                        0x1000u + static_cast<uint32_t>(index), net.SenderFor(addr)));
        clientAddrs.push_back(addr);
        net.Add(addr, [this, index](const NetAddress& from, const uint8_t* d, size_t n) {
            clients[index]->Receive(from, d, n, now);
        });
        clients.back()->Start(now);
        return *clients.back();
    }

    void Tick(int frames = 1) {
        for (int i = 0; i < frames; i++) {
            now += 16;
            net.Step();
            host->Update(now);
            for (auto& c : clients) {
                c->Update(now);
            }
            for (auto& b : browsers) {
                b->Update(now);
            }
        }
    }

    // Runs until the clients have settled: 1.5 s of frames covers the handshake, join and roster.
    void Settle() {
        Tick(90);
    }
};

const PlayerInfo* FindIn(const std::vector<PlayerInfo>& roster, uint8_t id) {
    for (const PlayerInfo& p : roster) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool LastLineIs(const std::deque<ShownLine>& log, uint8_t from, const std::string& text) {
    return !log.empty() && log.back().from == from && log.back().text == text;
}

bool HasLine(const std::deque<ShownLine>& log, const std::string& text) {
    for (const ShownLine& l : log) {
        if (l.text == text) {
            return true;
        }
    }
    return false;
}

// xorshift32, for the decoder sweep.
uint32_t Next(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ---------------------------------------------------------------------------------------------------------
// Wire format

// Every prefix shorter than the message, and the message with one byte added, must be refused.
template <typename Decode> void CheckEdges(const std::vector<uint8_t>& bytes, Decode decode) {
    for (size_t n = 0; n < bytes.size(); n++) {
        CHECK(!decode(bytes.data(), n));
    }
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK(!decode(longer.data(), longer.size()));
    CHECK(decode(bytes.data(), bytes.size()));
}

void TestCodec() {
    std::printf("codec\n");
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    join.colour = 0x123456;
    join.machine = 29;
    const std::vector<uint8_t> joinBytes = EncodeJoin(join);
    JoinRequest joinOut;
    CHECK(DecodeJoin(joinBytes.data(), joinBytes.size(), joinOut));
    CHECK(joinOut.build == kBuild && joinOut.name == "Ana" && joinOut.colour == 0x123456 && joinOut.machine == 29);
    CheckEdges(joinBytes, [](const uint8_t* d, size_t n) {
        JoinRequest o;
        return DecodeJoin(d, n, o);
    });
    const auto refusesJoin = [](JoinRequest j) {
        const std::vector<uint8_t> b = EncodeJoin(j);
        JoinRequest o;
        o.name = "unchanged";
        return !DecodeJoin(b.data(), b.size(), o) && o.name == "unchanged";
    };
    JoinRequest bad = join;
    bad.name = " Ana";
    CHECK(refusesJoin(bad));
    bad = join;
    bad.name = "";
    CHECK(refusesJoin(bad));
    bad = join;
    bad.name = "Seventeen chars!!";
    CHECK(refusesJoin(bad));
    bad = join;
    bad.name = "Tab\there";
    CHECK(refusesJoin(bad));
    bad = join;
    bad.machine = 30;
    CHECK(refusesJoin(bad));
    bad = join;
    bad.colour = 0x1000000;
    CHECK(refusesJoin(bad));
    bad = join;
    bad.build = "";
    CHECK(refusesJoin(bad));
    bad = join;
    bad.build = std::string(65, 'b');
    CHECK(refusesJoin(bad));
    bad = join;
    bad.machine = 0xFF;
    const std::vector<uint8_t> noMachine = EncodeJoin(bad);
    CHECK(DecodeJoin(noMachine.data(), noMachine.size(), joinOut) && joinOut.machine == 0xFF);

    uint8_t id = 0;
    std::vector<uint8_t> b = EncodeWelcome(3);
    CHECK(DecodeWelcome(b.data(), b.size(), id) && id == 3);
    b = EncodeWelcome(0);
    CHECK(!DecodeWelcome(b.data(), b.size(), id));
    b = EncodeWelcome(8);
    CHECK(!DecodeWelcome(b.data(), b.size(), id));

    RejectReason reason = RejectReason::Full;
    b = EncodeReject(RejectReason::Kicked);
    CHECK(DecodeReject(b.data(), b.size(), reason) && reason == RejectReason::Kicked);
    const uint8_t zero = 0;
    const uint8_t five = 5;
    CHECK(!DecodeReject(&zero, 1, reason) && !DecodeReject(&five, 1, reason));

    bool ready = false;
    b = EncodeReady(true);
    CHECK(DecodeReady(b.data(), b.size(), ready) && ready);
    const uint8_t two = 2;
    CHECK(!DecodeReady(&two, 1, ready));

    std::vector<PlayerInfo> roster(3);
    roster[0].id = 0;
    roster[0].host = true;
    roster[0].name = "Host";
    roster[0].colour = 0xFF0000;
    roster[0].machine = 0;
    roster[1].id = 1;
    roster[1].ready = true;
    roster[1].name = "Ana";
    roster[2].id = 5;
    roster[2].name = "Bo";
    roster[2].machine = 0xFF;
    const std::vector<uint8_t> rosterBytes = EncodeRoster(roster);
    std::vector<PlayerInfo> rosterOut;
    CHECK(DecodeRoster(rosterBytes.data(), rosterBytes.size(), rosterOut) && rosterOut == roster);
    CheckEdges(rosterBytes, [](const uint8_t* d, size_t n) {
        std::vector<PlayerInfo> o;
        return DecodeRoster(d, n, o);
    });
    const auto refusesRoster = [](const std::vector<PlayerInfo>& r) {
        const std::vector<uint8_t> e = EncodeRoster(r);
        std::vector<PlayerInfo> o;
        return !DecodeRoster(e.data(), e.size(), o) && o.empty();
    };
    std::vector<PlayerInfo> badRoster = roster;
    badRoster[2].id = 1;
    CHECK(refusesRoster(badRoster)); // duplicate id
    badRoster = roster;
    badRoster[1].host = true;
    CHECK(refusesRoster(badRoster)); // two hosts
    badRoster = roster;
    badRoster[0].host = false;
    CHECK(refusesRoster(badRoster)); // no host
    badRoster = roster;
    badRoster[2].id = 8;
    CHECK(refusesRoster(badRoster)); // id out of range
    CHECK(refusesRoster({}));         // empty
    badRoster.assign(9, roster[1]);
    for (size_t i = 0; i < badRoster.size(); i++) {
        badRoster[i].id = static_cast<uint8_t>(i);
        badRoster[i].host = i == 0;
    }
    CHECK(refusesRoster(badRoster)); // nine players
    std::vector<uint8_t> flagged = rosterBytes;
    flagged[2] |= 0x80; // first entry's flags byte
    CHECK(!DecodeRoster(flagged.data(), flagged.size(), rosterOut));

    std::string text;
    b = EncodeChat("hello there");
    CHECK(DecodeChat(b.data(), b.size(), text) && text == "hello there");
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        std::string o;
        return DecodeChat(d, n, o);
    });
    b = EncodeChat(" padded");
    CHECK(!DecodeChat(b.data(), b.size(), text));
    b = EncodeChat("");
    CHECK(!DecodeChat(b.data(), b.size(), text));
    b = EncodeChat(std::string(121, 'x'));
    CHECK(!DecodeChat(b.data(), b.size(), text));
    b = EncodeChat(std::string(120, 'x'));
    CHECK(DecodeChat(b.data(), b.size(), text) && text.size() == 120);
    b = EncodeChat("two  spaces");
    CHECK(!DecodeChat(b.data(), b.size(), text));

    ChatLine line;
    line.from = kSystemId;
    line.text = "Ana joined the room.";
    b = EncodeChatLine(line);
    ChatLine lineOut;
    CHECK(DecodeChatLine(b.data(), b.size(), lineOut) && lineOut.from == kSystemId && lineOut.text == line.text);
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        ChatLine o;
        return DecodeChatLine(d, n, o);
    });
    line.from = 8;
    b = EncodeChatLine(line);
    CHECK(!DecodeChatLine(b.data(), b.size(), lineOut));
}

void TestDiscoveryCodec() {
    std::printf("discovery codec\n");
    const std::vector<uint8_t> probe = EncodeProbe(0xDEADBEEF);
    uint32_t nonce = 0;
    CHECK(IsDiscovery(probe.data(), probe.size()));
    CHECK(DecodeProbe(probe.data(), probe.size(), nonce) && nonce == 0xDEADBEEF);
    CheckEdges(probe, [](const uint8_t* d, size_t n) {
        uint32_t o = 0;
        return DecodeProbe(d, n, o);
    });

    RoomInfo room;
    room.port = 24302;
    room.players = 3;
    room.cap = 8;
    room.open = true;
    room.build = kBuild;
    room.hostName = "Host";
    const std::vector<uint8_t> reply = EncodeRoom(7, room);
    RoomInfo out;
    CHECK(DecodeRoom(reply.data(), reply.size(), nonce, out) && nonce == 7 && out.port == 24302 &&
          out.players == 3 && out.cap == 8 && out.open && out.build == kBuild && out.hostName == "Host");
    CheckEdges(reply, [](const uint8_t* d, size_t n) {
        uint32_t o = 0;
        RoomInfo r;
        return DecodeRoom(d, n, o, r);
    });
    CHECK(!DecodeProbe(reply.data(), reply.size(), nonce)); // a reply is not a probe
    CHECK(!DecodeRoom(probe.data(), probe.size(), nonce, out));
    const auto refuses = [](RoomInfo r) {
        const std::vector<uint8_t> e = EncodeRoom(1, r);
        uint32_t n = 0;
        RoomInfo o;
        return !DecodeRoom(e.data(), e.size(), n, o);
    };
    RoomInfo bad = room;
    bad.port = 0;
    CHECK(refuses(bad));
    bad = room;
    bad.cap = 9;
    CHECK(refuses(bad));
    bad = room;
    bad.players = 9;
    CHECK(refuses(bad));
    bad = room;
    bad.players = 0;
    CHECK(refuses(bad));
    bad = room;
    bad.hostName = "";
    CHECK(refuses(bad));

    // Channel packets ("GD" + type 0 to 4) are never mistaken for discovery.
    const uint8_t hello[] = { 'G', 'D', 0, 1, 2, 3, 4, 1, 0 };
    CHECK(!IsDiscovery(hello, sizeof(hello)));
}

void TestSanitizeAndFilter() {
    std::printf("sanitize and filter\n");
    CHECK(SanitizeChat("  hi\tthere\n friend  ") == "hi there friend");
    CHECK(SanitizeChat("\x01\x7f\x80\xff") == "");
    CHECK(SanitizeChat("caf\xc3\xa9 ok") == "caf ok");
    CHECK(SanitizeChat(std::string(300, 'a')).size() == kChatMax);
    CHECK(SanitizeChat(std::string(119, 'a') + "  b").size() <= kChatMax);
    CHECK(SanitizeChat(SanitizeChat("a  b\t\tc")) == SanitizeChat("a  b\t\tc"));

    const std::vector<std::string> none;
    CHECK(FilterChat("what the fuck", none) == "what the ****");
    CHECK(FilterChat("motherfucker!", none) == "************!");
    CHECK(FilterChat("BULLSHIT", none) == "********");
    CHECK(FilterChat("sh1tty 5h1t", none) == "****** ****");
    CHECK(FilterChat("Scunthorpe class assassin", none) == "Scunthorpe class assassin");
    CHECK(FilterChat("you ass", none) == "you ***");
    CHECK(FilterChat("cocktail dickens", none) == "cocktail dickens");
    CHECK(FilterChat("good race!", none) == "good race!");
    CHECK(FilterChat("", none).empty());

    const std::vector<std::string> mine = { "noob", "*bad", "lag|", "", "a b", "x!" };
    CHECK(FilterChat("noobs are bad", mine) == "***** are ***");
    CHECK(FilterChat("superbad lagging lag", mine) == "******** lagging ***");
    CHECK(FilterChat("a b x!", mine) == "a b x!");
}

void TestDecoderSweep() {
    std::printf("decoder sweep\n");
    // Random bytes and random single-byte damage to valid messages: every decoder either refuses or returns a
    // value that passes its own rules again. Nothing may crash.
    uint32_t seed = 0x51ED;
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    std::vector<PlayerInfo> roster(2);
    roster[0].host = true;
    roster[0].name = "Host";
    roster[1].id = 1;
    roster[1].name = "Ana";
    const RoomInfo seedRoom{ 1, 1, 8, true, "b", "H" };
    const std::vector<std::vector<uint8_t>> seeds = { EncodeJoin(join), EncodeRoster(roster), EncodeChat("hi"),
                                                      EncodeProbe(1), EncodeRoom(1, seedRoom) };
    int accepted = 0;
    for (int round = 0; round < 20000; round++) {
        std::vector<uint8_t> bytes;
        if (round % 2 == 0) {
            bytes.resize(Next(seed) % 48);
            for (uint8_t& v : bytes) {
                v = static_cast<uint8_t>(Next(seed));
            }
        } else {
            bytes = seeds[Next(seed) % seeds.size()];
            if (!bytes.empty()) {
                bytes[Next(seed) % bytes.size()] = static_cast<uint8_t>(Next(seed));
            }
        }
        JoinRequest j;
        if (DecodeJoin(bytes.data(), bytes.size(), j)) {
            accepted++;
            CHECK(NameOk(j.name) && MachineOk(j.machine) && ColourOk(j.colour));
        }
        std::vector<PlayerInfo> r;
        if (DecodeRoster(bytes.data(), bytes.size(), r)) {
            accepted++;
            CHECK(!r.empty() && r.size() <= kMaxPlayers);
            const std::vector<uint8_t> again = EncodeRoster(r);
            CHECK(again == bytes);
        }
        std::string text;
        if (DecodeChat(bytes.data(), bytes.size(), text)) {
            CHECK(SanitizeChat(text) == text && !text.empty());
        }
        ChatLine line;
        (void) DecodeChatLine(bytes.data(), bytes.size(), line);
        uint32_t nonce = 0;
        RoomInfo room;
        if (DecodeRoom(bytes.data(), bytes.size(), nonce, room)) {
            CHECK(room.players <= room.cap && room.cap <= kMaxPlayers && NameOk(room.hostName));
        }
        (void) DecodeProbe(bytes.data(), bytes.size(), nonce);
    }
    CHECK(accepted > 0); // some damaged copies stay valid, so the checks above ran
}

// ---------------------------------------------------------------------------------------------------------
// Sessions

void TestJoinReadyChat() {
    std::printf("join, ready and chat\n");
    Room room;
    LobbyClient& ana = room.AddClient("Ana");
    room.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined);
    CHECK(ana.LocalId() == 1);
    CHECK(room.host->Roster().size() == 2 && ana.Roster().size() == 2);
    CHECK(ana.Roster() == room.host->Roster());
    const PlayerInfo* seen = FindIn(ana.Roster(), 1);
    CHECK(seen != nullptr && seen->name == "Ana" && seen->colour == 0x00FF00 && seen->machine == 3 && !seen->ready);
    CHECK(HasLine(room.host->ChatLog(), "Ana joined the room."));
    CHECK(HasLine(ana.ChatLog(), "Ana joined the room."));

    ana.SetReady(true);
    room.Tick(20);
    CHECK(FindIn(room.host->Roster(), 1)->ready);
    CHECK(FindIn(ana.Roster(), 1)->ready);
    room.host->SetReady(true);
    room.Tick(20);
    CHECK(FindIn(ana.Roster(), 0)->ready);

    CHECK(ana.Chat("  good   luck\t"));
    room.Tick(20);
    CHECK(LastLineIs(room.host->ChatLog(), 1, "good luck"));
    CHECK(LastLineIs(ana.ChatLog(), 1, "good luck"));
    CHECK(ana.ChatLog().back().name == "Ana" && ana.ChatLog().back().colour == 0x00FF00);
    CHECK(!ana.Chat(" \t\x01 "));
    CHECK(room.host->Chat("welcome"));
    room.Tick(20);
    CHECK(LastLineIs(ana.ChatLog(), 0, "welcome") && ana.ChatLog().back().name == "Host");

    LobbyClient& bo = room.AddClient("Bo", kBuild, 0x0000FF);
    room.Settle();
    CHECK(bo.GetState() == LobbyClient::State::Joined && bo.LocalId() == 2);
    CHECK(room.host->Roster().size() == 3 && ana.Roster().size() == 3 && bo.Roster().size() == 3);
    CHECK(bo.Roster() == room.host->Roster());
    CHECK(HasLine(ana.ChatLog(), "Bo joined the room."));
    CHECK(!HasLine(bo.ChatLog(), "good luck")); // no history for late joiners
    CHECK(room.host->Refused() == 0 && ana.Refused() == 0 && bo.Refused() == 0);
}

void TestChatRateLimit() {
    std::printf("chat rate limit\n");
    Room room;
    LobbyClient& ana = room.AddClient("Ana");
    LobbyClient& bo = room.AddClient("Bo");
    room.Settle();
    for (int i = 0; i < 7; i++) {
        CHECK(ana.Chat("spam " + std::to_string(i)));
    }
    room.Tick(30);
    CHECK(HasLine(bo.ChatLog(), "spam 4"));
    CHECK(!HasLine(bo.ChatLog(), "spam 5") && !HasLine(bo.ChatLog(), "spam 6"));
    CHECK(HasLine(ana.ChatLog(), "You are sending messages too fast. Wait a moment."));
    CHECK(!HasLine(bo.ChatLog(), "You are sending messages too fast. Wait a moment."));
    room.Tick(320); // past the 5 s window
    CHECK(ana.Chat("again"));
    room.Tick(20);
    CHECK(LastLineIs(bo.ChatLog(), 1, "again"));
}

void TestRejects() {
    std::printf("rejects\n");
    Room room;
    LobbyClient& old = room.AddClient("Old", "1.0.9/zzz");
    room.Settle();
    CHECK(old.GetState() == LobbyClient::State::Closed);
    CHECK(old.CloseText() == RejectReasonText(RejectReason::Build));
    CHECK(room.host->Roster().size() == 1);
    room.Tick(40);
    CHECK(room.host->PeerCount() == 0);

    for (int i = 0; i < 7; i++) {
        room.AddClient("P" + std::to_string(i + 1));
    }
    room.Settle();
    CHECK(room.host->Roster().size() == kMaxPlayers);
    for (size_t i = 1; i < room.clients.size(); i++) {
        CHECK(room.clients[i]->GetState() == LobbyClient::State::Joined);
    }
    LobbyClient& late = room.AddClient("Late");
    room.Settle();
    CHECK(late.GetState() == LobbyClient::State::Closed);
    CHECK(late.CloseText() == RejectReasonText(RejectReason::Full));
    CHECK(room.host->Roster().size() == kMaxPlayers);

    // Kick: the player hears why, everyone else sees the seat free up.
    LobbyClient& p3 = *room.clients[3];
    const uint8_t p3Id = p3.LocalId();
    room.host->Kick(p3Id, room.now);
    room.Tick(60);
    CHECK(p3.GetState() == LobbyClient::State::Closed);
    CHECK(p3.CloseText() == RejectReasonText(RejectReason::Kicked));
    CHECK(FindIn(room.host->Roster(), p3Id) == nullptr);
    CHECK(FindIn(room.clients[1]->Roster(), p3Id) == nullptr);
    CHECK(HasLine(room.clients[1]->ChatLog(), "P3 left the room."));
    room.host->Kick(kHostId, room.now); // the host cannot kick itself
    room.Tick(5);
    CHECK(FindIn(room.host->Roster(), kHostId) != nullptr);

    // The freed id is reused by the next joiner.
    LobbyClient& again = room.AddClient("Again");
    room.Settle();
    CHECK(again.GetState() == LobbyClient::State::Joined && again.LocalId() == p3Id);
}

void TestLeaveVanishAndClose() {
    std::printf("leave, vanish and close\n");
    Room room;
    LobbyClient& ana = room.AddClient("Ana");
    LobbyClient& bo = room.AddClient("Bo");
    LobbyClient& cy = room.AddClient("Cy");
    room.Settle();
    CHECK(room.host->Roster().size() == 4);

    ana.Leave(room.now);
    room.Tick(20);
    CHECK(ana.GetState() == LobbyClient::State::Closed && ana.CloseText().empty());
    CHECK(room.host->Roster().size() == 3 && bo.Roster().size() == 3);
    CHECK(HasLine(bo.ChatLog(), "Ana left the room."));

    // Bo's machine drops off the network: the host notices after the channel's 10 s silence timeout.
    room.net.Cut(room.clientAddrs[1]);
    room.Tick(300);
    CHECK(room.host->Roster().size() == 3);
    room.Tick(400);
    CHECK(room.host->Roster().size() == 2 && cy.Roster().size() == 2);
    CHECK(HasLine(cy.ChatLog(), "Bo left the room."));

    room.host->Close(room.now);
    room.Tick(20);
    CHECK(cy.GetState() == LobbyClient::State::Closed);
    CHECK(cy.CloseText() == "The host closed the room.");
    CHECK(room.host->PeerCount() == 0);
}

void TestNoHost() {
    std::printf("no host\n");
    FakeNet net;
    uint32_t now = 5000;
    const NetAddress me = Addr(Ip(10, 0, 0, 2), 40000);
    LobbyClient client(kBuild, Profile("Ana", 1, 0xFF), Addr(Ip(10, 0, 0, 99), kDefaultPort), 42, net.SenderFor(me));
    net.Add(me, [&](const NetAddress& from, const uint8_t* d, size_t n) { client.Receive(from, d, n, now); });
    client.Start(now);
    for (int i = 0; i < 700; i++) {
        now += 16;
        net.Step();
        client.Update(now);
    }
    CHECK(client.GetState() == LobbyClient::State::Closed);
    CHECK(client.CloseText() == "No room answered at 10.0.0.99:24302.");
}

void TestHostIgnoresStrangers() {
    std::printf("host ignores strangers\n");
    Room room;
    const uint8_t junk[] = { 1, 2, 3, 4, 5 };
    room.host->Receive(Addr(Ip(10, 1, 1, 1), 1), junk, sizeof(junk), room.now);
    CHECK(room.host->PeerCount() == 0 && room.host->Refused() == 1);
    // A flood of hellos from many addresses holds at most kPendingPeerCap channels, and they expire.
    const uint8_t hello[] = { 'G', 'D', 0, 0x10, 0, 0, 0, 1, 0 };
    for (int i = 0; i < 40; i++) {
        room.host->Receive(Addr(Ip(10, 2, 0, i), 9999), hello, sizeof(hello), room.now);
    }
    CHECK(room.host->PeerCount() == kPendingPeerCap);
    room.Tick(400);
    CHECK(room.host->PeerCount() == 0);
    CHECK(room.host->Roster().size() == 1);
    // A probe gets one room reply back.
    const size_t before = room.net.Sent();
    const std::vector<uint8_t> probe = EncodeProbe(9);
    room.host->Receive(Addr(Ip(10, 3, 0, 1), 7), probe.data(), probe.size(), room.now);
    CHECK(room.net.Sent() == before + 1);
}

void TestBrowser() {
    std::printf("browser\n");
    Room room("LanHost");
    room.AddClient("Ana");
    room.Settle();
    const NetAddress me = Addr(Ip(192, 168, 1, 77), 41000);
    const std::vector<NetAddress> targets = { Addr(0xFFFFFFFFu, kDefaultPort), Addr(Ip(127, 0, 0, 1), kDefaultPort) };
    room.browsers.push_back(std::make_unique<LanBrowser>(targets, 0xABCD, room.net.SenderFor(me)));
    LanBrowser& browser = *room.browsers.back();
    room.net.Add(me,
                 [&](const NetAddress& from, const uint8_t* d, size_t n) { browser.Receive(from, d, n, room.now); });
    room.Tick(10);
    CHECK(browser.Rooms().size() == 1);
    if (!browser.Rooms().empty()) {
        const LanBrowser::FoundRoom& found = browser.Rooms()[0];
        CHECK(found.address == room.hostAddr);
        CHECK(found.room.hostName == "LanHost" && found.room.players == 2 && found.room.cap == kMaxPlayers &&
              found.room.open && found.room.build == kBuild);
    }
    // A reply for another browser's probe, and noise, are not rooms.
    RoomInfo other;
    other.port = 1;
    other.players = 1;
    other.cap = 8;
    other.build = "b";
    other.hostName = "Other";
    const std::vector<uint8_t> foreign = EncodeRoom(0x1111, other);
    CHECK(!browser.Receive(Addr(Ip(1, 2, 3, 4), 5), foreign.data(), foreign.size(), room.now));
    const uint8_t junk[] = { 'G', 'D', 'X', 'L', 2 };
    CHECK(!browser.Receive(Addr(Ip(1, 2, 3, 4), 5), junk, sizeof(junk), room.now));
    CHECK(browser.Rooms().size() == 1);
    // The host goes away: the room drops off the list after kRoomExpireMs.
    room.net.Cut(room.hostAddr);
    room.Tick(200);
    CHECK(browser.Rooms().size() == 1);
    room.Tick(200);
    CHECK(browser.Rooms().empty());
}

} // namespace

int main() {
    TestCodec();
    TestDiscoveryCodec();
    TestSanitizeAndFilter();
    TestDecoderSweep();
    TestJoinReadyChat();
    TestChatRateLimit();
    TestRejects();
    TestLeaveVanishAndClose();
    TestNoHost();
    TestHostIgnoresStrangers();
    TestBrowser();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
