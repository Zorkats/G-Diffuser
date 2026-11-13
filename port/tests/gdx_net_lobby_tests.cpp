// port/tests/gdx_net_lobby_tests.cpp: the LAN room wire format (version 2), the chat filter, and the host,
// client and room browser sessions (port/net/gdx_net_lobby*.h).
//
// The session tests run every peer in this process on a fake network and a fake clock: datagrams go
// into a queue and arrive on the next Step, a broadcast reaches every endpoint on its port, and a test can
// cut an endpoint off to play a peer that vanished. No socket is opened. The room's own rules are tested
// without a network in gdx_net_room_tests.cpp; here they are followed from the host to the clients.

#include "../net/gdx_net_channel.h"
#include "../net/gdx_net_friends.h"
#include "../net/gdx_net_lobby.h"
#include "../net/gdx_net_lobby_session.h"
#include "../net/gdx_net_track_pack.h"

#include "gdx_course_branch_format.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
const char* const kTooFast = "You are sending messages too fast. Wait a moment.";

uint32_t Ip(int a, int b, int c, int d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

NetAddress Addr(uint32_t ip, uint16_t port) {
    return NetAddress::FromV4(ip, port);
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
            if (!Alive(from) || (filter_ && filter_(from, to, len))) {
                return;
            }
            const NetAddress source = source_ ? source_(from, to) : from;
            const int wait = delay_ ? delay_(from, to) : 0;
            queue_.push_back({ source, to, std::vector<uint8_t>(data, data + len), wait });
            sent_++;
        };
    }

    // Delivers what is due now; anything sent during delivery waits for the next Step.
    void Step() {
        std::vector<Datagram> now;
        now.swap(queue_);
        std::vector<Datagram> later;
        for (Datagram& d : now) {
            if (d.wait > 0) {
                d.wait--;
                later.push_back(std::move(d));
                continue;
            }
            const bool broadcast = d.to.Ipv4() == 0xFFFFFFFFu;
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
        later.insert(later.end(), queue_.begin(), queue_.end());
        queue_.swap(later);
    }

    size_t Sent() const {
        return sent_;
    }

    // Asked about every datagram as it is sent: a test counts with it, or loses the datagram by saying true.
    using Filter = std::function<bool(const NetAddress& from, const NetAddress& to, size_t len)>;

    void SetFilter(Filter filter) {
        filter_ = std::move(filter);
    }

    // The Steps a datagram waits before it is delivered: 0 is the next Step.
    using Delay = std::function<int(const NetAddress& from, const NetAddress& to)>;
    void SetDelay(Delay delay) {
        delay_ = std::move(delay);
    }

    // The source address a datagram arrives from: a copy on a LAN reaches a neighbour from its LAN address.
    using Source = std::function<NetAddress(const NetAddress& from, const NetAddress& to)>;
    void SetSource(Source source) {
        source_ = std::move(source);
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
        int wait = 0;
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
    Filter filter_;
    Delay delay_;
    Source source_;
};

LocalProfile Profile(const std::string& name, uint32_t colour, uint8_t machine) {
    LocalProfile p;
    p.name = name;
    p.colour = colour;
    p.machine = machine;
    return p;
}

RoomConfig ConfigFor(uint32_t roomId) {
    RoomConfig config;
    config.roomId = roomId;
    return config;
}

// One host and any number of clients on the fake network, ticked together every 16 ms. A test can destroy
// the host (DropHost) and let the clients follow their handover plans (FollowPlans): the successor's host then
// runs on that client's address as `next`, and each member that comes back replaces its client object.
struct Lan {
    FakeNet net;
    uint32_t now = 1000;
    NetAddress hostAddr = Addr(Ip(192, 168, 1, 10), kDefaultPort);
    std::unique_ptr<LobbyHost> host;
    std::vector<std::unique_ptr<LobbyClient>> clients;
    std::vector<NetAddress> clientAddrs;
    std::vector<LocalProfile> profiles;
    std::vector<TrackStore> stores;
    std::vector<bool> followed;
    std::unique_ptr<LobbyHost> next; // the successor's host, once one promoted
    size_t nextAt = 0;               // the client whose address it runs on
    // Each client's clock is the host's plus its skew: two computers never share one.
    std::vector<uint32_t> skew;
    uint32_t nextSkew = 0; // the skew of the next client added
    std::vector<std::unique_ptr<LanBrowser>> browsers;
    HackIdentity clientHack; // the ROM hack of the next client added
    JoinSigner clientSigner;  // the signer of the next client added, then cleared (O7.3)
    Bytes32 clientKey{};      // that signer's key
    std::optional<Bytes32> clientExpect; // the host key the next client added expects, then cleared
    std::optional<NetAddress> clientAt;   // the address the next client added runs on, then cleared
    std::vector<Bytes32> clientKeys;     // each client's friend key; all zero for one with no signer
    bool clientReach = false;            // the next client added reports itself reachable, then cleared (R4)
    std::vector<bool> reach;             // each client's reachability, kept across a rejoin

    explicit Lan(const std::string& hostName = "Host", uint32_t roomId = 77, const HackIdentity& hostHack = {},
                 TrackStore hostStore = TrackStore(), RoomType type = RoomType::Lan) {
        LocalProfile hostProfile = Profile(hostName, 0xFF0000, 0);
        hostProfile.hack = hostHack;
        RoomConfig config = ConfigFor(roomId);
        config.type = type;
        host = std::make_unique<LobbyHost>(kBuild, hostProfile, kDefaultPort, config, net.SenderFor(hostAddr),
                                           std::move(hostStore));
        net.Add(hostAddr,
                [this](const NetAddress& from, const uint8_t* d, size_t n) { host->Receive(from, d, n, now); });
    }

    LobbyClient& AddClient(const std::string& name, Seat seat = Seat::Race, const std::string& build = kBuild,
                           uint32_t colour = 0x00FF00, TrackStore store = TrackStore()) {
        const size_t index = clients.size();
        const NetAddress addr = clientAt.value_or(
            Addr(Ip(192, 168, 1, static_cast<int>(20 + index)), static_cast<uint16_t>(50000 + index)));
        clientAt.reset();
        LocalProfile profile = Profile(name, colour, 3);
        profile.hack = clientHack;
        clients.push_back(std::make_unique<LobbyClient>(build, profile, seat, hostAddr,
                                                        0x1000u + static_cast<uint32_t>(index), net.SenderFor(addr),
                                                        store));
        clientAddrs.push_back(addr);
        profiles.push_back(profile);
        stores.push_back(store);
        skew.push_back(nextSkew);
        clientKeys.push_back(clientSigner ? clientKey : Bytes32{});
        reach.push_back(clientReach);
        clients.back()->SetReachable(clientReach);
        clientReach = false;
        if (clientSigner) {
            clients.back()->SetSigner(std::move(clientSigner));
            clientSigner = nullptr;
            clientKey = Bytes32{};
        }
        if (clientExpect.has_value()) {
            clients.back()->ExpectHostKey(*clientExpect);
            clientExpect.reset();
        }
        net.Add(addr, [this, index](const NetAddress& from, const uint8_t* d, size_t n) {
            if (next != nullptr && nextAt == index) {
                next->Receive(from, d, n, now + skew[index]);
                return;
            }
            clients[index]->Receive(from, d, n, now + skew[index]);
        });
        clients.back()->Start(now + skew[index]);
        return *clients.back();
    }

    // Every copy on the grid reports the track loaded, all of them the same track.
    void LoadAll() {
        ReadyToRace ready;
        ready.raceIndex = host->Room(now).raceIndex;
        CHECK(host->GridReady(ready, now));
        for (auto& c : clients) {
            if (c->GetState() != LobbyClient::State::Joined) {
                continue;
            }
            for (const PlayerInfo& p : c->Roster()) {
                if (p.id == c->LocalId() && p.inRace) {
                    c->GridReady(ready);
                }
            }
        }
    }

    void Tick(int frames = 1) {
        for (int i = 0; i < frames; i++) {
            now += 16;
            net.Step();
            if (host != nullptr) {
                host->Update(now);
            }
            if (next != nullptr) {
                next->Update(now + skew[nextAt]);
            }
            for (size_t i = 0; i < clients.size(); i++) {
                clients[i]->Update(now + skew[i]);
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

    // The host copy is gone without a word: a crash, or a cable pulled.
    void DropHost() {
        net.Cut(hostAddr);
        host.reset();
    }

    // Each client that closed follows its plan once, as the LAN worker does.
    void FollowPlans() {
        followed.resize(clients.size(), false);
        for (size_t i = 0; i < clients.size(); i++) {
            LobbyClient& c = *clients[i];
            if (followed[i] || c.GetState() != LobbyClient::State::Closed) {
                continue;
            }
            followed[i] = true;
            const HandoverPlan plan = c.NextHost();
            const uint32_t t = now + skew[i];
            if (plan.kind == HandoverPlan::Kind::Promote) {
                std::optional<RoomMachine> machine = RoomMachine::Promote(c.Mirror(t), plan.id, t);
                CHECK(machine.has_value() && next == nullptr);
                if (machine.has_value() && next == nullptr) {
                    next = std::make_unique<LobbyHost>(kBuild, profiles[i], clientAddrs[i].port, std::move(*machine),
                                                       c.HandoverTable(), plan.id, net.SenderFor(clientAddrs[i]),
                                                       stores[i]);
                    next->SetIdentity(clientKeys[i]);
                    next->SetMirrorKeys(c.Keys());
                    next->SetBans(c.Bans()); // as the LAN worker does (decision 52)
                    nextAt = i;
                }
            } else if (plan.kind == HandoverPlan::Kind::Rejoin) {
                auto back = std::make_unique<LobbyClient>(kBuild, profiles[i], Seat::Race, plan.address,
                                                          0x2000u + static_cast<uint32_t>(i),
                                                          net.SenderFor(clientAddrs[i]), stores[i]);
                CHECK(back->SetRejoin(plan));
                back->SetReachable(reach[i]);
                back->Start(t);
                clients[i] = std::move(back);
            }
        }
    }

    void TickFollow(int frames) {
        for (int i = 0; i < frames; i++) {
            Tick();
            FollowPlans();
        }
    }

    // True when the successor's host and every client still in its room are in the phase.
    bool NextIn(Phase phase) const {
        if (next == nullptr || next->Room(now + skew[nextAt]).phase != phase) {
            return false;
        }
        for (size_t i = 0; i < clients.size(); i++) {
            if (i != nextAt && clients[i]->GetState() == LobbyClient::State::Joined &&
                clients[i]->Room(now + skew[i]).phase != phase) {
                return false;
            }
        }
        return true;
    }

    // The race that is running goes to its cap with nobody over the line. order is the host game's order
    // of those still driving, or empty when the game said nothing. Nobody can end a race sooner.
    void CapRace(const std::vector<uint8_t>& order) {
        CHECK(HostPhase() == Phase::Racing);
        if (!order.empty()) {
            host->SetStanding(order);
        }
        const uint32_t limit = now + kRaceCapMs + 1000;
        while (HostPhase() == Phase::Racing && now < limit) {
            Tick();
        }
        CHECK(HostPhase() == Phase::Results);
    }

    Phase HostPhase() const {
        return host->Room(now).phase;
    }

    // True when the host and every client still in the room are in the phase.
    bool AllIn(Phase phase) const {
        if (HostPhase() != phase) {
            return false;
        }
        for (const auto& c : clients) {
            if (c->GetState() == LobbyClient::State::Joined && c->Room(now).phase != phase) {
                return false;
            }
        }
        return true;
    }
};

// A peer that speaks the channel but is not a LobbyClient: it sends what the test tells it to, so the host is
// tested against a copy that skips the client's own checks.
struct RawPeer {
    Lan& lan;
    NetAddress addr;
    std::unique_ptr<Channel> channel;
    std::vector<Message> got;

    RawPeer(Lan& l, int lastOctet, uint16_t version) : lan(l), addr(Addr(Ip(192, 168, 1, lastOctet), 51000)) {
        ChannelConfig config;
        config.role = Role::Client;
        config.connectionNonce = 0x7700u + static_cast<uint32_t>(lastOctet);
        config.version = version;
        config.timeoutMs = kLinkTimeoutMs;
        const DatagramSend send = lan.net.SenderFor(addr);
        const NetAddress host = lan.hostAddr;
        channel = std::make_unique<Channel>(config, [send, host](const uint8_t* d, size_t n) { send(host, d, n); });
        lan.net.Add(addr,
                    [this](const NetAddress&, const uint8_t* d, size_t n) { channel->Receive(d, n, lan.now); });
        channel->Connect(lan.now);
    }

    // This peer's part of one frame. A test with two raw peers ticks the network once and turns both.
    void Turn() {
        channel->Update(lan.now);
        Message msg;
        while (channel->PopMessage(msg)) {
            got.push_back(msg);
        }
    }

    void Run(int frames) {
        for (int i = 0; i < frames; i++) {
            lan.Tick();
            Turn();
        }
    }

    void Send(uint8_t type, const std::vector<uint8_t>& payload) {
        CHECK(channel->SendReliable(type, payload.data(), payload.size()) == QueueResult::Ok);
    }

    // The way a message is sent when the next one makes it worthless.
    void SendLoose(uint8_t type, const std::vector<uint8_t>& payload) {
        CHECK(channel->SendUnreliable(type, payload.data(), payload.size()) == QueueResult::Ok);
    }

    size_t Count(uint8_t type) const {
        size_t n = 0;
        for (const Message& m : got) {
            n += m.type == type ? 1 : 0;
        }
        return n;
    }

    const Message* Last(uint8_t type) const {
        for (auto it = got.rbegin(); it != got.rend(); ++it) {
            if (it->type == type) {
                return &*it;
            }
        }
        return nullptr;
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

// The ping column is refreshed on the host's own timer, so two copies of a roster may differ in it for a
// frame. Everything else must match.
bool SameRoster(std::vector<PlayerInfo> a, std::vector<PlayerInfo> b) {
    for (PlayerInfo& p : a) {
        p.pingMs = 0;
    }
    for (PlayerInfo& p : b) {
        p.pingMs = 0;
    }
    return a == b;
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

size_t LinesFrom(const std::deque<ShownLine>& log, uint8_t from) {
    size_t n = 0;
    for (const ShownLine& l : log) {
        n += l.from == from ? 1 : 0;
    }
    return n;
}

uint32_t Apart(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

LockRequest LockOf(uint8_t machine, uint8_t skin, uint8_t engine) {
    LockRequest lock;
    lock.machine = machine;
    lock.skin = skin;
    lock.engine = engine;
    return lock;
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

void TestJoinCodec() {
    std::printf("codec: join, welcome, reject, ready\n");
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    join.colour = 0x123456;
    join.machine = 29;
    join.seat = Seat::Watch;
    const std::vector<uint8_t> joinBytes = EncodeJoin(join);
    JoinRequest joinOut;
    CHECK(DecodeJoin(joinBytes.data(), joinBytes.size(), joinOut));
    CHECK(joinOut.build == kBuild && joinOut.name == "Ana" && joinOut.colour == 0x123456 && joinOut.machine == 29 &&
          joinOut.seat == Seat::Watch);
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
    bad.seat = static_cast<Seat>(2);
    CHECK(refusesJoin(bad));
    bad = join;
    bad.machine = kNoMachine;
    bad.seat = Seat::Race;
    const std::vector<uint8_t> noMachine = EncodeJoin(bad);
    CHECK(DecodeJoin(noMachine.data(), noMachine.size(), joinOut) && joinOut.machine == kNoMachine &&
          joinOut.seat == Seat::Race);

    uint8_t id = 0;
    std::vector<uint8_t> b = EncodeWelcome(3);
    CHECK(DecodeWelcome(b.data(), b.size(), id) && id == 3);
    b = EncodeWelcome(13);
    CHECK(DecodeWelcome(b.data(), b.size(), id) && id == 13);
    b = EncodeWelcome(0);
    CHECK(!DecodeWelcome(b.data(), b.size(), id));
    b = EncodeWelcome(14);
    CHECK(!DecodeWelcome(b.data(), b.size(), id) && id == 13);

    RejectReason reason = RejectReason::Full;
    for (uint8_t value = 1; value <= 11; value++) {
        b = EncodeReject(static_cast<RejectReason>(value));
        CHECK(DecodeReject(b.data(), b.size(), reason) && static_cast<uint8_t>(reason) == value);
        CHECK(RejectReasonText(reason)[0] != '\0');
    }
    CHECK(static_cast<uint8_t>(RejectReason::Rejoin) == 8 && static_cast<uint8_t>(RejectReason::BadProof) == 11);
    const uint8_t zero = 0;
    const uint8_t twelve = 12;
    CHECK(!DecodeReject(&zero, 1, reason) && !DecodeReject(&twelve, 1, reason));

    bool flag = false;
    b = EncodeReady(true);
    CHECK(DecodeReady(b.data(), b.size(), flag) && flag);
    const uint8_t two = 2;
    CHECK(!DecodeReady(&two, 1, flag));
    b = EncodeSeatNext(false);
    CHECK(DecodeSeatNext(b.data(), b.size(), flag) && !flag);
    b = EncodeSeatNext(true);
    CHECK(DecodeSeatNext(b.data(), b.size(), flag) && flag);
    CHECK(!DecodeSeatNext(&two, 1, flag));
}

Grid SampleGrid() {
    Grid grid;
    grid.raceIndex = 0x1234;
    grid.seed = 0xDEADBEEFu;
    grid.rules.laps = 5;
    grid.rules.ignoreTrackSettings = true;
    grid.rules.magnetRange = 120.0f;
    grid.rules.magnetStrength = 0.5f;
    grid.cpus = 27;
    grid.humans = { { 3, 29, 3 }, { 0, 0, 0 }, { 13, 7, 1 } };
    return grid;
}

// The bytes of a grid message with a stock track: 23 of header, then four per human with no machine record.
constexpr size_t kGridLaps = 6;
constexpr size_t kGridFlags = 7;
constexpr size_t kGridRange = 8;
constexpr size_t kGridBoost = 16;
constexpr size_t kGridTrack = 20;
constexpr size_t kGridCpus = 21;
constexpr size_t kGridCount = 22;
constexpr size_t kGridHumans = 23;
constexpr size_t kGridStride = 4;

void TestRaceCodec() {
    std::printf("codec: the grid, the start and the clock\n");

    ReadyToRace ready;
    ready.raceIndex = 0xBEEF;
    for (size_t i = 0; i < ready.digest.size(); i++) {
        ready.digest[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ready.loadMs = 4321;
    std::vector<uint8_t> b = EncodeReadyToRace(ready);
    CHECK(b.size() == 2 + kDigestBytes + 2);
    ReadyToRace gotReady;
    CHECK(DecodeReadyToRace(b.data(), b.size(), gotReady) && gotReady == ready);
    gotReady = ReadyToRace();
    CHECK(!DecodeReadyToRace(b.data(), b.size() - 1, gotReady) && !DecodeReadyToRace(nullptr, 0, gotReady));
    b.push_back(0);
    CHECK(!DecodeReadyToRace(b.data(), b.size(), gotReady));
    CHECK(gotReady == ReadyToRace()); // a refused message leaves the value alone

    const Grid grid = SampleGrid();
    const std::vector<uint8_t> g = EncodeGrid(grid);
    CHECK(g.size() == kGridHumans + kGridStride * grid.humans.size());
    CHECK(g[kGridLaps] == 5 && g[kGridCpus] == 27 && g[kGridCount] == 3 && g[kGridHumans] == 3);
    CHECK(g[kGridTrack] == 0 && g[kGridHumans + 3] == 0); // a stock track 0, and no record
    Grid got;
    CHECK(DecodeGrid(g.data(), g.size(), got) && got == grid);
    const auto refuses = [&g](size_t at, uint8_t value) {
        std::vector<uint8_t> bytes = g;
        bytes[at] = value;
        Grid out;
        return !DecodeGrid(bytes.data(), bytes.size(), out);
    };
    CHECK(refuses(kGridLaps, 0) && refuses(kGridLaps, static_cast<uint8_t>(kLapsMax + 1)));
    CHECK(!refuses(kGridLaps, kLapsMax) && !refuses(kGridLaps, 1));
    CHECK(refuses(kGridFlags, 4) && refuses(kGridFlags, 0x80) && !refuses(kGridFlags, 3) && !refuses(kGridFlags, 0));
    CHECK(refuses(kGridCpus, 28)); // three humans and 28 CPUs: one racer too many
    CHECK(refuses(kGridCount, 0) && refuses(kGridCount, 2) && refuses(kGridCount, 4)); // the count and the bytes
    CHECK(refuses(kGridHumans, static_cast<uint8_t>(kMaxPlayers))); // no such player
    CHECK(refuses(kGridHumans + kGridStride, 3));                    // the same player twice
    CHECK(refuses(kGridHumans + 3, 2));                              // a record flag is 0 or 1
    CHECK(refuses(kGridTrack, kTrackRandom) && refuses(kGridTrack, kTrackNone) && refuses(kGridTrack, 24));
    CHECK(refuses(kGridBoost, 0xFF) || refuses(kGridBoost + 3, 0xFF)); // far above kBoostFramesMax
    CHECK(refuses(kGridHumans + 1, static_cast<uint8_t>(kRosterMachines)));
    CHECK(refuses(kGridHumans + 2, static_cast<uint8_t>(kMachineSkins)));
    for (size_t cut = 0; cut < g.size(); cut++) {
        CHECK(!DecodeGrid(g.data(), cut, got));
    }
    std::vector<uint8_t> longer = g;
    longer.push_back(0);
    CHECK(!DecodeGrid(longer.data(), longer.size(), got));

    // A magnet value outside its limits, or one that is no number at all.
    Grid wild = grid;
    wild.rules.magnetRange = kMagnetRangeMax + 1.0f;
    std::vector<uint8_t> w = EncodeGrid(wild);
    CHECK(!DecodeGrid(w.data(), w.size(), got));
    wild = grid;
    wild.rules.magnetStrength = kMagnetStrengthMin / 2.0f;
    w = EncodeGrid(wild);
    CHECK(!DecodeGrid(w.data(), w.size(), got));
    w = g;
    for (size_t i = 0; i < 4; i++) {
        w[kGridRange + i] = 0xFF; // every byte order reads this as a NaN
    }
    CHECK(!DecodeGrid(w.data(), w.size(), got));

    // The fullest grid, and one racer more.
    Grid full;
    full.cpus = static_cast<uint8_t>(kGridSlots - kMaxRacers);
    for (uint8_t i = 0; i < kMaxRacers; i++) {
        full.humans.push_back({ i, static_cast<uint8_t>(i * 2), static_cast<uint8_t>(i % kMachineSkins) });
    }
    w = EncodeGrid(full);
    CHECK(DecodeGrid(w.data(), w.size(), got) && got == full);
    full.humans.push_back({ 12, 1, 0 });
    full.cpus = 0;
    w = EncodeGrid(full);
    CHECK(!DecodeGrid(w.data(), w.size(), got)); // thirteen racers
    Grid cpusOff = grid;
    cpusOff.cpus = 0;
    w = EncodeGrid(cpusOff);
    CHECK(DecodeGrid(w.data(), w.size(), got) && got == cpusOff);

    StartSignal start;
    start.raceIndex = 0xFFFF;
    start.startAtMs = 0xFFFFFFF0u;
    b = EncodeStart(start);
    StartSignal gotStart;
    CHECK(b.size() == 6 && DecodeStart(b.data(), b.size(), gotStart) && gotStart == start);
    CHECK(!DecodeStart(b.data(), 5, gotStart) && !DecodeStart(nullptr, 0, gotStart));
    b.push_back(0);
    CHECK(!DecodeStart(b.data(), b.size(), gotStart));

    uint32_t ms = 0;
    b = EncodeClockPing(0x89ABCDEFu);
    CHECK(b.size() == 4 && DecodeClockPing(b.data(), b.size(), ms) && ms == 0x89ABCDEFu);
    CHECK(!DecodeClockPing(b.data(), 3, ms) && !DecodeClockPing(nullptr, 0, ms));
    ClockPong pong;
    pong.clientMs = 0x01020304u;
    pong.hostMs = 0xF0E0D0C0u;
    b = EncodeClockPong(pong);
    ClockPong gotPong;
    CHECK(b.size() == 8 && DecodeClockPong(b.data(), b.size(), gotPong) && gotPong.clientMs == pong.clientMs &&
          gotPong.hostMs == pong.hostMs);
    CHECK(!DecodeClockPong(b.data(), 7, gotPong));
    b.push_back(0);
    CHECK(!DecodeClockPong(b.data(), b.size(), gotPong));
}

// A machine somewhere on a track, different for every slot and tick.
StatePose PoseOf(uint8_t slot, uint32_t tick) {
    StatePose pose;
    pose.slot = slot;
    pose.tick = tick;
    pose.timeMs = 500000u + tick * 17u + slot;
    pose.pos = { 100.0f * static_cast<float>(slot), -20.5f, 3.0f * static_cast<float>(tick) };
    pose.vel = { 0.25f * static_cast<float>(slot), -0.5f, 36.5f };
    pose.forward = { 0.0f, 0.0f, 1.0f };
    pose.up = { 0.0f, 1.0f, 0.0f };
    pose.lap = 1;
    pose.flags = 0x80000000u | tick;
    pose.attack = static_cast<uint8_t>(slot % 3);
    return pose;
}

void TestStateCodec() {
    std::printf("codec: the poses of a race\n");
    const auto decode = [](const uint8_t* d, size_t n) {
        RaceState s;
        return DecodeState(d, n, s);
    };

    // One pose, every number at the edge of what a machine can have.
    RaceState one;
    one.raceIndex = 0xBEEF;
    one.poses = { PoseOf(static_cast<uint8_t>(kGridSlots - 1), 0xFFFFFFF0u) };
    one.poses[0].timeMs = 0xFFFFFFFFu;
    one.poses[0].pos = { -kStatePosMax, kStatePosMax, 0.0f };
    one.poses[0].vel = { kStateVelMax, -kStateVelMax, 0.0f };
    one.poses[0].attack = kStateAttackMax;
    one.poses[0].forward = { -kStateAxisMax, kStateAxisMax, 0.25f };
    one.poses[0].up = { kStateAxisMax, -kStateAxisMax, -0.25f };
    one.poses[0].lap = kStateLapMin;
    one.poses[0].flags = 0xFFFFFFFFu;
    std::vector<uint8_t> b = EncodeState(one);
    RaceState out;
    CHECK(kStatePoseBytes == 64 && b.size() == kStateHeaderBytes + kStatePoseBytes);
    CHECK(StatePoseOk(one.poses[0]) && DecodeState(b.data(), b.size(), out) && out == one);
    // Every field has its own bytes: one changed and nothing else, the message says so.
    {
        const auto differs = [&](const std::function<void(StatePose&)>& change) {
            RaceState other = one;
            RaceState got;
            change(other.poses[0]);
            const std::vector<uint8_t> bytes = EncodeState(other);
            return bytes != b && DecodeState(bytes.data(), bytes.size(), got) && got == other && !(got == one);
        };
        CHECK(differs([](StatePose& p) { p.timeMs = 7; }));
        CHECK(differs([](StatePose& p) { p.vel[0] = 1.5f; }));
        CHECK(differs([](StatePose& p) { p.vel[1] = 1.5f; }));
        CHECK(differs([](StatePose& p) { p.vel[2] = 1.5f; }));
        CHECK(differs([](StatePose& p) { p.attack = 1; }));
        CHECK(differs([](StatePose& p) { p.tick = 9; }));
    }
    CheckEdges(b, decode);
    one.poses[0].lap = kStateLapMax;
    b = EncodeState(one);
    CHECK(DecodeState(b.data(), b.size(), out) && out == one);

    // A full message fits in one datagram, and a longer list is cut to it: the sender splits.
    RaceState full;
    full.raceIndex = 3;
    for (uint8_t slot = 0; slot < kStatePoses + 1; slot++) {
        full.poses.push_back(PoseOf(slot, 40));
    }
    b = EncodeState(full);
    CHECK(b.size() == kStateHeaderBytes + kStatePoses * kStatePoseBytes && b.size() <= kMaxMessagePayload);
    CHECK(DecodeState(b.data(), b.size(), out) && out.raceIndex == 3 && out.poses.size() == kStatePoses);
    full.poses.pop_back();
    CHECK(out == full);
    CheckEdges(b, decode);

    // A count the bytes do not bear out, in either direction, and no pose at all.
    std::vector<uint8_t> wrong = b;
    wrong[2] = static_cast<uint8_t>(kStatePoses - 1);
    CHECK(!decode(wrong.data(), wrong.size()));
    wrong = b;
    wrong.insert(wrong.end(), b.end() - kStatePoseBytes, b.end());
    wrong[2] = static_cast<uint8_t>(kStatePoses + 1);
    CHECK(wrong.size() == kStateHeaderBytes + (kStatePoses + 1) * kStatePoseBytes && !decode(wrong.data(), wrong.size()));
    const uint8_t none[] = { 3, 0, 0 };
    CHECK(!decode(none, sizeof(none)));
    b = EncodeState(RaceState());
    CHECK(b.size() == kStateHeaderBytes && !decode(b.data(), b.size()));

    // One number no machine can have refuses the whole message.
    const auto refused = [&](const std::function<void(StatePose&)>& damage) {
        RaceState bad;
        bad.poses = { PoseOf(1, 2), PoseOf(2, 2) };
        damage(bad.poses[1]);
        const std::vector<uint8_t> bytes = EncodeState(bad);
        return !StatePoseOk(bad.poses[1]) && StatePoseOk(bad.poses[0]) && !decode(bytes.data(), bytes.size());
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(!refused([](StatePose&) {})); // the pair is fine as it is
    CHECK(refused([](StatePose& p) { p.slot = static_cast<uint8_t>(kGridSlots); }));
    CHECK(refused([](StatePose& p) { p.slot = 0xFF; }));
    CHECK(refused([](StatePose& p) { p.lap = static_cast<int16_t>(kStateLapMin - 1); }));
    CHECK(refused([](StatePose& p) { p.lap = static_cast<int16_t>(kStateLapMax + 1); }));
    CHECK(refused([](StatePose& p) { p.attack = static_cast<uint8_t>(kStateAttackMax + 1); }));
    CHECK(refused([](StatePose& p) { p.attack = 0xFF; }));
    for (size_t k = 0; k < 3; k++) {
        CHECK(refused([&](StatePose& p) { p.vel[k] = nan; }));
        CHECK(refused([&](StatePose& p) { p.vel[k] = inf; }));
        CHECK(refused([&](StatePose& p) { p.vel[k] = -kStateVelMax * 1.001f; }));
        CHECK(refused([&](StatePose& p) { p.pos[k] = nan; }));
        CHECK(refused([&](StatePose& p) { p.pos[k] = -inf; }));
        CHECK(refused([&](StatePose& p) { p.pos[k] = kStatePosMax * 1.001f; }));
        CHECK(refused([&](StatePose& p) { p.forward[k] = nan; }));
        CHECK(refused([&](StatePose& p) { p.forward[k] = inf; }));
        CHECK(refused([&](StatePose& p) { p.forward[k] = -kStateAxisMax * 1.01f; }));
        CHECK(refused([&](StatePose& p) { p.up[k] = nan; }));
        CHECK(refused([&](StatePose& p) { p.up[k] = inf; }));
        CHECK(refused([&](StatePose& p) { p.up[k] = kStateAxisMax * 1.01f; }));
    }
}

void TestClockSync() {
    std::printf("the host clock as a client sees it\n");
    ClockSync clock;
    CHECK(!clock.Ready() && clock.OffsetMs() == 0 && clock.RttMs() == 0 && clock.Samples() == 0);

    // A ping stamped 1000 comes back at 1100 saying the host read 5050 when it answered: half the trip
    // later the host reads 5100, so the host clock is 4000 ahead.
    CHECK(clock.Sample(1000, 5050, 1100));
    CHECK(clock.Ready() && clock.OffsetMs() == 4000 && clock.RttMs() == 100 && clock.Samples() == 1);

    // A slower trip had more room to be wrong: the quicker sample stays in use.
    CHECK(clock.Sample(2000, 6300, 2400));
    CHECK(clock.OffsetMs() == 4000 && clock.RttMs() == 100);
    // A quicker one replaces it.
    CHECK(clock.Sample(3000, 7012, 3020));
    CHECK(clock.OffsetMs() == 4002 && clock.RttMs() == 20);

    // An answer that took longer than kClockStaleMs is no sample.
    CHECK(!clock.Sample(4000, 9000, 4000 + kClockStaleMs + 1));
    CHECK(clock.Samples() == 3 && clock.RttMs() == 20);
    CHECK(clock.Sample(4000, 9000, 4000 + kClockStaleMs));

    // The best sample is forgotten once kClockWindow newer ones are in: a clock that drifts is followed.
    for (uint32_t i = 0; i < kClockWindow; i++) {
        CHECK(clock.Sample(10000 + i * 100, 14530 + i * 100, 10060 + i * 100));
    }
    CHECK(clock.RttMs() == 60 && clock.OffsetMs() == 4500);

    // A host clock behind the local one, and both across the wrap of the counter.
    ClockSync wrap;
    CHECK(wrap.Sample(0xFFFFFFF0u, 100, 0x00000010u)); // a 32 ms trip that crossed the wrap
    CHECK(wrap.RttMs() == 32 && wrap.OffsetMs() == 100u + 16u - 0x10u);
    ClockSync behind;
    CHECK(behind.Sample(50000, 980, 50040));
    CHECK(behind.OffsetMs() == 0u - 49040u); // host clock minus local clock, modulo 2^32
    CHECK(static_cast<uint32_t>(50040u + behind.OffsetMs()) == 1000u);
}

void TestRosterCodec() {
    std::printf("codec: roster\n");
    std::vector<PlayerInfo> roster(4);
    roster[0].id = 0;
    roster[0].host = true;
    roster[0].name = "Host";
    roster[0].colour = 0xFF0000;
    roster[0].machine = 0;
    roster[0].skin = 3;
    roster[0].inRace = true;
    roster[0].locked = true;
    roster[1].id = 1;
    roster[1].ready = true;
    roster[1].name = "Ana";
    roster[1].machine = 29;
    roster[1].pingMs = 1234;
    roster[2].id = 5;
    roster[2].name = "Bo";
    roster[2].machine = kNoMachine;
    roster[2].inRace = true;
    roster[3].id = 13;
    roster[3].name = "Wim";
    roster[3].watcher = true;
    roster[3].raceNext = true;
    roster[0].points = 293;
    roster[2].points = 0xFFFF;
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
    std::vector<PlayerInfo> bad = roster;
    bad[2].id = 1;
    CHECK(refusesRoster(bad)); // duplicate id
    bad = roster;
    bad[1].host = true;
    CHECK(refusesRoster(bad)); // two hosts
    bad = roster;
    bad[0].host = false;
    CHECK(refusesRoster(bad)); // no host
    bad = roster;
    bad[2].id = 14;
    CHECK(refusesRoster(bad)); // id out of range
    CHECK(refusesRoster({}));   // empty
    bad = roster;
    bad[0].watcher = true;
    bad[0].inRace = false;
    bad[0].locked = false;
    CHECK(refusesRoster(bad)); // the host holds a race seat
    bad = roster;
    bad[1].raceNext = true;
    CHECK(refusesRoster(bad)); // only a watcher asks to race next
    bad = roster;
    bad[3].inRace = true;
    CHECK(refusesRoster(bad)); // a watcher is not on the grid
    bad = roster;
    bad[1].locked = true;
    CHECK(refusesRoster(bad)); // locked in, but not on the grid
    bad = roster;
    bad[1].skin = 4;
    CHECK(refusesRoster(bad));
    bad = roster;
    bad[1].machine = 30;
    CHECK(refusesRoster(bad));
    bad = roster;
    bad[1].colour = 0x1000000;
    CHECK(refusesRoster(bad));
    bad = roster;
    bad[1].name = "Ana ";
    CHECK(refusesRoster(bad));

    // Twelve racers and two watchers fill a room. One more of either kind is refused.
    const auto seats = [&roster](size_t racers, size_t watchers) {
        std::vector<PlayerInfo> r;
        for (size_t i = 0; i < racers + watchers; i++) {
            PlayerInfo p = i < racers ? roster[1] : roster[3];
            p.id = static_cast<uint8_t>(i);
            p.host = i == 0;
            r.push_back(p);
        }
        return r;
    };
    const std::vector<uint8_t> full = EncodeRoster(seats(12, 2));
    CHECK(DecodeRoster(full.data(), full.size(), rosterOut) && rosterOut.size() == kMaxPlayers);
    CHECK(refusesRoster(seats(13, 0)));
    CHECK(refusesRoster(seats(13, 1)));
    CHECK(refusesRoster(seats(1, 3)));
    CHECK(refusesRoster(seats(13, 2))); // fifteen players

    std::vector<uint8_t> flagged = rosterBytes;
    flagged[2] |= 0x80; // first entry's flags byte
    CHECK(!DecodeRoster(flagged.data(), flagged.size(), rosterOut));
    flagged = rosterBytes;
    flagged[2] |= 0x40;
    CHECK(!DecodeRoster(flagged.data(), flagged.size(), rosterOut));
}

void TestResultsCodec() {
    std::printf("codec: finish reports and the results table\n");
    FinishReport done;
    done.raceIndex = 0x1234;
    done.kind = ResultKind::Finished;
    done.timeMs = 91234;
    const std::vector<uint8_t> doneBytes = EncodeFinish(done);
    FinishReport got;
    CHECK(doneBytes.size() == 7 && DecodeFinish(doneBytes.data(), doneBytes.size(), got) && got == done);
    CheckEdges(doneBytes, [](const uint8_t* d, size_t n) {
        FinishReport o;
        return DecodeFinish(d, n, o);
    });
    FinishReport gone;
    gone.raceIndex = 0xFFFF;
    gone.kind = ResultKind::Retired;
    const std::vector<uint8_t> goneBytes = EncodeFinish(gone);
    CHECK(goneBytes.size() == 7 && DecodeFinish(goneBytes.data(), goneBytes.size(), got) && got == gone);
    // A refused report leaves what was there alone.
    const auto refusesFinish = [](const FinishReport& f) {
        const std::vector<uint8_t> e = EncodeFinish(f);
        FinishReport o;
        o.raceIndex = 999;
        return !DecodeFinish(e.data(), e.size(), o) && o.raceIndex == 999;
    };
    FinishReport bad = done;
    bad.timeMs = 0;
    CHECK(refusesFinish(bad)); // nobody finishes in no time
    bad.timeMs = kRaceTimeMaxMs + 1;
    CHECK(refusesFinish(bad));
    bad.timeMs = kRaceTimeMaxMs;
    CHECK(!refusesFinish(bad));
    bad.timeMs = 1;
    CHECK(!refusesFinish(bad));
    bad = gone;
    bad.timeMs = 1;
    CHECK(refusesFinish(bad)); // a retirement has no time
    bad = done;
    bad.kind = ResultKind::Unfinished;
    CHECK(refusesFinish(bad)); // nobody reports that they are still driving
    bad.timeMs = 0;
    CHECK(refusesFinish(bad));
    std::vector<uint8_t> raw = goneBytes;
    raw[2] = 3; // no such kind
    CHECK(!DecodeFinish(raw.data(), raw.size(), got));

    RaceResults table;
    table.raceIndex = 9;
    table.final = true;
    table.entries = {
        { 4, ResultKind::Finished, 90000, 100 }, { 0, ResultKind::Finished, 90000, 93 },
        { 13, ResultKind::Finished, kRaceTimeMaxMs, 87 }, { 2, ResultKind::Unfinished, 0, 81 },
        { 1, ResultKind::Unfinished, 0, 0 },      { 7, ResultKind::Retired, 0, 0 },
        { 3, ResultKind::Retired, 0, 0 },
    };
    const std::vector<uint8_t> tableBytes = EncodeResults(table);
    RaceResults tableOut;
    CHECK(tableBytes.size() == 4 + 7 * table.entries.size());
    CHECK(DecodeResults(tableBytes.data(), tableBytes.size(), tableOut) && tableOut == table);
    CheckEdges(tableBytes, [](const uint8_t* d, size_t n) {
        RaceResults o;
        return DecodeResults(d, n, o);
    });
    // Nobody on the grid: the table is empty, and still a table.
    RaceResults nobody;
    nobody.raceIndex = 2;
    const std::vector<uint8_t> nobodyBytes = EncodeResults(nobody);
    CHECK(nobodyBytes.size() == 4 && DecodeResults(nobodyBytes.data(), nobodyBytes.size(), tableOut) &&
          tableOut == nobody && !tableOut.final);

    const auto refusesTable = [](const RaceResults& t) {
        const std::vector<uint8_t> e = EncodeResults(t);
        RaceResults o;
        o.raceIndex = 999;
        return !DecodeResults(e.data(), e.size(), o) && o.raceIndex == 999 && o.entries.empty();
    };
    RaceResults wrong = table;
    wrong.entries[3].id = 4;
    CHECK(refusesTable(wrong)); // one player twice
    wrong = table;
    wrong.entries[3].id = kMaxPlayers;
    CHECK(refusesTable(wrong)); // nobody has that id
    wrong = table;
    wrong.entries[0].points = kPointsMax + 1;
    CHECK(refusesTable(wrong));
    wrong = table;
    wrong.entries[0].timeMs = 0;
    CHECK(refusesTable(wrong)); // finished in no time
    wrong = table;
    wrong.entries[2].timeMs = kRaceTimeMaxMs + 1;
    CHECK(refusesTable(wrong));
    wrong = table;
    wrong.entries[3].timeMs = 5;
    CHECK(refusesTable(wrong)); // a time, and still driving
    wrong = table;
    wrong.entries[5].timeMs = 5;
    CHECK(refusesTable(wrong)); // a time, and retired
    wrong = table;
    wrong.entries[5].points = 1;
    CHECK(refusesTable(wrong)); // a retirement scores nothing
    wrong = table;
    wrong.entries[1].timeMs = 89999;
    CHECK(refusesTable(wrong)); // the faster finisher below the slower
    wrong = table;
    std::swap(wrong.entries[2], wrong.entries[3]);
    CHECK(refusesTable(wrong)); // still driving, above a finisher
    wrong = table;
    std::swap(wrong.entries[4], wrong.entries[5]);
    CHECK(refusesTable(wrong)); // retired, above one still driving
    raw = tableBytes;
    raw[2] = 2; // final is yes or no
    CHECK(!DecodeResults(raw.data(), raw.size(), tableOut));
    raw = tableBytes;
    raw[5] = 3; // the first entry's kind
    CHECK(!DecodeResults(raw.data(), raw.size(), tableOut));
    raw = tableBytes;
    raw[3]++; // one more entry than there is
    CHECK(!DecodeResults(raw.data(), raw.size(), tableOut));
    raw = tableBytes;
    raw[3]--; // one fewer
    CHECK(!DecodeResults(raw.data(), raw.size(), tableOut));

    // Twelve racers fill a table. A thirteenth is refused.
    RaceResults full;
    for (uint8_t i = 0; i < kMaxRacers + 1; i++) {
        full.entries.push_back({ i, ResultKind::Unfinished, 0, PlacePoints(i + 1u) });
    }
    CHECK(refusesTable(full));
    full.entries.pop_back();
    const std::vector<uint8_t> fullBytes = EncodeResults(full);
    CHECK(fullBytes.size() == 4 + 7 * kMaxRacers && DecodeResults(fullBytes.data(), fullBytes.size(), tableOut) &&
          tableOut == full);

    // The points of a place: the game's own Grand Prix table, and nothing outside the twelve.
    const uint8_t points[] = { 100, 93, 87, 81, 76, 71, 66, 62, 58, 54, 50, 47 };
    for (size_t place = 1; place <= kMaxRacers; place++) {
        CHECK(PlacePoints(place) == points[place - 1]);
    }
    CHECK(PlacePoints(0) == 0 && PlacePoints(kMaxRacers + 1) == 0 && PlacePoints(1000) == 0 &&
          PlacePoints(1) == kPointsMax);
}

void TestRoomCodec() {
    std::printf("codec: room state, vote, lottery, pick, lock, preset messages\n");
    RoomState state;
    state.roomId = 0xA1B2C3D4;
    state.raceIndex = 513;
    state.phase = Phase::Machine;
    state.phaseLeftMs = 59000;
    state.phaseTotalMs = 60000;
    state.racerCap = 12;
    state.pool = 0x00F0F0F0;
    state.track = 7;
    state.lastTrack = 23;
    state.picker = 13;
    state.locked = 3;
    state.racers = 9;
    state.cpusOut = true;
    const std::vector<uint8_t> stateBytes = EncodeRoomState(state);
    RoomState stateOut;
    CHECK(DecodeRoomState(stateBytes.data(), stateBytes.size(), stateOut) && stateOut == state);
    CHECK(stateOut.cpusOut);
    // The CPU machines are in the race or out of it: that byte is 0 or 1 and nothing else. After it come
    // the custom-track switch (1), the CPU flags (1), AI aggression (2), the rules (14) and the successor
    // with its address (19), the room type (1), the lottery commitment (32), the room code (4) and the
    // course choice (1).
    {
        RoomState plain = state;
        plain.cpusOut = false;
        CHECK(!(plain == state));
        std::vector<uint8_t> bytes = EncodeRoomState(plain);
        const size_t at = bytes.size() - 76;
        CHECK(bytes.size() == stateBytes.size() && bytes[at] == 0 && stateBytes[at] == 1);
        CHECK(DecodeRoomState(bytes.data(), bytes.size(), stateOut) && stateOut == plain && !stateOut.cpusOut);
        bytes[at] = 2;
        RoomState kept;
        kept.roomId = 99;
        CHECK(!DecodeRoomState(bytes.data(), bytes.size(), kept) && kept.roomId == 99);
    }
    // The course choice (protocol 11) is the last byte: 0 the room votes, 1 last place picks, nothing else.
    {
        RoomState picks = state;
        picks.lastPlacePicks = true;
        CHECK(!(picks == state));
        std::vector<uint8_t> bytes = EncodeRoomState(picks);
        CHECK(bytes.size() == stateBytes.size() && bytes.back() == 1 && stateBytes.back() == 0);
        CHECK(DecodeRoomState(bytes.data(), bytes.size(), stateOut) && stateOut == picks && stateOut.lastPlacePicks);
        bytes.back() = 2;
        RoomState kept;
        kept.roomId = 99;
        CHECK(!DecodeRoomState(bytes.data(), bytes.size(), kept) && kept.roomId == 99);
    }
    CheckEdges(stateBytes, [](const uint8_t* d, size_t n) {
        RoomState o;
        return DecodeRoomState(d, n, o);
    });
    const std::vector<uint8_t> fresh = EncodeRoomState(RoomState());
    CHECK(DecodeRoomState(fresh.data(), fresh.size(), stateOut) && stateOut == RoomState());
    const auto refusesState = [](const RoomState& s) {
        const std::vector<uint8_t> e = EncodeRoomState(s);
        RoomState o;
        o.roomId = 99;
        return !DecodeRoomState(e.data(), e.size(), o) && o.roomId == 99;
    };
    RoomState badState = state;
    badState.phase = static_cast<Phase>(kPhaseCount);
    CHECK(refusesState(badState));
    badState = state;
    badState.phaseLeftMs = 60001;
    CHECK(refusesState(badState));
    badState = state;
    badState.racerCap = 0;
    CHECK(refusesState(badState));
    {
        RoomState solo = state; // a room of one race seat is a room (decision 76)
        solo.racerCap = 1;
        solo.racers = 1;
        solo.locked = 1;
        const std::vector<uint8_t> soloBytes = EncodeRoomState(solo);
        CHECK(DecodeRoomState(soloBytes.data(), soloBytes.size(), stateOut) && stateOut == solo);
    }
    badState = state;
    badState.racerCap = 13;
    CHECK(refusesState(badState));
    badState = state;
    badState.pool = 0;
    CHECK(refusesState(badState));
    badState = state;
    badState.pool = 1u << 24;
    CHECK(refusesState(badState));
    badState = state;
    badState.track = 24;
    CHECK(refusesState(badState));
    badState = state;
    badState.track = kTrackRandom;
    CHECK(refusesState(badState));
    badState = state;
    badState.lastTrack = 24;
    CHECK(refusesState(badState));
    badState = state;
    badState.picker = 14;
    CHECK(refusesState(badState));
    badState = state;
    badState.racers = 13;
    CHECK(refusesState(badState));
    badState = state;
    badState.locked = 10;
    CHECK(refusesState(badState));

    // Version 4: the settings a successor needs, and the successor itself (D28.1).
    {
        RoomState full = state;
        full.cpuFill = false;
        full.aiAggression = kAiAggressionMax;
        full.cpuTrackHazards = true;
        full.rules.laps = 7;
        full.rules.ignoreTrackSettings = true;
        full.rules.lavaNoKill = true;
        full.rules.magnetRange = 150.5f;
        full.rules.magnetStrength = 0.75f;
        full.rules.boostFrames = 420;
        full.successor = 5;
        full.successorIp = MappedV4(0xC0A8010A); // 192.168.1.10
        full.successorPort = 41234;
        const std::vector<uint8_t> fullBytes = EncodeRoomState(full);
        CHECK(fullBytes.size() == stateBytes.size());
        RoomState fullOut;
        CHECK(DecodeRoomState(fullBytes.data(), fullBytes.size(), fullOut) && fullOut == full);
        CHECK(!fullOut.cpuFill && fullOut.cpuTrackHazards && fullOut.aiAggression == 400 && fullOut.successor == 5 &&
              fullOut.successorIp == MappedV4(0xC0A8010A) && fullOut.successorPort == 41234 && fullOut.rules == full.rules);
        CheckEdges(fullBytes, [](const uint8_t* d, size_t n) {
            RoomState o;
            return DecodeRoomState(d, n, o);
        });
        // An IPv6 successor (O5.3) travels in the same 16 bytes.
        RoomState six = full;
        six.successorIp = IpBytes{ 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2a };
        const std::vector<uint8_t> sixBytes = EncodeRoomState(six);
        RoomState sixOut;
        CHECK(sixBytes.size() == fullBytes.size() && DecodeRoomState(sixBytes.data(), sixBytes.size(), sixOut) &&
              sixOut == six && sixOut.successorIp[15] == 0x2a && !IsMappedV4(sixOut.successorIp));
        // The defaults: CPUs fill the grid, stock aggression, no hazard avoidance, no successor.
        const RoomState plain;
        CHECK(plain.cpuFill && plain.aiAggression == kAiAggressionDefault && !plain.cpuTrackHazards &&
              plain.successor == kSystemId && IsZeroIp(plain.successorIp) && plain.successorPort == 0 &&
              plain.rules == RaceRules());
        // The CPU flags byte takes two bits and nothing else.
        std::vector<uint8_t> flagged = fullBytes;
        const size_t cpuFlagsAt = fullBytes.size() - 74;
        CHECK(flagged[cpuFlagsAt] == 2);
        for (uint8_t bad : { uint8_t{ 4 }, uint8_t{ 0x80 }, uint8_t{ 0xFF } }) {
            flagged[cpuFlagsAt] = bad;
            CHECK(!DecodeRoomState(flagged.data(), flagged.size(), fullOut));
        }
        flagged[cpuFlagsAt] = 3;
        CHECK(DecodeRoomState(flagged.data(), flagged.size(), fullOut) && fullOut.cpuFill && fullOut.cpuTrackHazards);
    }
    badState = state;
    badState.aiAggression = kAiAggressionMin - 1;
    CHECK(refusesState(badState));
    badState.aiAggression = kAiAggressionMax + 1;
    CHECK(refusesState(badState));
    badState.aiAggression = 0;
    CHECK(refusesState(badState));
    badState = state;
    badState.rules.laps = 0;
    CHECK(refusesState(badState));
    badState = state;
    badState.rules.magnetRange = kMagnetRangeMax + 1.0f;
    CHECK(refusesState(badState));
    badState = state;
    badState.rules.magnetStrength = std::numeric_limits<float>::quiet_NaN();
    CHECK(refusesState(badState));
    badState = state;
    badState.rules.boostFrames = kBoostFramesMax + 1;
    CHECK(refusesState(badState));
    // A successor is a member other than the host, with a port; nobody comes with no address at all.
    badState = state;
    badState.successor = kHostId;
    badState.successorPort = 1;
    CHECK(refusesState(badState));
    badState.successor = static_cast<uint8_t>(kMaxPlayers);
    CHECK(refusesState(badState));
    badState.successor = 3;
    badState.successorPort = 0;
    CHECK(refusesState(badState));
    badState = state;
    badState.successorIp = MappedV4(1);
    CHECK(refusesState(badState));
    badState = state;
    badState.successorPort = 1;
    CHECK(refusesState(badState));
    {
        RoomState lastSeat = state;
        lastSeat.successor = static_cast<uint8_t>(kMaxPlayers - 1);
        lastSeat.successorPort = 1;
        const std::vector<uint8_t> v = EncodeRoomState(lastSeat);
        CHECK(DecodeRoomState(v.data(), v.size(), stateOut) && stateOut == lastSeat);
    }
    for (uint8_t phase = 0; phase < kPhaseCount; phase++) {
        CHECK(PhaseName(static_cast<Phase>(phase))[0] != '\0');
    }

    CHECK(PoolOk(kPoolAll) && PoolOk(1u << 23) && !PoolOk(0) && !PoolOk(1u << 24));
    CHECK(TrackInPool(3, 1u << 3) && TrackInPool(kTrackRandom, 1u << 3));
    CHECK(!TrackInPool(4, 1u << 3) && !TrackInPool(24, kPoolAll) && !TrackInPool(kTrackNone, kPoolAll));

    uint8_t track = 0;
    for (uint8_t value : { uint8_t{ 0 }, uint8_t{ 23 }, kTrackRandom }) {
        std::vector<uint8_t> v = EncodeVote(value);
        CHECK(DecodeVote(v.data(), v.size(), track) && track == value);
        v = EncodePick(value);
        CHECK(DecodePick(v.data(), v.size(), track) && track == value);
    }
    for (uint8_t value : { uint8_t{ 24 }, kTrackNone }) {
        std::vector<uint8_t> v = EncodeVote(value);
        CHECK(!DecodeVote(v.data(), v.size(), track));
        v = EncodePick(value);
        CHECK(!DecodePick(v.data(), v.size(), track));
    }
    CheckEdges(EncodeVote(5), [](const uint8_t* d, size_t n) {
        uint8_t o = 0;
        return DecodeVote(d, n, o);
    });

    const std::vector<Ballot> ballots = { { 0, 4 }, { 2, kTrackRandom }, { 13, 23 } };
    std::vector<Ballot> ballotsOut;
    std::vector<uint8_t> b = EncodeVotes(ballots);
    CHECK(DecodeVotes(b.data(), b.size(), ballotsOut) && ballotsOut == ballots);
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        std::vector<Ballot> o;
        return DecodeVotes(d, n, o);
    });
    b = EncodeVotes({});
    CHECK(DecodeVotes(b.data(), b.size(), ballotsOut) && ballotsOut.empty());
    const auto refusesVotes = [](const std::vector<Ballot>& v) {
        const std::vector<uint8_t> e = EncodeVotes(v);
        std::vector<Ballot> o = { { 9, 9 } };
        return !DecodeVotes(e.data(), e.size(), o) && o.size() == 1;
    };
    CHECK(refusesVotes({ { 2, 4 }, { 1, 4 } }));  // ids not ascending
    CHECK(refusesVotes({ { 2, 4 }, { 2, 5 } }));  // two ballots from one player
    CHECK(refusesVotes({ { 14, 4 } }));           // id out of range
    CHECK(refusesVotes({ { 1, 24 } }));           // no such track
    CHECK(refusesVotes({ { 1, kTrackNone } }));
    std::vector<Ballot> many;
    for (uint8_t i = 0; i < 13; i++) {
        many.push_back({ i, 1 });
    }
    CHECK(refusesVotes(many)); // more ballots than race seats
    many.pop_back();
    b = EncodeVotes(many);
    CHECK(DecodeVotes(b.data(), b.size(), ballotsOut) && ballotsOut.size() == kMaxRacers);

    LotteryResult lottery;
    lottery.ballots = ballots;
    lottery.winner = 2;
    lottery.track = 23;
    LotteryResult lotteryOut;
    b = EncodeLottery(lottery);
    CHECK(DecodeLottery(b.data(), b.size(), lotteryOut) && lotteryOut == lottery);
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        LotteryResult o;
        return DecodeLottery(d, n, o);
    });
    const auto refusesLottery = [](const LotteryResult& l) {
        const std::vector<uint8_t> e = EncodeLottery(l);
        LotteryResult o;
        o.track = 1;
        return !DecodeLottery(e.data(), e.size(), o) && o.track == 1;
    };
    LotteryResult badLottery = lottery;
    badLottery.winner = 1; // a Random ballot wins with whatever was drawn
    badLottery.track = 17;
    b = EncodeLottery(badLottery);
    CHECK(DecodeLottery(b.data(), b.size(), lotteryOut) && lotteryOut == badLottery);
    badLottery.winner = 3;
    CHECK(refusesLottery(badLottery)); // no such ballot
    badLottery = lottery;
    badLottery.winner = 0;
    CHECK(refusesLottery(badLottery)); // ballot 0 named track 4, not 23
    badLottery = lottery;
    badLottery.winner = kNoWinner;
    CHECK(refusesLottery(badLottery)); // somebody voted, so somebody won
    badLottery = lottery;
    badLottery.winner = 1;
    badLottery.track = 24;
    CHECK(refusesLottery(badLottery));
    LotteryResult empty;
    empty.track = 3;
    b = EncodeLottery(empty);
    CHECK(DecodeLottery(b.data(), b.size(), lotteryOut) && lotteryOut == empty);
    empty.winner = 0;
    CHECK(refusesLottery(empty));
    CHECK(refusesLottery(LotteryResult())); // no track at all

    Picked pickedOut;
    for (const Picked& picked : { Picked{ 7, false, 3 }, Picked{ 23, true, kSystemId }, Picked{ 0, true, 13 } }) {
        b = EncodePicked(picked);
        CHECK(DecodePicked(b.data(), b.size(), pickedOut) && pickedOut == picked);
    }
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        Picked o;
        return DecodePicked(d, n, o);
    });
    b = EncodePicked(Picked{ 24, false, 3 });
    CHECK(!DecodePicked(b.data(), b.size(), pickedOut));
    b = EncodePicked(Picked{ 7, false, 14 });
    CHECK(!DecodePicked(b.data(), b.size(), pickedOut));
    b = EncodePicked(Picked{ 7, false, 3 });
    b[1] = 2; // drawn is 0 or 1
    CHECK(!DecodePicked(b.data(), b.size(), pickedOut));
    b = EncodePicked(Picked());
    CHECK(!DecodePicked(b.data(), b.size(), pickedOut)); // no track

    LockRequest lockOut;
    b = EncodeLock(LockOf(29, 3, 100));
    CHECK(DecodeLock(b.data(), b.size(), lockOut) && lockOut.machine == 29 && lockOut.skin == 3 &&
          lockOut.engine == 100);
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        LockRequest o;
        return DecodeLock(d, n, o);
    });
    b = EncodeLock(LockOf(30, 0, 50));
    CHECK(!DecodeLock(b.data(), b.size(), lockOut));
    b = EncodeLock(LockOf(kNoMachine, 0, 50));
    CHECK(!DecodeLock(b.data(), b.size(), lockOut));
    b = EncodeLock(LockOf(0, 4, 50));
    CHECK(!DecodeLock(b.data(), b.size(), lockOut));
    b = EncodeLock(LockOf(0, 0, 101));
    CHECK(!DecodeLock(b.data(), b.size(), lockOut) && lockOut.machine == 29);

    uint8_t emote = 0;
    b = EncodeEmote(7);
    CHECK(DecodeEmote(b.data(), b.size(), emote) && emote == 7);
    b = EncodeEmote(8);
    CHECK(!DecodeEmote(b.data(), b.size(), emote));
    EmoteLine line;
    line.from = 13;
    line.emote = 7;
    EmoteLine lineOut;
    b = EncodeEmoteLine(line);
    CHECK(DecodeEmoteLine(b.data(), b.size(), lineOut) && lineOut.from == 13 && lineOut.emote == 7);
    CheckEdges(b, [](const uint8_t* d, size_t n) {
        EmoteLine o;
        return DecodeEmoteLine(d, n, o);
    });
    line.from = kSystemId; // the room itself has no preset message to send
    b = EncodeEmoteLine(line);
    CHECK(!DecodeEmoteLine(b.data(), b.size(), lineOut));
    line.from = 1;
    line.emote = 8;
    b = EncodeEmoteLine(line);
    CHECK(!DecodeEmoteLine(b.data(), b.size(), lineOut));
    for (uint8_t e = 0; e < kEmoteCount; e++) {
        CHECK(EmoteText(e)[0] != '\0');
        for (uint8_t other = 0; other < e; other++) {
            CHECK(std::string(EmoteText(e)) != EmoteText(other));
        }
    }
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
    room.racers = 3;
    room.racerCap = 8;
    room.watchers = 2;
    room.phase = Phase::Voting;
    room.build = kBuild;
    room.hostName = "Host";
    const std::vector<uint8_t> reply = EncodeRoom(7, room);
    RoomInfo out;
    CHECK(DecodeRoom(reply.data(), reply.size(), nonce, out) && nonce == 7 && out.port == 24302 && out.racers == 3 &&
          out.racerCap == 8 && out.watchers == 2 && out.phase == Phase::Voting && out.build == kBuild &&
          out.hostName == "Host");
    CHECK(out.RaceSeatFree() && !out.WatchSeatFree());
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
    bad.racerCap = 0;
    CHECK(refuses(bad));
    bad = room;
    bad.racerCap = 13;
    CHECK(refuses(bad));
    bad = room;
    bad.racers = 9;
    CHECK(refuses(bad));
    bad = room;
    bad.racers = 0;
    CHECK(refuses(bad));
    bad = room;
    bad.watchers = 3;
    CHECK(refuses(bad));
    bad = room;
    bad.phase = static_cast<Phase>(kPhaseCount);
    CHECK(refuses(bad));
    bad = room;
    bad.hostName = "";
    CHECK(refuses(bad));
    bad = room;
    bad.build = "";
    CHECK(refuses(bad));
    bad = room;
    bad.racers = 8;
    bad.watchers = 0;
    const std::vector<uint8_t> fullRace = EncodeRoom(1, bad);
    CHECK(DecodeRoom(fullRace.data(), fullRace.size(), nonce, out) && !out.RaceSeatFree() && out.WatchSeatFree());

    // A copy on the version 1 lobby is not answered and its rooms are not listed: the version sits right
    // after the kind byte.
    std::vector<uint8_t> oldProbe = probe;
    oldProbe[5] = 1;
    CHECK(IsDiscovery(oldProbe.data(), oldProbe.size()) && !DecodeProbe(oldProbe.data(), oldProbe.size(), nonce));
    std::vector<uint8_t> oldReply = reply;
    oldReply[5] = 1;
    CHECK(!DecodeRoom(oldReply.data(), oldReply.size(), nonce, out));

    // Channel packets ("GD" + type 0 to 4) are never mistaken for discovery.
    const uint8_t hello[] = { 'G', 'D', 0, 1, 2, 3, 4, 2, 0 };
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

CustomTrack CustomOf(uint8_t fill, const std::string& title) {
    CustomTrack custom;
    for (size_t i = 0; i < custom.id.size(); i++) {
        custom.id[i] = static_cast<uint8_t>(fill + i);
    }
    custom.title = title;
    return custom;
}

// A record Machine Create could have made: every field the room checks in range, and a name with its NUL. The
// other bytes vary with fill.
MachineRecord RecordOf(uint8_t fill) {
    MachineRecord record{};
    for (size_t i = 0; i < record.size(); i++) {
        record[i] = static_cast<uint8_t>(fill ^ i);
    }
    record[0x00] = static_cast<uint8_t>(fill % 3);
    record[0x01] = static_cast<uint8_t>((fill / 3) % 3);
    record[0x02] = static_cast<uint8_t>((fill / 9) % 3);
    record[0x03] = static_cast<uint8_t>(fill % 7);
    record[0x04] = static_cast<uint8_t>((fill + 1) % 7);
    record[0x05] = static_cast<uint8_t>((fill + 2) % 7);
    record[0x06] = static_cast<uint8_t>(1 + fill % 8);
    record[0x08] = static_cast<uint8_t>(1 + fill % 5);
    record[0x15 + 8] = 0;
    return record;
}

void TestMachineRecordBounds() {
    std::printf("machine record bounds (O4.5)\n");
    for (int fill = 0; fill < 256; fill++) {
        CHECK(MachineRecordOk(RecordOf(static_cast<uint8_t>(fill))));
    }
    const auto with = [](size_t at, uint8_t value) {
        MachineRecord record = RecordOf(0);
        record[at] = value;
        return MachineRecordOk(record);
    };
    // Grades are 0 to 4.
    CHECK(with(0x00, 4) && !with(0x00, 5) && with(0x01, 4) && !with(0x01, 5) && with(0x02, 4) && !with(0x02, 5));
    // The weightings: body 4 (5) + boost 4 (8) is 13, the most Machine Create allows; one grip step more is 14.
    MachineRecord heavy = RecordOf(0);
    heavy[0x00] = 4;
    heavy[0x01] = 4;
    heavy[0x02] = 0;
    CHECK(MachineRecordOk(heavy));
    heavy[0x02] = 1;
    CHECK(!MachineRecordOk(heavy));
    heavy[0x00] = 3;
    CHECK(MachineRecordOk(heavy)); // 4 + 8 + 1
    heavy[0x01] = 3;
    heavy[0x02] = 4;
    CHECK(!MachineRecordOk(heavy)); // 4 + 6 + 8 is 18
    // Seven of each part.
    for (size_t part : { size_t{ 0x03 }, size_t{ 0x04 }, size_t{ 0x05 } }) {
        CHECK(with(part, 6) && !with(part, 7) && !with(part, 0xFF));
    }
    // Logos 1 to 8, decals 1 to 5, stored one above the option.
    CHECK(!with(0x06, 0) && with(0x06, 1) && with(0x06, 8) && !with(0x06, 9));
    CHECK(!with(0x08, 0) && with(0x08, 1) && with(0x08, 5) && !with(0x08, 6));
    // The colours, the number and the checksum are any bytes.
    CHECK(with(0x07, 0xFF) && with(0x09, 0xFF) && with(0x14, 0xFF) && with(0x1E, 0xFF) && with(0x1F, 0xFF));
    // A name ends within its nine bytes.
    MachineRecord name = RecordOf(0);
    for (size_t i = 0x15; i < 0x1E; i++) {
        name[i] = 'A';
    }
    CHECK(!MachineRecordOk(name));
    name[0x1D] = 0;
    CHECK(MachineRecordOk(name));
    name[0x1D] = 'A';
    name[0x15] = 0;
    CHECK(MachineRecordOk(name)); // an empty name

    // A decoder refuses a record out of range, in a lock and in a grid.
    MachineRecord bad = RecordOf(0);
    bad[0x03] = 7;
    LockRequest lock = LockOf(5, 0, 50);
    lock.hasRecord = true;
    lock.record = bad;
    std::vector<uint8_t> v = EncodeLock(lock);
    LockRequest lockOut;
    CHECK(!DecodeLock(v.data(), v.size(), lockOut));
    lock.record = RecordOf(0);
    v = EncodeLock(lock);
    CHECK(DecodeLock(v.data(), v.size(), lockOut) && lockOut == lock);
    // With no record the bytes are not there to check.
    lock.hasRecord = false;
    lock.record = MachineRecord{};
    v = EncodeLock(lock);
    CHECK(v.size() == 4 && DecodeLock(v.data(), v.size(), lockOut) && !lockOut.hasRecord);
    Grid grid = SampleGrid();
    grid.humans[0].hasRecord = true;
    grid.humans[0].record = bad;
    v = EncodeGrid(grid);
    Grid gridOut;
    CHECK(!DecodeGrid(v.data(), v.size(), gridOut));
    grid.humans[0].record = RecordOf(7);
    v = EncodeGrid(grid);
    CHECK(DecodeGrid(v.data(), v.size(), gridOut) && gridOut == grid);
}

void TestCustomTrackWire() {
    std::printf("codec: custom tracks, machine records, hack identity, boost length\n");
    CHECK(kLobbyVersion == kProtocolVersion && kProtocolVersion == 11); // O5.1, O5.3, O6.1, O7.3, R4, direct links and the course choice bumped it
    CHECK(static_cast<uint8_t>(Phase::Assets) == 9 && kPhaseCount == 10);
    CHECK(std::string(PhaseName(Phase::Assets)) == "Getting the track");
    CHECK(static_cast<uint8_t>(RejectReason::Hack) == 7 && RejectReasonText(RejectReason::Hack)[0] != '\0');
    CHECK(kMsgTrackOffer == 26 && kMsgTrackAbort == 30);

    CHECK(TrackTitleOk("A") && TrackTitleOk(std::string(kTrackTitleMax, 'Z')) && TrackTitleOk("Big Blue 2"));
    CHECK(!TrackTitleOk("") && !TrackTitleOk(std::string(kTrackTitleMax + 1, 'Z')) && !TrackTitleOk("Tab\tx"));

    // A vote and a pick name a custom track by its identity and title; a stock track keeps its one byte.
    const CustomTrack mine = CustomOf(0x40, "Loop City");
    TrackChoice choice;
    choice.track = kTrackCustom;
    choice.custom = mine;
    std::vector<uint8_t> v = EncodeVote(choice);
    CHECK(v.size() == 1 + kTrackIdBytes + 1 + mine.title.size() && v[0] == kTrackCustom);
    TrackChoice back;
    CHECK(DecodeVote(v.data(), v.size(), back) && back == choice);
    back = TrackChoice();
    CHECK(DecodePick(v.data(), v.size(), back) && back == choice);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        TrackChoice o;
        return DecodeVote(d, n, o);
    });
    uint8_t one = 0;
    CHECK(!DecodeVote(v.data(), v.size(), one)); // the one-byte form never takes a custom track
    TrackChoice stock;
    stock.track = 9;
    v = EncodeVote(stock);
    CHECK(v.size() == 1 && DecodeVote(v.data(), v.size(), back) && back == stock && back.custom == CustomTrack());
    stock.track = kTrackRandom;
    v = EncodePick(stock);
    CHECK(DecodePick(v.data(), v.size(), back) && back.track == kTrackRandom);
    for (uint8_t bad : { uint8_t{ 24 }, kTrackNone }) {
        TrackChoice b;
        b.track = bad;
        v = EncodeVote(b);
        CHECK(!DecodeVote(v.data(), v.size(), back));
    }
    for (const std::string& title : { std::string(), std::string(kTrackTitleMax + 1, 'x'), std::string("a\nb") }) {
        TrackChoice b = choice;
        b.custom.title = title;
        v = EncodeVote(b);
        TrackChoice kept;
        kept.track = 3;
        CHECK(!DecodeVote(v.data(), v.size(), kept) && kept.track == 3);
    }

    // Ballots, the lottery and the pick result.
    const std::vector<Ballot> ballots = { { 0, 4, {} }, { 2, kTrackCustom, mine }, { 5, kTrackRandom, {} } };
    std::vector<Ballot> ballotsOut;
    v = EncodeVotes(ballots);
    CHECK(DecodeVotes(v.data(), v.size(), ballotsOut) && ballotsOut == ballots);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        std::vector<Ballot> o;
        return DecodeVotes(d, n, o);
    });
    LotteryResult lottery;
    lottery.ballots = ballots;
    lottery.winner = 1;
    lottery.track = kTrackCustom;
    lottery.custom = mine;
    v = EncodeLottery(lottery);
    LotteryResult lotteryOut;
    CHECK(DecodeLottery(v.data(), v.size(), lotteryOut) && lotteryOut == lottery);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        LotteryResult o;
        return DecodeLottery(d, n, o);
    });
    const auto refusesLottery = [](const LotteryResult& l) {
        const std::vector<uint8_t> e = EncodeLottery(l);
        LotteryResult o;
        return !DecodeLottery(e.data(), e.size(), o);
    };
    LotteryResult badLottery = lottery;
    badLottery.custom = CustomOf(0x41, "Loop City"); // not the winning ballot's track
    CHECK(refusesLottery(badLottery));
    badLottery = lottery;
    badLottery.winner = 2; // a Random ballot won: the draw is stock
    CHECK(refusesLottery(badLottery));
    badLottery = LotteryResult();
    badLottery.track = kTrackCustom;
    badLottery.custom = mine; // nobody voted: the draw is stock
    CHECK(refusesLottery(badLottery));
    // The same ballots give the same custom winner on every copy.
    const LotteryResult run = RunLottery(77, 3, ballots, kPoolAll, kTrackNone);
    CHECK(run.ballots[run.winner].track != kTrackCustom || (run.track == kTrackCustom && run.custom == mine));
    CHECK(run.track == kTrackCustom || run.custom == CustomTrack());
    {
        const std::vector<Ballot> only = { { 4, kTrackCustom, mine } };
        const LotteryResult solo = RunLottery(77, 3, only, kPoolAll, kTrackNone);
        CHECK(solo.track == kTrackCustom && solo.custom == mine && solo.winner == 0);
        v = EncodeLottery(solo);
        CHECK(DecodeLottery(v.data(), v.size(), lotteryOut) && lotteryOut == solo);
    }

    Picked picked;
    picked.track = kTrackCustom;
    picked.by = 2;
    picked.custom = mine;
    v = EncodePicked(picked);
    Picked pickedOut;
    CHECK(DecodePicked(v.data(), v.size(), pickedOut) && pickedOut == picked);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        Picked o;
        return DecodePicked(d, n, o);
    });
    Picked badPick = picked;
    badPick.drawn = true;
    v = EncodePicked(badPick);
    CHECK(!DecodePicked(v.data(), v.size(), pickedOut));
    badPick = picked;
    badPick.by = kSystemId;
    v = EncodePicked(badPick);
    CHECK(!DecodePicked(v.data(), v.size(), pickedOut));

    // The room: a custom track in play, and the host's switch.
    RoomState room;
    room.phase = Phase::Assets;
    room.track = kTrackCustom;
    room.custom = mine;
    room.lastTrack = kTrackCustom;
    room.customTracks = true;
    v = EncodeRoomState(room);
    RoomState roomOut;
    CHECK(DecodeRoomState(v.data(), v.size(), roomOut) && roomOut == room && roomOut.customTracks);
    // The switch is the byte before the version 4 tail: CPU flags, aggression, rules, successor (36 bytes),
    // then the version 7 room type, commitment and code (37 bytes) and the version 11 course choice (1 byte).
    const size_t switchAt = v.size() - 75;
    CHECK(v[switchAt] == 1);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        RoomState o;
        return DecodeRoomState(d, n, o);
    });
    v[switchAt] = 2;
    CHECK(!DecodeRoomState(v.data(), v.size(), roomOut));
    room.customTracks = false;
    room.custom.title.clear();
    v = EncodeRoomState(room);
    CHECK(!DecodeRoomState(v.data(), v.size(), roomOut));

    // A lock and a grid entry carry an optional machine record.
    LockRequest lock = LockOf(29, 3, 70);
    v = EncodeLock(lock);
    CHECK(v.size() == 4 && v[3] == 0);
    lock.hasRecord = true;
    lock.record = RecordOf(0x5A);
    v = EncodeLock(lock);
    CHECK(v.size() == 4 + kMachineRecordBytes && v[3] == 1);
    LockRequest lockOut;
    CHECK(DecodeLock(v.data(), v.size(), lockOut) && lockOut == lock);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        LockRequest o;
        return DecodeLock(d, n, o);
    });
    v[3] = 2;
    CHECK(!DecodeLock(v.data(), v.size(), lockOut));

    // The grid: the boost length, a custom track and the records.
    Grid grid = SampleGrid();
    grid.rules.boostFrames = 300;
    grid.track.track = kTrackCustom;
    grid.track.custom = mine;
    grid.humans[1].hasRecord = true;
    grid.humans[1].record = RecordOf(0x11);
    v = EncodeGrid(grid);
    Grid gridOut;
    CHECK(DecodeGrid(v.data(), v.size(), gridOut) && gridOut == grid && gridOut.rules.boostFrames == 300);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        Grid o;
        return DecodeGrid(d, n, o);
    });
    Grid badGrid = grid;
    badGrid.rules.boostFrames = kBoostFramesMax + 1;
    v = EncodeGrid(badGrid);
    CHECK(!DecodeGrid(v.data(), v.size(), gridOut));
    badGrid.rules.boostFrames = kBoostFramesMax;
    v = EncodeGrid(badGrid);
    CHECK(DecodeGrid(v.data(), v.size(), gridOut) && gridOut.rules.boostFrames == kBoostFramesMax);
    for (uint8_t bad : { kTrackRandom, kTrackNone }) {
        badGrid = grid;
        badGrid.track.track = bad;
        badGrid.track.custom = CustomTrack();
        v = EncodeGrid(badGrid);
        CHECK(!DecodeGrid(v.data(), v.size(), gridOut));
    }
    RaceRules rules;
    rules.boostFrames = kBoostFramesMax;
    CHECK(RulesOk(rules));
    rules.boostFrames = kBoostFramesMax + 1;
    CHECK(!RulesOk(rules));

    // The join carries the hack: the stock game is an empty name and zero bytes.
    CHECK(HackOk(HackIdentity()));
    HackIdentity hack;
    hack.name = "Big Blue Remix";
    hack.hash[0] = 0xAB;
    hack.hash[31] = 0xCD;
    CHECK(HackOk(hack));
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    join.hack = hack;
    v = EncodeJoin(join);
    JoinRequest joinOut;
    CHECK(DecodeJoin(v.data(), v.size(), joinOut) && joinOut.hack == hack && joinOut.name == "Ana");
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        JoinRequest o;
        return DecodeJoin(d, n, o);
    });
    JoinRequest badJoin = join;
    badJoin.hack.name.clear(); // a hash with no name
    CHECK(!HackOk(badJoin.hack));
    v = EncodeJoin(badJoin);
    CHECK(!DecodeJoin(v.data(), v.size(), joinOut));
    badJoin = join;
    badJoin.hack.name = std::string(kHackNameMax + 1, 'h');
    v = EncodeJoin(badJoin);
    CHECK(!DecodeJoin(v.data(), v.size(), joinOut));
    badJoin = join;
    badJoin.hack.name = "bad\x01name";
    v = EncodeJoin(badJoin);
    CHECK(!DecodeJoin(v.data(), v.size(), joinOut));
    badJoin = join;
    badJoin.hack.name = "Caf\xC3\xA9 Mix"; // a file name may be UTF-8
    v = EncodeJoin(badJoin);
    CHECK(DecodeJoin(v.data(), v.size(), joinOut) && joinOut.hack == badJoin.hack);
}

void TestHackIdentity() {
    std::printf("the host admits only its own ROM hack\n");
    HackIdentity hack;
    hack.name = "Remix";
    hack.hash[5] = 0x42;
    {
        // A stock room refuses a hacked copy and takes a stock one.
        Lan lan;
        lan.clientHack = hack;
        LobbyClient& hacked = lan.AddClient("Hacked");
        lan.clientHack = HackIdentity();
        LobbyClient& plain = lan.AddClient("Plain");
        lan.Settle();
        CHECK(hacked.GetState() == LobbyClient::State::Closed);
        CHECK(hacked.CloseText() == RejectReasonText(RejectReason::Hack));
        CHECK(plain.GetState() == LobbyClient::State::Joined);
        CHECK(lan.host->Roster().size() == 2);
    }
    {
        // A hacked room takes the same hack, and refuses the stock game and the same name with other bytes.
        Lan lan("Host", 78, hack);
        lan.clientHack = hack;
        LobbyClient& same = lan.AddClient("Same");
        lan.clientHack = HackIdentity();
        LobbyClient& plain = lan.AddClient("Plain");
        HackIdentity other = hack;
        other.hash[5] = 0x43;
        lan.clientHack = other;
        LobbyClient& near = lan.AddClient("Near");
        lan.Settle();
        CHECK(same.GetState() == LobbyClient::State::Joined);
        CHECK(plain.GetState() == LobbyClient::State::Closed && plain.CloseText() == RejectReasonText(RejectReason::Hack));
        CHECK(near.GetState() == LobbyClient::State::Closed && near.CloseText() == RejectReasonText(RejectReason::Hack));
        CHECK(lan.host->Roster().size() == 2);
    }
}

void TestHandoverCodec() {
    std::printf("codec: handover token, tokens, handover, rejoin\n");
    // A member's own token: all 64 bits travel, and zero is never a token.
    HandoverToken token;
    token.token = 0x0123456789ABCDEFull;
    std::vector<uint8_t> v = EncodeHandoverToken(token);
    HandoverToken tokenOut;
    CHECK(v.size() == 8 && v[0] == 0xEF && v[7] == 0x01);
    CHECK(DecodeHandoverToken(v.data(), v.size(), tokenOut) && tokenOut == token);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        HandoverToken o;
        return DecodeHandoverToken(d, n, o);
    });
    token.token = ~0ull;
    v = EncodeHandoverToken(token);
    CHECK(DecodeHandoverToken(v.data(), v.size(), tokenOut) && tokenOut.token == ~0ull);
    tokenOut.token = 7;
    v = EncodeHandoverToken(HandoverToken());
    CHECK(!DecodeHandoverToken(v.data(), v.size(), tokenOut) && tokenOut.token == 7);

    // The successor's copy of every other member's token.
    const std::vector<HandoverTokenEntry> tokens = { { 1, 0x1111222233334444ull },
                                                     { 4, 0x8000000000000001ull },
                                                     { static_cast<uint8_t>(kMaxPlayers - 1), 9 } };
    v = EncodeHandoverTokens(tokens);
    std::vector<HandoverTokenEntry> tokensOut;
    CHECK(v.size() == 1 + 3 * 9 + 32 + 2 && DecodeHandoverTokens(v.data(), v.size(), tokensOut) && tokensOut == tokens);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        std::vector<HandoverTokenEntry> o;
        return DecodeHandoverTokens(d, n, o);
    });
    v = EncodeHandoverTokens({});
    CHECK(DecodeHandoverTokens(v.data(), v.size(), tokensOut) && tokensOut.empty());
    const auto refusesTokens = [](const std::vector<HandoverTokenEntry>& t) {
        const std::vector<uint8_t> e = EncodeHandoverTokens(t);
        std::vector<HandoverTokenEntry> o = { { 3, 3 } };
        return !DecodeHandoverTokens(e.data(), e.size(), o) && o.size() == 1 && o[0].id == 3;
    };
    CHECK(refusesTokens({ { 1, 5 }, { 1, 6 } }));                               // the same id twice
    CHECK(refusesTokens({ { 4, 5 }, { 2, 6 } }));                               // out of order
    CHECK(refusesTokens({ { 2, 0 } }));                                          // a zero token
    CHECK(refusesTokens({ { static_cast<uint8_t>(kMaxPlayers), 5 } }));         // not a member id
    {
        std::vector<HandoverTokenEntry> tooMany;
        for (uint8_t id = 0; id <= kMaxPlayers; id++) {
            tooMany.push_back({ id, 1u + id });
        }
        CHECK(refusesTokens(tooMany));
        tooMany.pop_back();
        v = EncodeHandoverTokens(tooMany);
        CHECK(DecodeHandoverTokens(v.data(), v.size(), tokensOut) && tokensOut.size() == kMaxPlayers);
    }

    // The host leaves now: a successor with a port, or nobody at all.
    Handover handover;
    handover.successor = 2;
    handover.ip = MappedV4(0x0A000002);
    handover.port = 50000;
    v = EncodeHandover(handover);
    Handover handoverOut;
    CHECK(v.size() == 19 && DecodeHandover(v.data(), v.size(), handoverOut) && handoverOut == handover);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        Handover o;
        return DecodeHandover(d, n, o);
    });
    Handover six = handover;
    six.ip = IpBytes{ 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7 };
    v = EncodeHandover(six);
    CHECK(DecodeHandover(v.data(), v.size(), handoverOut) && handoverOut == six);
    v = EncodeHandover(Handover());
    CHECK(DecodeHandover(v.data(), v.size(), handoverOut) && handoverOut.successor == kSystemId);
    const auto refusesHandover = [](uint8_t successor, uint32_t ip, uint16_t port) {
        Handover h;
        h.successor = successor;
        h.ip = ip == 0 ? IpBytes{} : MappedV4(ip);
        h.port = port;
        const std::vector<uint8_t> e = EncodeHandover(h);
        Handover o;
        o.port = 77;
        return !DecodeHandover(e.data(), e.size(), o) && o.port == 77;
    };
    CHECK(refusesHandover(kHostId, 1, 1));
    CHECK(refusesHandover(static_cast<uint8_t>(kMaxPlayers), 1, 1));
    CHECK(refusesHandover(2, 1, 0));
    CHECK(refusesHandover(kSystemId, 1, 0));
    CHECK(refusesHandover(kSystemId, 0, 1));

    // A member comes back under its old id.
    Rejoin rejoin;
    rejoin.id = 6;
    rejoin.token = 0xFEEDFACECAFEBEEFull;
    v = EncodeRejoin(rejoin);
    Rejoin rejoinOut;
    CHECK(v.size() == 9 && DecodeRejoin(v.data(), v.size(), rejoinOut) && rejoinOut == rejoin);
    CheckEdges(v, [](const uint8_t* d, size_t n) {
        Rejoin o;
        return DecodeRejoin(d, n, o);
    });
    const auto refusesRejoin = [](uint8_t id, uint64_t t) {
        Rejoin r;
        r.id = id;
        r.token = t;
        const std::vector<uint8_t> e = EncodeRejoin(r);
        Rejoin o;
        o.id = 9;
        return !DecodeRejoin(e.data(), e.size(), o) && o.id == 9;
    };
    CHECK(refusesRejoin(kHostId, 5));                               // the host never rejoins
    CHECK(refusesRejoin(static_cast<uint8_t>(kMaxPlayers), 5));
    CHECK(refusesRejoin(3, 0));
    CHECK(!refusesRejoin(static_cast<uint8_t>(kMaxPlayers - 1), 1));
}

void TestDecoderSweep() {
    std::printf("decoder sweep\n");
    // Random bytes and random single-byte damage to valid messages: every decoder either refuses or returns a
    // value that encodes back to the very bytes it read. Nothing may crash.
    uint32_t seed = 0x51ED;
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    std::vector<PlayerInfo> roster(3);
    roster[0].host = true;
    roster[0].name = "Host";
    roster[0].inRace = true;
    roster[1].id = 1;
    roster[1].name = "Ana";
    roster[2].id = 2;
    roster[2].name = "Wim";
    roster[2].watcher = true;
    roster[1].points = 187;
    RoomState state;
    state.phase = Phase::Voting;
    state.phaseLeftMs = 100;
    state.phaseTotalMs = 20000;
    state.racers = 2;
    const std::vector<Ballot> ballots = { { 0, 4 }, { 1, kTrackRandom } };
    LotteryResult lottery;
    lottery.ballots = ballots;
    lottery.winner = 0;
    lottery.track = 4;
    EmoteLine line;
    line.from = 1;
    line.emote = 2;
    RoomInfo seedRoom;
    seedRoom.port = 1;
    seedRoom.racers = 1;
    seedRoom.racerCap = 8;
    seedRoom.build = "b";
    seedRoom.hostName = "H";
    const std::vector<std::vector<uint8_t>> seeds = {
        EncodeJoin(join),       EncodeRoster(roster),          EncodeRoomState(state),      EncodeVotes(ballots),
        EncodeLottery(lottery), EncodePicked({ 7, false, 1 }), EncodeLock(LockOf(5, 1, 60)), EncodeEmoteLine(line),
        EncodeProbe(1),         EncodeRoom(1, seedRoom),
        EncodeGrid(SampleGrid()), EncodeStart({ 3, 5000 }), EncodeReadyToRace(ReadyToRace()),
        EncodeClockPong({ 1, 2 }), EncodeState({ 3, { PoseOf(1, 10), PoseOf(2, 10) } }),
        EncodeFinish({ 3, ResultKind::Finished, 91000 }), EncodeFinish({ 3, ResultKind::Retired, 0 }),
        EncodeResults({ 3, true, { { 1, ResultKind::Finished, 91000, 100 }, { 0, ResultKind::Unfinished, 0, 93 },
                                   { 2, ResultKind::Retired, 0, 0 } } }),
    };
    std::vector<std::vector<uint8_t>> extra;
    {
        const CustomTrack custom = CustomOf(0x20, "Sweep");
        TrackChoice choice;
        choice.track = kTrackCustom;
        choice.custom = custom;
        extra.push_back(EncodeVote(choice));
        const std::vector<Ballot> withCustom = { { 1, kTrackCustom, custom } };
        extra.push_back(EncodeVotes(withCustom));
        LotteryResult won;
        won.ballots = withCustom;
        won.winner = 0;
        won.track = kTrackCustom;
        won.custom = custom;
        extra.push_back(EncodeLottery(won));
        Picked picked;
        picked.track = kTrackCustom;
        picked.by = 1;
        picked.custom = custom;
        extra.push_back(EncodePicked(picked));
        LockRequest lock = LockOf(5, 1, 60);
        lock.hasRecord = true;
        lock.record = RecordOf(3);
        extra.push_back(EncodeLock(lock));
        RoomState customRoom = state;
        customRoom.track = kTrackCustom;
        customRoom.custom = custom;
        customRoom.customTracks = true;
        extra.push_back(EncodeRoomState(customRoom));
        Grid grid = SampleGrid();
        grid.track = choice;
        grid.rules.boostFrames = 250;
        grid.humans[0].hasRecord = true;
        grid.humans[0].record = RecordOf(9);
        extra.push_back(EncodeGrid(grid));
        JoinRequest hacked = join;
        hacked.hack.name = "Hack";
        hacked.hack.hash[0] = 1;
        extra.push_back(EncodeJoin(hacked));
        RoomState handedOver = state;
        handedOver.cpuFill = false;
        handedOver.cpuTrackHazards = true;
        handedOver.aiAggression = 250;
        handedOver.rules.laps = 5;
        handedOver.successor = 2;
        handedOver.successorIp = MappedV4(0x7F000001);
        handedOver.successorPort = 7777;
        extra.push_back(EncodeRoomState(handedOver));
        extra.push_back(EncodeHandoverToken({ 0x0102030405060708ull }));
        extra.push_back(EncodeHandoverTokens({ { 1, 11 }, { 2, 0xFFFFFFFFFFull } }));
        extra.push_back(EncodeHandover({ 2, MappedV4(0x7F000001), 7777 }));
        extra.push_back(EncodeRejoin({ 2, 0xABCDEFull }));
    }
    std::vector<std::vector<uint8_t>> allSeeds = seeds;
    allSeeds.insert(allSeeds.end(), extra.begin(), extra.end());
    int accepted = 0;
    for (int round = 0; round < 30000; round++) {
        std::vector<uint8_t> bytes;
        if (round % 2 == 0) {
            bytes.resize(Next(seed) % 48);
            for (uint8_t& v : bytes) {
                v = static_cast<uint8_t>(Next(seed));
            }
        } else {
            bytes = allSeeds[Next(seed) % allSeeds.size()];
            if (!bytes.empty()) {
                bytes[Next(seed) % bytes.size()] = static_cast<uint8_t>(Next(seed));
            }
        }
        const uint8_t* d = bytes.data();
        const size_t n = bytes.size();
        JoinRequest j;
        if (DecodeJoin(d, n, j)) {
            accepted++;
            CHECK(NameOk(j.name) && MachineOk(j.machine) && ColourOk(j.colour) && HackOk(j.hack));
            CHECK(EncodeJoin(j) == bytes);
        }
        std::vector<PlayerInfo> r;
        if (DecodeRoster(d, n, r)) {
            accepted++;
            CHECK(!r.empty() && r.size() <= kMaxPlayers);
            CHECK(EncodeRoster(r) == bytes);
        }
        RoomState s;
        if (DecodeRoomState(d, n, s)) {
            accepted++;
            CHECK(static_cast<uint8_t>(s.phase) < kPhaseCount && s.phaseLeftMs <= s.phaseTotalMs && PoolOk(s.pool));
            CHECK(s.track == kTrackCustom ? TrackTitleOk(s.custom.title) : s.custom == CustomTrack());
            CHECK(RulesOk(s.rules) && s.aiAggression >= kAiAggressionMin && s.aiAggression <= kAiAggressionMax);
            CHECK(s.successor == kSystemId ? IsZeroIp(s.successorIp) && s.successorPort == 0
                                           : s.successor != kHostId && s.successor < kMaxPlayers && s.successorPort != 0);
            CHECK(s.code == kNoRoomCode || (s.code < kRoomCodeValues && s.type != RoomType::Lan));
            CHECK(EncodeRoomState(s) == bytes);
        }
        HandoverToken mine;
        if (DecodeHandoverToken(d, n, mine)) {
            CHECK(mine.token != 0 && EncodeHandoverToken(mine) == bytes);
        }
        std::vector<HandoverTokenEntry> held;
        Bytes32 heldSecret{};
        if (DecodeHandoverTokens(d, n, held, heldSecret)) {
            bool fine = held.size() <= kMaxPlayers;
            for (size_t i = 0; i < held.size(); i++) {
                fine = fine && held[i].id < kMaxPlayers && held[i].token != 0 && (i == 0 || held[i - 1].id < held[i].id);
            }
            CHECK(fine && EncodeHandoverTokens(held, heldSecret) == bytes);
        }
        Handover leaving;
        if (DecodeHandover(d, n, leaving)) {
            CHECK(EncodeHandover(leaving) == bytes);
        }
        Rejoin back;
        if (DecodeRejoin(d, n, back)) {
            CHECK(back.id != kHostId && back.id < kMaxPlayers && back.token != 0 && EncodeRejoin(back) == bytes);
        }
        std::vector<Ballot> v;
        if (DecodeVotes(d, n, v)) {
            accepted++;
            CHECK(v.size() <= kMaxRacers && EncodeVotes(v) == bytes);
        }
        LotteryResult l;
        if (DecodeLottery(d, n, l)) {
            accepted++;
            CHECK((l.track < kStockTracks || l.track == kTrackCustom) &&
                  (l.ballots.empty() ? l.winner == kNoWinner : l.winner < l.ballots.size()));
            CHECK(l.track != kTrackCustom || (!l.ballots.empty() && l.ballots[l.winner].custom == l.custom));
            CHECK(EncodeLottery(l) == bytes);
        }
        Picked p;
        if (DecodePicked(d, n, p)) {
            accepted++;
            CHECK((p.track < kStockTracks || (p.track == kTrackCustom && !p.drawn && p.by != kSystemId)) &&
                  EncodePicked(p) == bytes);
        }
        LockRequest k;
        if (DecodeLock(d, n, k)) {
            accepted++;
            CHECK(k.machine < kRosterMachines && k.skin < kMachineSkins && k.engine <= kEngineMax);
            CHECK(EncodeLock(k) == bytes);
        }
        TrackChoice named;
        if (DecodeVote(d, n, named)) {
            CHECK(named.track == kTrackCustom ? TrackTitleOk(named.custom.title) : TrackInPool(named.track, kPoolAll));
            CHECK(EncodeVote(named) == bytes);
        }
        EmoteLine e;
        if (DecodeEmoteLine(d, n, e)) {
            CHECK(e.from < kMaxPlayers && e.emote < kEmoteCount);
        }
        uint8_t byte = 0;
        bool flag = false;
        RejectReason why = RejectReason::Full;
        if (DecodeVote(d, n, byte)) {
            CHECK(TrackInPool(byte, kPoolAll));
        }
        if (DecodeEmote(d, n, byte)) {
            CHECK(byte < kEmoteCount);
        }
        if (DecodeWelcome(d, n, byte)) {
            CHECK(byte != kHostId && byte < kMaxPlayers);
        }
        (void) DecodeReady(d, n, flag);
        (void) DecodeReject(d, n, why);
        ReadyToRace loaded;
        if (DecodeReadyToRace(d, n, loaded)) {
            accepted++;
            CHECK(EncodeReadyToRace(loaded) == bytes);
        }
        Grid field;
        if (DecodeGrid(d, n, field)) {
            accepted++;
            CHECK(!field.humans.empty() && field.humans.size() <= kMaxRacers &&
                  field.humans.size() + field.cpus <= kGridSlots && RulesOk(field.rules));
            CHECK(field.track.track < kStockTracks || field.track.track == kTrackCustom);
            CHECK(EncodeGrid(field) == bytes);
        }
        StartSignal go;
        if (DecodeStart(d, n, go)) {
            CHECK(EncodeStart(go) == bytes);
        }
        uint32_t pingMs = 0;
        if (DecodeClockPing(d, n, pingMs)) {
            CHECK(EncodeClockPing(pingMs) == bytes);
        }
        ClockPong pong;
        if (DecodeClockPong(d, n, pong)) {
            CHECK(EncodeClockPong(pong) == bytes);
        }
        RaceState poses;
        if (DecodeState(d, n, poses)) {
            accepted++;
            bool fine = !poses.poses.empty() && poses.poses.size() <= kStatePoses;
            for (const StatePose& pose : poses.poses) {
                fine = fine && StatePoseOk(pose);
            }
            CHECK(fine && EncodeState(poses) == bytes);
        }
        FinishReport ended;
        if (DecodeFinish(d, n, ended)) {
            accepted++;
            CHECK(ended.kind == ResultKind::Finished ? ended.timeMs >= 1 && ended.timeMs <= kRaceTimeMaxMs
                                                     : ended.kind == ResultKind::Retired && ended.timeMs == 0);
            CHECK(EncodeFinish(ended) == bytes);
        }
        RaceResults table;
        if (DecodeResults(d, n, table)) {
            accepted++;
            bool fine = table.entries.size() <= kMaxRacers;
            for (const ResultEntry& entry : table.entries) {
                fine = fine && entry.id < kMaxPlayers && entry.points <= kPointsMax &&
                       (entry.kind == ResultKind::Finished) == (entry.timeMs != 0);
            }
            CHECK(fine && EncodeResults(table) == bytes);
        }
        uint32_t nonce = 0;
        RoomInfo room;
        if (DecodeRoom(d, n, nonce, room)) {
            CHECK(room.racers >= 1 && room.racers <= room.racerCap && room.racerCap <= kMaxRacers &&
                  room.watchers <= kMaxWatchers && NameOk(room.hostName));
            CHECK(EncodeRoom(nonce, room) == bytes);
        }
        (void) DecodeProbe(d, n, nonce);
    }
    CHECK(accepted > 0); // some damaged copies stay valid, so the checks above ran
}

// ---------------------------------------------------------------------------------------------------------
// Sessions

void TestJoinReadyEmotes() {
    std::printf("join, ready and preset messages\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined);
    CHECK(ana.LocalId() == 1);
    CHECK(lan.host->Roster().size() == 2 && ana.Roster().size() == 2);
    CHECK(SameRoster(ana.Roster(), lan.host->Roster()));
    const PlayerInfo* seen = FindIn(ana.Roster(), 1);
    CHECK(seen != nullptr && seen->name == "Ana" && seen->colour == 0x00FF00 && seen->machine == 3 && !seen->ready &&
          !seen->watcher && !seen->inRace);
    CHECK(HasLine(lan.host->Lines(), "Ana joined the room."));
    CHECK(ana.Lines().empty()); // the room as she found it: nobody in it just joined
    CHECK(ana.Room(lan.now) == lan.host->Room(lan.now));
    CHECK(ana.Room(lan.now).roomId == 77 && ana.Room(lan.now).racerCap == kDefaultRacers);
    CHECK(lan.AllIn(Phase::Lobby));

    ana.SetReady(true);
    lan.Tick(20);
    CHECK(FindIn(lan.host->Roster(), 1)->ready);
    CHECK(FindIn(ana.Roster(), 1)->ready);
    CHECK(lan.AllIn(Phase::Lobby)); // the host is not ready
    ana.SetReady(false);
    lan.Tick(20);
    CHECK(!FindIn(lan.host->Roster(), 1)->ready && !FindIn(ana.Roster(), 1)->ready);
    CHECK(lan.host->SetReady(true, lan.now));
    lan.Tick(20);
    CHECK(FindIn(ana.Roster(), 0)->ready);
    CHECK(lan.AllIn(Phase::Lobby));
    CHECK(lan.host->SetReady(false, lan.now));

    CHECK(ana.Emote(2, lan.now));
    lan.Tick(20);
    CHECK(LastLineIs(lan.host->Lines(), 1, EmoteText(2)));
    CHECK(LastLineIs(ana.Lines(), 1, EmoteText(2)));
    CHECK(ana.Lines().back().name == "Ana" && ana.Lines().back().colour == 0x00FF00);
    CHECK(!ana.Emote(kEmoteCount, lan.now));
    CHECK(lan.host->Emote(0, lan.now));
    CHECK(!lan.host->Emote(kEmoteCount, lan.now));
    lan.Tick(20);
    CHECK(LastLineIs(ana.Lines(), 0, EmoteText(0)) && ana.Lines().back().name == "Host");
    CHECK(LastLineIs(lan.host->Lines(), 0, EmoteText(0)));

    LobbyClient& bo = lan.AddClient("Bo", Seat::Race, kBuild, 0x0000FF);
    lan.Settle();
    CHECK(bo.GetState() == LobbyClient::State::Joined && bo.LocalId() == 2);
    CHECK(lan.host->Roster().size() == 3 && ana.Roster().size() == 3 && bo.Roster().size() == 3);
    CHECK(SameRoster(bo.Roster(), lan.host->Roster()) && SameRoster(ana.Roster(), lan.host->Roster()));
    CHECK(HasLine(ana.Lines(), "Bo joined the room."));
    CHECK(bo.Lines().empty()); // no history for late joiners

    // The ping column fills in once the host has measured the link.
    lan.Tick(300);
    const PlayerInfo* pinged = FindIn(bo.Roster(), 1);
    CHECK(pinged != nullptr && pinged->pingMs > 0 && pinged->pingMs < 500);
    CHECK(FindIn(bo.Roster(), 0)->pingMs == 0);
    CHECK(lan.host->Refused() == 0 && ana.Refused() == 0 && bo.Refused() == 0);
}

void TestEmoteRateLimit() {
    std::printf("preset message rate limit\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    for (uint8_t i = 0; i < 7; i++) {
        CHECK(ana.Emote(i, lan.now) == (i < kEmoteBurst));
    }
    lan.Tick(30);
    CHECK(LinesFrom(bo.Lines(), 1) == kEmoteBurst && LinesFrom(lan.host->Lines(), 1) == kEmoteBurst);
    CHECK(LastLineIs(bo.Lines(), 1, EmoteText(kEmoteBurst - 1)));
    CHECK(HasLine(ana.Lines(), kTooFast));
    CHECK(!HasLine(bo.Lines(), kTooFast) && !HasLine(lan.host->Lines(), kTooFast));
    CHECK(lan.host->Refused() == 0); // her own copy held the two back
    lan.Tick(320);                   // past the 5 s window
    CHECK(ana.Emote(1, lan.now));
    lan.Tick(20);
    CHECK(LastLineIs(bo.Lines(), 1, EmoteText(1)));

    // The host player is held to the same limit.
    for (uint8_t i = 0; i < 7; i++) {
        CHECK(lan.host->Emote(i, lan.now) == (i < kEmoteBurst));
    }
    lan.Tick(20);
    CHECK(LinesFrom(bo.Lines(), 0) == kEmoteBurst);
    CHECK(HasLine(lan.host->Lines(), kTooFast) && !HasLine(bo.Lines(), kTooFast));
}

void TestRejects() {
    std::printf("rejects and seats\n");
    Lan lan;
    // Another build on the same protocol joins (decision 25), and leaves again.
    LobbyClient& old = lan.AddClient("Old", Seat::Race, "1.0.9/zzz");
    lan.Settle();
    CHECK(old.GetState() == LobbyClient::State::Joined);
    CHECK(lan.host->Roster().size() == 2);
    old.Leave(lan.now);
    lan.Tick(40);
    CHECK(lan.host->Roster().size() == 1 && lan.host->PeerCount() == 0);

    // Eight race seats by default, the host's among them.
    for (int i = 0; i < 7; i++) {
        lan.AddClient("P" + std::to_string(i + 1));
    }
    lan.Settle();
    CHECK(lan.host->Roster().size() == kDefaultRacers);
    for (size_t i = 1; i < lan.clients.size(); i++) {
        CHECK(lan.clients[i]->GetState() == LobbyClient::State::Joined);
    }
    LobbyClient& late = lan.AddClient("Late");
    lan.Settle();
    CHECK(late.GetState() == LobbyClient::State::Closed);
    CHECK(late.CloseText() == RejectReasonText(RejectReason::RaceSeatsFull));
    CHECK(lan.host->Roster().size() == kDefaultRacers);

    // Two watch seats on top.
    LobbyClient& w1 = lan.AddClient("W1", Seat::Watch);
    LobbyClient& w2 = lan.AddClient("W2", Seat::Watch);
    lan.Settle();
    CHECK(w1.GetState() == LobbyClient::State::Joined && w2.GetState() == LobbyClient::State::Joined);
    CHECK(lan.host->Roster().size() == kDefaultRacers + kMaxWatchers);
    const PlayerInfo* watcher = FindIn(lan.clients[1]->Roster(), w1.LocalId());
    CHECK(watcher != nullptr && watcher->watcher && watcher->name == "W1");
    CHECK(HasLine(lan.host->Lines(), "W1 joined to watch."));
    CHECK(HasLine(lan.clients[1]->Lines(), "W2 joined to watch."));
    LobbyClient& w3 = lan.AddClient("W3", Seat::Watch);
    LobbyClient& late2 = lan.AddClient("Late2");
    lan.Settle();
    CHECK(w3.GetState() == LobbyClient::State::Closed && w3.CloseText() == RejectReasonText(RejectReason::Full));
    CHECK(late2.GetState() == LobbyClient::State::Closed);
    CHECK(late2.CloseText() == RejectReasonText(RejectReason::Full));

    // The host raises the cap to twelve: four more race, the thirteenth is refused.
    CHECK(!lan.host->SetRacerCap(13, lan.now));
    CHECK(lan.host->SetRacerCap(12, lan.now));
    for (int i = 0; i < 4; i++) {
        lan.AddClient("R" + std::to_string(i + 1));
    }
    lan.Settle();
    CHECK(lan.host->Roster().size() == kMaxPlayers);
    CHECK(lan.clients.back()->GetState() == LobbyClient::State::Joined);
    CHECK(w1.Room(lan.now).racerCap == 12 && SameRoster(w1.Roster(), lan.host->Roster()));
    LobbyClient& thirteenth = lan.AddClient("Thirteen");
    lan.Settle();
    CHECK(thirteenth.GetState() == LobbyClient::State::Closed);
    CHECK(thirteenth.CloseText() == RejectReasonText(RejectReason::Full));
    CHECK(lan.host->Roster().size() == kMaxPlayers);
    CHECK(!lan.host->SetRacerCap(8, lan.now)); // twelve racers are seated

    // Kick: the player hears why, everyone else sees the seat free up.
    LobbyClient& p3 = *lan.clients[3];
    const uint8_t p3Id = p3.LocalId();
    lan.host->Kick(p3Id, lan.now);
    lan.Tick(60);
    CHECK(p3.GetState() == LobbyClient::State::Closed);
    CHECK(p3.CloseText() == RejectReasonText(RejectReason::Kicked));
    CHECK(FindIn(lan.host->Roster(), p3Id) == nullptr);
    CHECK(FindIn(lan.clients[1]->Roster(), p3Id) == nullptr);
    CHECK(HasLine(lan.clients[1]->Lines(), "P3 left the room."));
    CHECK(HasLine(lan.host->Lines(), "P3 left the room."));
    lan.host->Kick(kHostId, lan.now); // the host cannot kick itself
    lan.Tick(5);
    CHECK(FindIn(lan.host->Roster(), kHostId) != nullptr);

    // With a race seat free again, a watcher is refused for the right reason, and a racer gets the freed id.
    LobbyClient& w4 = lan.AddClient("W4", Seat::Watch);
    LobbyClient& again = lan.AddClient("Again");
    lan.Settle();
    CHECK(w4.GetState() == LobbyClient::State::Closed);
    CHECK(w4.CloseText() == RejectReasonText(RejectReason::WatchSeatsFull));
    CHECK(again.GetState() == LobbyClient::State::Joined && again.LocalId() == p3Id);
    CHECK(HasLine(lan.clients[1]->Lines(), "Again joined the room."));
}

void TestLeaveVanishAndClose() {
    std::printf("leave, vanish and close\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& cy = lan.AddClient("Cy");
    lan.Settle();
    CHECK(lan.host->Roster().size() == 4);

    ana.Leave(lan.now);
    lan.Tick(20);
    CHECK(ana.GetState() == LobbyClient::State::Closed && ana.CloseText().empty());
    CHECK(lan.host->Roster().size() == 3 && bo.Roster().size() == 3);
    CHECK(HasLine(bo.Lines(), "Ana left the room."));
    CHECK(HasLine(lan.host->Lines(), "Ana left the room."));

    // Bo's machine drops off the network: the host frees the seat after kLinkTimeoutMs of silence. The last
    // packet from Bo is at most one keepalive (1 s) older than the cut.
    lan.net.Cut(lan.clientAddrs[1]);
    lan.Tick(120); // 1.9 s
    CHECK(lan.host->Roster().size() == 3 && bo.GetState() == LobbyClient::State::Joined);
    lan.Tick(100); // 3.5 s
    CHECK(lan.host->Roster().size() == 2 && cy.Roster().size() == 2);
    CHECK(HasLine(cy.Lines(), "Bo left the room."));
    CHECK(bo.GetState() == LobbyClient::State::Closed && bo.CloseText() == "Lost the connection to the host.");

    lan.host->Close(lan.now);
    lan.Tick(20);
    CHECK(cy.GetState() == LobbyClient::State::Closed);
    CHECK(cy.CloseText() == "The host closed the room.");
    CHECK(lan.host->PeerCount() == 0);
    CHECK(!lan.host->Start(lan.now)); // a closed room takes no more calls
}

void TestNoHost() {
    std::printf("no host\n");
    FakeNet net;
    uint32_t now = 5000;
    const NetAddress me = Addr(Ip(10, 0, 0, 2), 40000);
    LobbyClient client(kBuild, Profile("Ana", 1, 0xFF), Seat::Race, Addr(Ip(10, 0, 0, 99), kDefaultPort), 42,
                       net.SenderFor(me));
    net.Add(me, [&](const NetAddress& from, const uint8_t* d, size_t n) { client.Receive(from, d, n, now); });
    client.Start(now);
    const auto run = [&](int frames) {
        for (int i = 0; i < frames; i++) {
            now += 16;
            net.Step();
            client.Update(now);
        }
    };
    run(150); // 2.4 s: still asking
    CHECK(client.GetState() == LobbyClient::State::Connecting);
    run(60); // 3.4 s
    CHECK(client.GetState() == LobbyClient::State::Closed);
    CHECK(client.CloseText() == "No room answered at 10.0.0.99:24302.");
    CHECK(!client.Emote(0, now));
}

// A hello as a client's channel builds it: magic, type 0, nonce, version and cookie, little-endian.
std::vector<uint8_t> HelloPacket(uint32_t nonce, uint64_t cookie, uint16_t version = kProtocolVersion) {
    std::vector<uint8_t> b = { 'G', 'D', 0 };
    for (int i = 0; i < 4; i++) {
        b.push_back(static_cast<uint8_t>(nonce >> (8 * i)));
    }
    b.push_back(static_cast<uint8_t>(version));
    b.push_back(static_cast<uint8_t>(version >> 8));
    for (int i = 0; i < 8; i++) {
        b.push_back(static_cast<uint8_t>(cookie >> (8 * i)));
    }
    return b;
}

void TestHostIgnoresStrangers() {
    std::printf("host ignores strangers\n");
    Lan lan;
    const uint8_t junk[] = { 1, 2, 3, 4, 5 };
    lan.host->Receive(Addr(Ip(10, 1, 1, 1), 1), junk, sizeof(junk), lan.now);
    CHECK(lan.host->PeerCount() == 0 && lan.host->Refused() == 1);
    // A flood of hellos with no cookie, as from spoofed addresses, takes no slot at all (decision 34).
    const std::vector<uint8_t> hello = HelloPacket(0x10, 0);
    for (int i = 0; i < 40; i++) {
        lan.host->Receive(Addr(Ip(10, 2, 0, i), 9999), hello.data(), hello.size(), lan.now);
    }
    CHECK(lan.host->PeerCount() == 0 && lan.host->Challenged() == 40);
    CHECK(lan.host->Roster().size() == 1);
    // A probe gets one room reply back.
    size_t before = lan.net.Sent();
    const std::vector<uint8_t> probe = EncodeProbe(9);
    lan.host->Receive(Addr(Ip(10, 3, 0, 1), 7), probe.data(), probe.size(), lan.now);
    CHECK(lan.net.Sent() == before + 1);
    // A probe from the version 1 lobby gets none.
    std::vector<uint8_t> oldProbe = probe;
    oldProbe[5] = 1;
    before = lan.net.Sent();
    const uint32_t refused = lan.host->Refused();
    lan.host->Receive(Addr(Ip(10, 3, 0, 1), 7), oldProbe.data(), oldProbe.size(), lan.now);
    CHECK(lan.net.Sent() == before && lan.host->Refused() == refused + 1);
}

// The stateless gate (decision 34): a hello opens a pending slot only when it echoes the cookie this host gave its
// address, and no reply to a stranger is larger than what the stranger sent.
void TestHelloGate() {
    std::printf("hello gate\n");
    Lan lan;
    // The longest datagram the host sent to a 10.x address that had not echoed a cookie, and how many it sent. An
    // address that echoed one has proved it hears the host, so its link may get more than it sent (the Nonce).
    size_t largest = 0;
    size_t strangers = 0;
    std::set<uint32_t> proved;
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t len) {
        if (from == lan.hostAddr && (to.Ipv4() >> 24) == 10 && proved.count(to.Ipv4()) == 0) {
            largest = std::max(largest, len);
            strangers++;
        }
        return false;
    });
    // Addresses that can hear the host keep the last datagram it sent them.
    std::map<uint32_t, std::vector<uint8_t>> heard;
    for (int i = 0; i < 40; i++) {
        const NetAddress addr = Addr(Ip(10, 9, 0, i), 7000);
        lan.net.Add(addr, [&heard, addr](const NetAddress&, const uint8_t* d, size_t n) {
            heard[addr.Ipv4()].assign(d, d + n);
        });
    }
    auto cookieFrom = [](const std::vector<uint8_t>& challenge) {
        CHECK(challenge.size() == kHelloBytes && challenge[2] == 5);
        uint64_t cookie = 0;
        for (size_t b = 0; b < 8 && 9 + b < challenge.size(); b++) {
            cookie |= static_cast<uint64_t>(challenge[9 + b]) << (8 * b);
        }
        return cookie;
    };
    const NetAddress a = Addr(Ip(10, 9, 0, 0), 7000);
    const NetAddress b = Addr(Ip(10, 9, 0, 1), 7000);
    const std::vector<uint8_t> first = HelloPacket(0x55, 0);
    lan.host->Receive(a, first.data(), first.size(), lan.now);
    lan.net.Step();
    CHECK(lan.host->PeerCount() == 0 && lan.host->Challenged() == 1);
    CHECK(heard.count(a.Ipv4()) == 1);
    const uint64_t cookie = cookieFrom(heard[a.Ipv4()]);

    // The cookie is bound to the address and the nonce: another address, another nonce or a guess gets a new
    // challenge, never a slot.
    const std::vector<uint8_t> stolen = HelloPacket(0x55, cookie);
    lan.host->Receive(b, stolen.data(), stolen.size(), lan.now);
    const std::vector<uint8_t> otherNonce = HelloPacket(0x56, cookie);
    lan.host->Receive(a, otherNonce.data(), otherNonce.size(), lan.now);
    const std::vector<uint8_t> guess = HelloPacket(0x55, cookie ^ 1);
    lan.host->Receive(a, guess.data(), guess.size(), lan.now);
    CHECK(lan.host->PeerCount() == 0 && lan.host->Challenged() == 4);

    // A channel packet that is not a hello, and a hello of the wrong length, get nothing back.
    const size_t before = lan.net.Sent();
    const uint32_t refused = lan.host->Refused();
    std::vector<uint8_t> data = HelloPacket(0x55, cookie);
    data[2] = 3;
    lan.host->Receive(a, data.data(), data.size(), lan.now);
    std::vector<uint8_t> shortHello = HelloPacket(0x55, cookie);
    shortHello.resize(9);
    lan.host->Receive(a, shortHello.data(), shortHello.size(), lan.now);
    CHECK(lan.net.Sent() == before && lan.host->Refused() == refused + 2 && lan.host->PeerCount() == 0);

    // The real cookie, from the address it was given to, takes a slot. It is still good one bucket later.
    lan.now += kCookieBucketMs;
    const std::vector<uint8_t> echo = HelloPacket(0x55, cookie);
    proved.insert(a.Ipv4());
    lan.host->Receive(a, echo.data(), echo.size(), lan.now);
    CHECK(lan.host->PeerCount() == 1);
    lan.Tick(static_cast<int>(kPendingPeerMs / 16) + 2);
    CHECK(lan.host->PeerCount() == 0);

    // Two buckets later it is stale.
    lan.now += 2 * kCookieBucketMs;
    lan.host->Receive(a, echo.data(), echo.size(), lan.now);
    CHECK(lan.host->PeerCount() == 0);

    // Forty addresses that each echo a real cookie still hold at most kPendingPeerCap slots, and those expire.
    heard.clear();
    for (int i = 0; i < 40; i++) {
        const std::vector<uint8_t> h = HelloPacket(0x100u + static_cast<uint32_t>(i), 0);
        lan.host->Receive(Addr(Ip(10, 9, 0, i), 7000), h.data(), h.size(), lan.now);
    }
    lan.net.Step();
    CHECK(heard.size() == 40);
    for (int i = 0; i < 40; i++) {
        const NetAddress addr = Addr(Ip(10, 9, 0, i), 7000);
        const std::vector<uint8_t> h = HelloPacket(0x100u + static_cast<uint32_t>(i), cookieFrom(heard[addr.Ipv4()]));
        proved.insert(addr.Ipv4());
        lan.host->Receive(addr, h.data(), h.size(), lan.now);
    }
    CHECK(lan.host->PeerCount() == kPendingPeerCap);
    lan.Tick(static_cast<int>(kPendingPeerMs / 16) + 2);
    CHECK(lan.host->PeerCount() == 0 && lan.host->Roster().size() == 1);

    // One machine sending from ten ports holds at most kPendingPerSourceCap of those slots; another address still
    // gets one.
    heard.clear();
    std::map<uint16_t, std::vector<uint8_t>> heardPort;
    for (uint16_t port = 8000; port < 8010; port++) {
        const NetAddress addr = Addr(Ip(10, 9, 1, 1), port);
        lan.net.Add(addr, [&heardPort, port](const NetAddress&, const uint8_t* d, size_t n) {
            heardPort[port].assign(d, d + n);
        });
        const std::vector<uint8_t> h = HelloPacket(0x200u + port, 0);
        lan.host->Receive(addr, h.data(), h.size(), lan.now);
    }
    lan.net.Step();
    CHECK(heardPort.size() == 10);
    proved.insert(Ip(10, 9, 1, 1));
    for (uint16_t port = 8000; port < 8010; port++) {
        const std::vector<uint8_t> h = HelloPacket(0x200u + port, cookieFrom(heardPort[port]));
        lan.host->Receive(Addr(Ip(10, 9, 1, 1), port), h.data(), h.size(), lan.now);
    }
    CHECK(lan.host->PeerCount() == kPendingPerSourceCap);
    {
        const NetAddress other = Addr(Ip(10, 9, 0, 3), 7000);
        const std::vector<uint8_t> h = HelloPacket(0x300, 0);
        lan.host->Receive(other, h.data(), h.size(), lan.now);
        lan.net.Step();
        const std::vector<uint8_t> e = HelloPacket(0x300, cookieFrom(heard[other.Ipv4()]));
        lan.host->Receive(other, e.data(), e.size(), lan.now);
        CHECK(lan.host->PeerCount() == kPendingPerSourceCap + 1);
    }
    lan.Tick(static_cast<int>(kPendingPeerMs / 16) + 2);
    CHECK(lan.host->PeerCount() == 0);

    // A hello on another protocol version gets a refusal, no larger than the hello, and no slot.
    const std::vector<uint8_t> old = HelloPacket(0x77, 0, 4);
    lan.host->Receive(a, old.data(), old.size(), lan.now);
    lan.net.Step();
    CHECK(heard[a.Ipv4()].size() == 10 && heard[a.Ipv4()][2] == 2 && lan.host->PeerCount() == 0);

    // Nothing the host sent to a stranger was larger than a hello.
    CHECK(strangers > 0 && largest == kHelloBytes);

    // A real client gets through the challenge with one extra round trip.
    LobbyClient& ana = lan.AddClient("Ana");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined);
}

// A probe is as long as the longest room reply, so a forged probe cannot make a host send more than it got.
void TestProbeNotAmplified() {
    std::printf("probe not amplified\n");
    RoomInfo room;
    room.port = 24302;
    room.racers = 1;
    room.racerCap = kDefaultRacers;
    room.build = std::string(kBuildMax, 'b');
    room.hostName = std::string(kNameMax, 'n');
    const std::vector<uint8_t> probe = EncodeProbe(3);
    CHECK(probe.size() == kProbeBytes && EncodeRoom(3, room).size() == kProbeBytes);
    uint32_t nonce = 0;
    CHECK(DecodeProbe(probe.data(), probe.size(), nonce) && nonce == 3);
    // The version 4 probe had no padding; padding that is not zero is refused too.
    std::vector<uint8_t> bare(probe.begin(), probe.begin() + 11);
    CHECK(!DecodeProbe(bare.data(), bare.size(), nonce));
    std::vector<uint8_t> dirty = probe;
    dirty.back() = 1;
    CHECK(!DecodeProbe(dirty.data(), dirty.size(), nonce));
    Lan lan;
    const size_t before = lan.net.Sent();
    lan.host->Receive(Addr(Ip(10, 3, 0, 1), 7), bare.data(), bare.size(), lan.now);
    CHECK(lan.net.Sent() == before);

    // A LAN room answers any probe. A directory room answers this network only: a scanner on a public address
    // learns nothing.
    const NetAddress local = Addr(Ip(192, 168, 1, 99), 7);
    const NetAddress scanner = Addr(Ip(203, 0, 113, 9), 7);
    lan.net.Add(local, [](const NetAddress&, const uint8_t*, size_t) {});
    lan.net.Add(scanner, [](const NetAddress&, const uint8_t*, size_t) {});
    size_t sent = lan.net.Sent();
    lan.host->Receive(scanner, probe.data(), probe.size(), lan.now);
    CHECK(lan.net.Sent() == sent + 1);
    Lan open("Host", 79, {}, TrackStore(), RoomType::Open);
    open.net.Add(local, [](const NetAddress&, const uint8_t*, size_t) {});
    open.net.Add(scanner, [](const NetAddress&, const uint8_t*, size_t) {});
    sent = open.net.Sent();
    open.host->Receive(scanner, probe.data(), probe.size(), open.now);
    CHECK(open.net.Sent() == sent);
    open.host->Receive(local, probe.data(), probe.size(), open.now);
    CHECK(open.net.Sent() == sent + 1);
}

// L2 (decision 46): the host's upload totals add up its links' counters and keep those of a dropped link.
// O6.1, O6.4 and O5.10 on the wire (protocol 7): the room type, the lottery commitment and its reveal, and the
// takeover secret beside the token table.
void TestRoomTypeWire() {
    std::printf("codec: room type, lottery commitment, takeover secret (protocol 7)\n");
    for (uint8_t t = 0; t < kRoomTypeCount; t++) {
        CHECK(RoomTypeName(static_cast<RoomType>(t))[0] != '\0');
    }
    RoomState open;
    open.type = RoomType::Open;
    open.commitment[0] = 0xC0;
    open.commitment[31] = 0x01;
    std::vector<uint8_t> v = EncodeRoomState(open);
    RoomState out;
    CHECK(v.size() == EncodeRoomState(RoomState()).size());
    CHECK(DecodeRoomState(v.data(), v.size(), out) && out == open && out.type == RoomType::Open);
    // Only an Open room commits.
    for (RoomType type : { RoomType::Lan, RoomType::Code, RoomType::Friends }) {
        RoomState other = open;
        other.type = type;
        v = EncodeRoomState(other);
        CHECK(!DecodeRoomState(v.data(), v.size(), out));
        other.commitment = Bytes32{};
        v = EncodeRoomState(other);
        CHECK(DecodeRoomState(v.data(), v.size(), out) && out == other);
    }
    // The type byte sits right before the commitment, the code and the course choice, and only known types
    // decode.
    v = EncodeRoomState(RoomState());
    CHECK(v[v.size() - 38] == 0);
    for (uint8_t bad : { kRoomTypeCount, uint8_t{ 0x80 }, uint8_t{ 0xFF } }) {
        v[v.size() - 38] = bad;
        CHECK(!DecodeRoomState(v.data(), v.size(), out));
    }
    v[v.size() - 38] = static_cast<uint8_t>(RoomType::Friends);
    CHECK(DecodeRoomState(v.data(), v.size(), out) && out.type == RoomType::Friends);

    // The room code: the 4 bytes before the course choice. None in any room; a code value only in a directory
    // room.
    CHECK(RoomState().code == kNoRoomCode);
    v = EncodeRoomState(RoomState());
    CHECK(v[v.size() - 2] == 0xFF && v[v.size() - 5] == 0xFF);
    for (RoomType type : { RoomType::Code, RoomType::Open, RoomType::Friends }) {
        for (uint32_t code : { 0u, 12345u, kRoomCodeValues - 1 }) {
            RoomState listed;
            listed.type = type;
            listed.code = code;
            v = EncodeRoomState(listed);
            CHECK(DecodeRoomState(v.data(), v.size(), out) && out == listed && out.code == code);
        }
        RoomState tooBig;
        tooBig.type = type;
        tooBig.code = kRoomCodeValues;
        v = EncodeRoomState(tooBig);
        CHECK(!DecodeRoomState(v.data(), v.size(), out));
    }
    RoomState lanCode;
    lanCode.code = 7;
    v = EncodeRoomState(lanCode);
    CHECK(!DecodeRoomState(v.data(), v.size(), out));

    // The lottery reveal: 32 bytes at the end, any value.
    LotteryResult drawn;
    drawn.track = 3;
    const size_t bare = EncodeLottery(drawn).size();
    drawn.salt[5] = 0x77;
    v = EncodeLottery(drawn);
    LotteryResult drawnOut;
    CHECK(v.size() == bare && DecodeLottery(v.data(), v.size(), drawnOut) && drawnOut == drawn &&
          drawnOut.salt[5] == 0x77);
    v.pop_back();
    CHECK(!DecodeLottery(v.data(), v.size(), drawnOut));

    // The takeover secret rides after the token table; a short one does not decode.
    Bytes32 secret{};
    secret[0] = 0xAB;
    secret[31] = 0xCD;
    const std::vector<HandoverTokenEntry> tokens = { { 1, 5 }, { 3, 6 } };
    v = EncodeHandoverTokens(tokens, secret);
    std::vector<HandoverTokenEntry> tokensOut;
    Bytes32 secretOut{};
    CHECK(v.size() == 1 + 2 * 9 + 32 + 2 && DecodeHandoverTokens(v.data(), v.size(), tokensOut, secretOut) &&
          tokensOut == tokens && secretOut == secret);
    v.pop_back();
    Bytes32 untouched{};
    untouched[1] = 9;
    CHECK(!DecodeHandoverTokens(v.data(), v.size(), tokensOut, untouched) && untouched[1] == 9 && untouched[0] == 0);
}

// O5.10: the host's takeover secret reaches the successor alone, and again when it changes.
void TestTakeoverSecret() {
    std::printf("the takeover secret goes to the successor only (O5.10)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.Settle();
    CHECK(lan.host->Room(lan.now).successor == 1 && IsZero32(lan.clients[0]->TakeoverSecret()));
    Bytes32 secret{};
    secret[0] = 0x5A;
    secret[31] = 0xA5;
    lan.host->SetTakeoverSecret(secret);
    lan.Tick(20);
    CHECK(lan.clients[0]->TakeoverSecret() == secret && IsZero32(lan.clients[1]->TakeoverSecret()));
    secret[1] = 0x11;
    lan.host->SetTakeoverSecret(secret);
    lan.Tick(20);
    CHECK(lan.clients[0]->TakeoverSecret() == secret && IsZero32(lan.clients[1]->TakeoverSecret()));
    CHECK(!lan.host->TakeSecretExposed()); // a new secret for the same successor exposes nothing

    // The successor who holds the secret is kicked: it still knows the secret, so the host is told to replace
    // it, once. The new successor gets the table at once, and the new secret when the listing hands it over.
    lan.host->Kick(lan.clients[0]->LocalId(), lan.now);
    lan.Tick(20);
    CHECK(lan.host->Room(lan.now).successor == lan.clients[1]->LocalId());
    CHECK(lan.host->TakeSecretExposed());
    CHECK(!lan.host->TakeSecretExposed());
    secret[2] = 0x22;
    lan.host->SetTakeoverSecret(secret);
    lan.Tick(20);
    CHECK(lan.clients[1]->TakeoverSecret() == secret);
    CHECK(!lan.host->TakeSecretExposed());
}

void TestUploadTotals() {
    std::printf("upload totals (L2)\n");
    Lan lan;
    CHECK(lan.host->Upload().bytes == 0 && lan.host->Upload().links == 0);
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined && bo.GetState() == LobbyClient::State::Joined);
    const LobbyHost::UploadTotals two = lan.host->Upload();
    CHECK(two.links == 2 && two.bytes > 0 && two.datagrams > 0);
    // Every datagram carries at least the IPv4 header estimate.
    CHECK(two.bytes >= two.datagrams * kUdpIpv4HeaderBytes);
    ana.Leave(lan.now);
    lan.Tick(40);
    const LobbyHost::UploadTotals one = lan.host->Upload();
    CHECK(one.links == 1 && one.bytes > two.bytes && one.datagrams > two.datagrams);
}

void TestOldVersionRefused() {
    std::printf("version 1 peer\n");
    Lan lan;
    RawPeer old(lan, 200, 1);
    old.Run(40);
    CHECK(old.channel->State() == LinkState::Disconnected);
    CHECK(old.channel->Reason() == CloseReason::VersionMismatch);
    CHECK(old.got.empty() && lan.host->Roster().size() == 1);
    lan.Tick(400);
    CHECK(lan.host->PeerCount() == 0);

    // The same peer on this version gets in.
    RawPeer current(lan, 201, kProtocolVersion);
    current.Run(20);
    CHECK(current.channel->State() == LinkState::Connected);
}

void TestHostChecksEveryMessage() {
    std::printf("host checks every message\n");
    Lan lan;
    RawPeer raw(lan, 210, kProtocolVersion);
    raw.Run(20);
    CHECK(raw.channel->State() == LinkState::Connected);

    // Before the join, only a Join is heard.
    raw.Send(kMsgReady, EncodeReady(true));
    raw.Run(10);
    CHECK(lan.host->Refused() == 1 && lan.host->Roster().size() == 1);
    // Nor is its clock ping answered.
    const std::vector<uint8_t> early = EncodeClockPing(7);
    CHECK(raw.channel->SendUnreliable(kMsgClockPing, early.data(), early.size()) == QueueResult::Ok);
    raw.Run(6);
    CHECK(lan.host->Refused() == 2 && raw.Count(kMsgClockPong) == 0);

    JoinRequest join;
    join.build = kBuild;
    join.name = "Raw";
    join.colour = 0x445566;
    join.machine = 2;
    raw.Send(kMsgJoin, EncodeJoin(join));
    raw.Run(10);
    uint8_t id = 0;
    const Message* welcome = raw.Last(kMsgWelcome);
    CHECK(welcome != nullptr && DecodeWelcome(welcome->payload.data(), welcome->payload.size(), id) && id == 1);
    CHECK(raw.Count(kMsgRoster) >= 1 && raw.Count(kMsgRoom) >= 1);
    // The Nonce comes first, at the link; then the id, before the roster.
    CHECK(raw.got.size() >= 2 && raw.got[0].type == kMsgNonce && raw.got[1].type == kMsgWelcome);
    CHECK(lan.host->Roster().size() == 2);

    uint32_t refused = lan.host->Refused();
    const auto refusedAfter = [&](uint8_t type, const std::vector<uint8_t>& payload) {
        raw.Send(type, payload);
        raw.Run(6);
        const bool grew = lan.host->Refused() == refused + 1;
        refused = lan.host->Refused();
        return grew;
    };
    CHECK(refusedAfter(kMsgJoin, EncodeJoin(join)));       // a second join
    CHECK(refusedAfter(6, { 'h', 'i' }));                   // version 1 typed chat
    CHECK(refusedAfter(kMsgVote, EncodeVote(3)));           // no vote is open
    CHECK(refusedAfter(kMsgPick, EncodePick(3)));           // nor a pick
    CHECK(refusedAfter(kMsgLock, EncodeLock(LockOf(1, 0, 50)))); // nor the lock-in screen
    CHECK(refusedAfter(kMsgLock, { 1, 0 }));                // truncated
    CHECK(refusedAfter(kMsgReadyToRace, EncodeReadyToRace(ReadyToRace()))); // no grid to be ready for
    CHECK(refusedAfter(kMsgReadyToRace, {}));
    CHECK(refusedAfter(kMsgReadyToRace, { 0 }));
    CHECK(refusedAfter(kMsgGrid, EncodeGrid(SampleGrid())));  // a host message from a client
    CHECK(refusedAfter(kMsgStart, EncodeStart({ 0, 5000 })));
    CHECK(refusedAfter(kMsgClockPing, EncodeClockPing(1)));   // a ping sent the reliable way
    CHECK(refusedAfter(kMsgSeatNext, EncodeSeatNext(true))); // a racer already has a seat
    CHECK(refusedAfter(kMsgEmote, { static_cast<uint8_t>(kEmoteCount) }));
    CHECK(refusedAfter(kMsgRoster, EncodeRoster(lan.host->Roster()))); // a host message from a client
    CHECK(refusedAfter(kMsgWelcome, EncodeWelcome(1)));
    CHECK(refusedAfter(kMsgResults, EncodeResults(RaceResults()))); // a host message from a client
    CHECK(refusedAfter(kMsgFinish, EncodeFinish({ 0, ResultKind::Retired, 0 }))); // no race is run
    CHECK(refusedAfter(kMsgFinish, { 0, 0, 0 }));                  // truncated
    CHECK(refusedAfter(kMsgFinish, { 0, 0, 1, 0, 0, 0, 0 }));      // nobody reports that they are still driving
    CHECK(refusedAfter(200, {}));
    const std::vector<uint8_t> ready = EncodeReady(true);
    CHECK(raw.channel->SendUnreliable(kMsgReady, ready.data(), ready.size()) == QueueResult::Ok);
    raw.Run(6);
    CHECK(lan.host->Refused() == refused + 1 && !FindIn(lan.host->Roster(), 1)->ready); // room traffic is reliable
    refused = lan.host->Refused();

    // The clock ping is the one message heard the other way. It is answered at once with the host clock.
    const std::vector<uint8_t> ping = EncodeClockPing(0xCAFE0001u);
    CHECK(raw.channel->SendUnreliable(kMsgClockPing, ping.data(), ping.size()) == QueueResult::Ok);
    raw.Run(6);
    ClockPong pong;
    const Message* answer = raw.Last(kMsgClockPong);
    CHECK(answer != nullptr && !answer->reliable &&
          DecodeClockPong(answer->payload.data(), answer->payload.size(), pong));
    CHECK(pong.clientMs == 0xCAFE0001u && lan.now - pong.hostMs <= 6 * 16 && lan.host->Refused() == refused);
    const uint8_t cutPing[] = { 1, 2, 3 };
    CHECK(raw.channel->SendUnreliable(kMsgClockPing, cutPing, sizeof(cutPing)) == QueueResult::Ok);
    raw.Run(6);
    CHECK(lan.host->Refused() == refused + 1 && raw.Count(kMsgClockPong) == 1);
    refused = lan.host->Refused();
    CHECK(lan.host->Roster().size() == 2 && lan.HostPhase() == Phase::Lobby);

    // A peer that skips its own rate limit is held to the host's: five pass, two are dropped without a word.
    for (uint8_t i = 0; i < 7; i++) {
        raw.Send(kMsgEmote, EncodeEmote(i));
    }
    raw.Run(10);
    CHECK(LinesFrom(lan.host->Lines(), 1) == kEmoteBurst && raw.Count(kMsgEmoteLine) == kEmoteBurst);
    CHECK(lan.host->Refused() == refused + 2);
    CHECK(!HasLine(lan.host->Lines(), kTooFast));

    // A join that breaks the profile rule is refused in words, and the link is dropped.
    RawPeer bad(lan, 211, kProtocolVersion);
    bad.Run(20);
    join.name = " x";
    bad.Send(kMsgJoin, EncodeJoin(join));
    bad.Run(10);
    RejectReason why = RejectReason::Full;
    const Message* reject = bad.Last(kMsgReject);
    CHECK(reject != nullptr && DecodeReject(reject->payload.data(), reject->payload.size(), why) &&
          why == RejectReason::BadProfile);
    bad.Run(40);
    CHECK(bad.channel->State() == LinkState::Disconnected && bad.channel->Reason() == CloseReason::Closed);
    CHECK(lan.host->Roster().size() == 2 && lan.host->PeerCount() == 1);
}

void TestVoteFlow() {
    std::printf("vote, lottery, lock-in and race over the network\n");
    Lan lan("Host", 0xBEEF);
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& cy = lan.AddClient("Cy");
    LobbyClient& wim = lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    CHECK(ana.LocalId() == 1 && bo.LocalId() == 2 && cy.LocalId() == 3 && wim.LocalId() == 4);

    // Four racers: a vote.
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Voting));
    const RoomState voting = cy.Room(lan.now);
    CHECK(voting.roomId == 0xBEEF && voting.raceIndex == 0 && voting.phaseTotalMs == kVoteMs);
    CHECK(Apart(voting.phaseLeftMs, lan.host->Room(lan.now).phaseLeftMs) <= 64); // the timer runs on each copy
    CHECK(voting.phaseLeftMs < kVoteMs && voting.phaseLeftMs > kVoteMs - 1000);
    ana.Vote(3);
    bo.Vote(kTrackRandom);
    wim.Vote(5); // a watcher has no ballot
    lan.Tick(10);
    CHECK(lan.host->Votes().size() == 2 && lan.host->Refused() == 1);
    CHECK(cy.Votes() == lan.host->Votes() && wim.Votes() == lan.host->Votes());
    CHECK(cy.Votes() == (std::vector<Ballot>{ { 1, 3 }, { 2, kTrackRandom } }));
    ana.Vote(9); // a ballot is final
    lan.Tick(10);
    CHECK(lan.host->Votes().size() == 2 && lan.host->Refused() == 2);
    CHECK(lan.AllIn(Phase::Voting));

    // The last ballot closes the vote. Every copy holds the same result and has checked it.
    CHECK(lan.host->Vote(9, lan.now));
    cy.Vote(3);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Lottery));
    const LotteryResult result = lan.host->Lottery();
    CHECK(result.ballots.size() == 4 && result.winner < 4 && result.track < kStockTracks);
    CHECK(result == RunLottery(0xBEEF, 0, lan.host->Votes(), kPoolAll, kTrackNone));
    for (const auto& c : lan.clients) {
        CHECK(c->Lottery() == result && c->LotteryChecked());
        CHECK(c->Room(lan.now).track == result.track && c->Room(lan.now).phaseTotalMs == kLotteryMs);
    }

    // Somebody who joins while the result is shown sees it too, and can check it.
    LobbyClient& dee = lan.AddClient("Dee");
    lan.Settle();
    CHECK(dee.GetState() == LobbyClient::State::Joined && dee.Lottery() == result && dee.LotteryChecked());
    CHECK(lan.AllIn(Phase::Lottery));

    CHECK(lan.host->SkipLottery(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    // The grid is fixed when the lock-in screen opens: Dee had a race seat by then, so she races the track
    // the others voted for.
    CHECK(dee.LocalId() == 5);
    CHECK(ana.Room(lan.now).racers == 5 && ana.Room(lan.now).locked == 0);
    CHECK(FindIn(ana.Roster(), 1)->inRace && !FindIn(ana.Roster(), wim.LocalId())->inRace);
    CHECK(FindIn(ana.Roster(), dee.LocalId())->inRace);

    // The lock-in screen: each lock shows on every copy, and the last one closes the screen.
    ana.Lock(LockOf(5, 2, 80));
    lan.Tick(10);
    const PlayerInfo* locked = FindIn(cy.Roster(), 1);
    CHECK(locked != nullptr && locked->locked && locked->machine == 5 && locked->skin == 2);
    CHECK(cy.Room(lan.now).locked == 1 && wim.Room(lan.now).locked == 1);
    ana.Lock(LockOf(6, 0, 10)); // final
    wim.Lock(LockOf(1, 0, 50)); // not racing
    bo.Lock(LockOf(1, 1, 50));
    cy.Lock(LockOf(1, 3, 100));
    dee.Lock(LockOf(2, 0, 40));
    lan.Tick(10);
    CHECK(lan.host->Refused() == 4 && FindIn(lan.host->Roster(), 1)->machine == 5);
    CHECK(lan.AllIn(Phase::Machine) && bo.Room(lan.now).locked == 4);
    CHECK(lan.host->Lock(LockOf(0, 0, 0), lan.now));
    CHECK(lan.HostPhase() == Phase::GridSync);

    // Every copy gets the grid, the watcher too. Nothing starts until the racers have the track.
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::GridSync));
    Grid grid;
    CHECK(cy.RaceGrid(grid) && grid == lan.host->RaceGrid());
    CHECK(wim.RaceGrid(grid) && grid == lan.host->RaceGrid());
    CHECK(grid.raceIndex == 0 && grid.humans.size() == 5 && grid.cpus == kGridSlots - 5);
    StartSignal start;
    CHECK(!cy.StartSent(start) && !lan.host->StartSent(start));

    // Every racer reports the track loaded, and the countdown starts on one start time.
    lan.LoadAll();
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    StartSignal hostStart;
    CHECK(lan.host->StartSent(hostStart) && hostStart.raceIndex == 0);
    CHECK(cy.StartSent(start) && start == hostStart);
    CHECK(wim.StartSent(start) && start == hostStart);
    CHECK(wim.Room(lan.now).racers == 5 && SameRoster(wim.Roster(), lan.host->Roster()));
    CHECK(!lan.host->HostLeftRace(lan.now)); // no race to leave yet
    lan.Tick(510);                           // 8.2 s
    CHECK(lan.AllIn(Phase::Racing));
    lan.CapRace({ 2, 1, 3, 0 }); // an order that leaves Dee out ranks her last
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Results));
    CHECK(lan.host->LastOrder() == (std::vector<uint8_t>{ 2, 1, 3, 0, 5 }));

    // The results time out into the next race: still four or more racers, so another vote, on the next index.
    lan.Tick(760); // 12.2 s
    CHECK(lan.AllIn(Phase::Voting));
    CHECK(dee.Room(lan.now).raceIndex == 1 && dee.Room(lan.now).lastTrack == result.track);
    CHECK(dee.Votes().empty() && !FindIn(dee.Roster(), 1)->locked);
    dee.Vote(result.track);
    lan.Tick(10);
    CHECK(ana.Votes() == (std::vector<Ballot>{ { dee.LocalId(), result.track } }));
    CHECK(lan.host->Refused() == 4);
    for (const auto& c : lan.clients) {
        CHECK(c->Refused() == 0);
    }
}

void TestPickFlow() {
    std::printf("ready start, draw and pick over the network\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    ana.SetReady(true);
    bo.SetReady(true);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Lobby));

    // The host chooses LAST PLACE PICKS (decision 75), and every copy hears it.
    CHECK(!ana.Room(lan.now).lastPlacePicks);
    CHECK(lan.host->SetLastPlacePicks(true, lan.now));
    lan.Tick(10);
    CHECK(ana.Room(lan.now).lastPlacePicks && bo.Room(lan.now).lastPlacePicks);

    // Every racer ready: the series starts. Nobody has finished last yet, so the room draws.
    CHECK(lan.host->SetReady(true, lan.now));
    CHECK(lan.HostPhase() == Phase::Machine);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    const Picked drawn = lan.host->LastPicked();
    CHECK(drawn.drawn && drawn.by == kSystemId && drawn.track < kStockTracks);
    CHECK(ana.LastPicked() == drawn && bo.LastPicked() == drawn && bo.Room(lan.now).track == drawn.track);
    CHECK(!FindIn(ana.Roster(), 0)->ready && !FindIn(ana.Roster(), 1)->ready);
    CHECK(ana.Room(lan.now).phaseTotalMs == kMachineMs && ana.Room(lan.now).racers == 3);

    ana.Lock(LockOf(4, 0, 50));
    bo.Lock(LockOf(4, 1, 50));
    lan.Tick(10);
    CHECK(lan.host->Lock(LockOf(4, 2, 50), lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::GridSync));
    lan.LoadAll();
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    lan.Tick(510);
    CHECK(lan.AllIn(Phase::Racing));
    lan.CapRace({ 1, 0, 2 });
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Results));

    // Bo finished last: he picks the next track.
    lan.Tick(760);
    CHECK(lan.AllIn(Phase::Pick));
    CHECK(ana.Room(lan.now).picker == 2 && bo.Room(lan.now).picker == 2);
    CHECK(ana.Room(lan.now).phaseTotalMs == kPickMs && ana.Room(lan.now).lastTrack == drawn.track);
    ana.Pick(4);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Pick) && lan.host->Refused() == 1);
    bo.Pick(4);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    CHECK(ana.LastPicked() == (Picked{ 4, false, 2 }) && ana.Room(lan.now).track == 4);
    CHECK(ana.Room(lan.now).picker == kSystemId && ana.Room(lan.now).raceIndex == 1);

    // Nobody locks: the screen times out after a minute and the race goes on with the machines they had.
    lan.Tick(3740); // 59.8 s since the pick, with the ten frames above
    CHECK(lan.AllIn(Phase::Machine));
    lan.Tick(40);
    CHECK(lan.AllIn(Phase::GridSync));
    lan.LoadAll();
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    CHECK(FindIn(bo.Roster(), 1)->machine == 4 && !FindIn(bo.Roster(), 1)->locked);
    // Ana won the race before: the grid has her last, and Bo, who came last, ahead of the other two.
    Grid second;
    CHECK(bo.RaceGrid(second) && second == lan.host->RaceGrid() && second.raceIndex == 1);
    CHECK(second.humans.size() == 3 && second.humans[0].id == 2 && second.humans[1].id == 0 &&
          second.humans[2].id == 1);

    // The host ends the series from the results.
    lan.Tick(510);
    CHECK(!lan.host->EndSeries(lan.now));
    lan.CapRace({});
    CHECK(lan.host->EndSeries(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Lobby));
    CHECK(bo.Room(lan.now).raceIndex == 2 && bo.Room(lan.now).lastTrack == 4 && bo.Room(lan.now).track == kTrackNone);
    CHECK(bo.Room(lan.now) == lan.host->Room(lan.now));
    CHECK(ana.Refused() == 0 && bo.Refused() == 0);
}

void TestWatcherRacesNext() {
    std::printf("watcher asks to race next\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& wim = lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    const uint8_t id = wim.LocalId();
    CHECK(FindIn(ana.Roster(), id)->watcher);
    CHECK(wim.Lines().empty() || !HasLine(wim.Lines(), "Wim joined to watch."));
    wim.SetReady(true); // a watcher has no ready mark
    lan.Tick(10);
    CHECK(lan.host->Refused() == 1 && !FindIn(lan.host->Roster(), id)->ready);
    wim.RaceNext(true);
    lan.Tick(10);
    CHECK(FindIn(ana.Roster(), id)->raceNext && FindIn(ana.Roster(), id)->watcher);
    wim.RaceNext(false);
    lan.Tick(10);
    CHECK(!FindIn(ana.Roster(), id)->raceNext);
    wim.RaceNext(true);
    lan.Tick(10);

    // The lock-in screen opens: he takes a race seat and is on the grid. On LAST PLACE PICKS the first race
    // is drawn, so it opens at once.
    CHECK(lan.host->SetLastPlacePicks(true, lan.now));
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    const PlayerInfo* seated = FindIn(wim.Roster(), id);
    CHECK(seated != nullptr && !seated->watcher && !seated->raceNext && seated->inRace);
    CHECK(wim.Room(lan.now).racers == 3);
    wim.Lock(LockOf(8, 1, 70));
    lan.Tick(10);
    CHECK(FindIn(ana.Roster(), id)->locked && FindIn(ana.Roster(), id)->machine == 8);
    CHECK(lan.host->Refused() == 1);
}

void TestRaceStart() {
    std::printf("the grid, the track check and the start over the network\n");
    // Three computers, three clocks: Ana's is far ahead of the host's, Bo's wraps during the test.
    const uint32_t anaSkew = 5000000u;
    const uint32_t boSkew = 0xFFFFE000u;
    Lan lan;
    lan.nextSkew = anaSkew;
    LobbyClient& ana = lan.AddClient("Ana");
    lan.nextSkew = boSkew;
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined && bo.GetState() == LobbyClient::State::Joined);

    // Each client knows the host clock within a tick of the fake network.
    const auto hostClock = [&lan](const LobbyClient& c, uint32_t clientSkew) {
        return static_cast<uint32_t>(lan.now + clientSkew + c.Clock().OffsetMs());
    };
    const auto apart = [](uint32_t a, uint32_t b) {
        const int32_t d = static_cast<int32_t>(a - b);
        return d < 0 ? -d : d;
    };
    CHECK(ana.Clock().Ready() && ana.Clock().Samples() >= kClockWarmSamples && ana.Clock().RttMs() <= 64);
    CHECK(bo.Clock().Ready() && bo.Clock().Samples() >= kClockWarmSamples && bo.Clock().RttMs() <= 64);
    CHECK(apart(hostClock(ana, anaSkew), lan.now) <= 16);
    CHECK(apart(hostClock(bo, boSkew), lan.now) <= 16);
    CHECK(lan.host->Refused() == 0 && ana.Refused() == 0 && bo.Refused() == 0);

    // The rules are the host's, and travel with the grid.
    RaceRules rules;
    rules.laps = 4;
    rules.lavaNoKill = true;
    rules.magnetRange = 95.0f;
    RaceRules bad = rules;
    bad.laps = 0;
    CHECK(!lan.host->SetRules(bad) && lan.host->SetRules(rules));
    CHECK(lan.host->SetLastPlacePicks(true, lan.now)); // the first race is drawn: no vote to wait out
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    Grid grid;
    CHECK(!ana.RaceGrid(grid) && lan.host->RaceGrid().humans.empty());
    ana.Lock(LockOf(7, 1, 50));
    bo.Lock(LockOf(9, 0, 50));
    lan.Tick(10);
    CHECK(lan.host->Lock(LockOf(2, 3, 50), lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::GridSync));
    CHECK(ana.RaceGrid(grid) && grid == lan.host->RaceGrid());
    CHECK(bo.RaceGrid(grid) && grid == lan.host->RaceGrid());
    CHECK(grid.rules == rules && grid.cpus == kGridSlots - 3 && grid.humans.size() == 3);
    CHECK(grid.humans[1].id == 1 && grid.humans[1].machine == 7 && grid.humans[1].skin == 1);
    CHECK(!lan.host->SetRules(RaceRules())); // the race keeps the rules its grid went out with

    // An answer to another race is refused. Bo loaded a different track than the host: he watches.
    ReadyToRace ready;
    ready.raceIndex = 1;
    ana.GridReady(ready);
    lan.Tick(10);
    CHECK(lan.host->Refused() == 1 && lan.AllIn(Phase::GridSync));
    ready.raceIndex = 0;
    ready.loadMs = 700;
    CHECK(lan.host->GridReady(ready, lan.now));
    CHECK(!lan.host->GridReady(ready, lan.now));
    ready.loadMs = 900;
    ana.GridReady(ready);
    ReadyToRace other = ready;
    other.digest[5] = 1;
    other.loadMs = 1200;
    bo.GridReady(other);
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    CHECK(lan.host->LoadMs(0) == 700 && lan.host->LoadMs(1) == 900 && lan.host->LoadMs(2) == 1200);
    CHECK(FindIn(ana.Roster(), 1)->inRace && !FindIn(ana.Roster(), 2)->inRace && !FindIn(bo.Roster(), 2)->inRace);
    CHECK(ana.Room(lan.now).racers == 2);

    // One start time, and every copy places it at the same moment: a little ahead, within a tick.
    StartSignal hostStart;
    StartSignal anaStart;
    StartSignal boStart;
    CHECK(lan.host->StartSent(hostStart) && ana.StartSent(anaStart) && bo.StartSent(boStart));
    CHECK(anaStart == hostStart && boStart == hostStart && hostStart.raceIndex == 0);
    const int32_t hostElapsed = static_cast<int32_t>(lan.now - hostStart.startAtMs);
    const int32_t anaElapsed = static_cast<int32_t>(hostClock(ana, anaSkew) - anaStart.startAtMs);
    const int32_t boElapsed = static_cast<int32_t>(hostClock(bo, boSkew) - boStart.startAtMs);
    CHECK(hostElapsed < 0 && hostElapsed > -static_cast<int32_t>(kStartLeadMs));
    CHECK(apart(static_cast<uint32_t>(anaElapsed), static_cast<uint32_t>(hostElapsed)) <= 16);
    CHECK(apart(static_cast<uint32_t>(boElapsed), static_cast<uint32_t>(hostElapsed)) <= 16);

    // A second answer changes nothing.
    ana.GridReady(ready);
    lan.Tick(10);
    CHECK(lan.host->Refused() == 2 && lan.AllIn(Phase::Countdown));
    lan.Tick(510);
    CHECK(lan.AllIn(Phase::Racing));
    lan.CapRace({ 1, 0 });
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Results) && lan.host->LastOrder() == (std::vector<uint8_t>{ 1, 0 }));
    CHECK(ana.Refused() == 0 && bo.Refused() == 0);

    // The host copy has no track: nobody races, and the room goes on to the results.
    Lan none;
    LobbyClient& cy = none.AddClient("Cy");
    none.Settle();
    CHECK(none.host->SetLastPlacePicks(true, none.now));
    CHECK(none.host->Start(none.now));
    cy.Lock(LockOf(1, 0, 50));
    none.Tick(10);
    CHECK(none.host->Lock(LockOf(2, 0, 50), none.now));
    none.Tick(10);
    cy.GridReady(ReadyToRace());
    none.Tick(10);
    CHECK(none.AllIn(Phase::GridSync));
    none.Tick(320); // 5.1 s: the wait for the tracks runs out
    CHECK(none.AllIn(Phase::Results));
    StartSignal never;
    CHECK(!none.host->StartSent(never) && !cy.StartSent(never) && none.host->LastOrder().empty());

    // A room closed in mid race takes no more calls, the host's leave from the race among them.
    Lan shut;
    LobbyClient& di = shut.AddClient("Di");
    shut.Settle();
    CHECK(shut.host->SetLastPlacePicks(true, shut.now));
    CHECK(shut.host->Start(shut.now));
    di.Lock(LockOf(1, 0, 50));
    shut.Tick(10);
    CHECK(shut.host->Lock(LockOf(2, 0, 50), shut.now));
    shut.Tick(10);
    shut.LoadAll();
    shut.Tick(520);
    CHECK(shut.AllIn(Phase::Racing));
    shut.host->Close(shut.now);
    CHECK(!shut.host->HostLeftRace(shut.now) && !shut.host->Room(shut.now).cpusOut);
}

void TestRaceResults() {
    std::printf("finish reports and the results table over the network\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    LobbyClient& wim = lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    CHECK(ana.LocalId() == 1 && bo.LocalId() == 2 && wim.LocalId() == 3);
    RaceResults table;
    CHECK(!ana.Results(table) && !wim.Results(table) && lan.host->Results().entries.empty());

    CHECK(lan.host->SetLastPlacePicks(true, lan.now)); // the first race is drawn: no vote to wait out
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    ana.Lock(LockOf(7, 1, 50));
    bo.Lock(LockOf(9, 0, 50));
    lan.Tick(10);
    CHECK(lan.host->Lock(LockOf(2, 3, 50), lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::GridSync));
    lan.LoadAll();
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    uint32_t refused = lan.host->Refused();
    // True when the host refused exactly one message since the last look.
    const auto refusedOne = [&] {
        const bool one = lan.host->Refused() == refused + 1;
        refused = lan.host->Refused();
        return one;
    };
    const auto report = [](ResultKind kind, uint32_t timeMs, uint16_t race = 0) {
        FinishReport r;
        r.raceIndex = race;
        r.kind = kind;
        r.timeMs = timeMs;
        return r;
    };
    // How long the host says the race has run.
    const auto raced = [&lan] {
        const RoomState s = lan.host->Room(lan.now);
        return s.phaseTotalMs - s.phaseLeftMs;
    };
    const auto lastLap = [](uint8_t slot, uint32_t tick, int16_t lap) {
        StatePose pose = PoseOf(slot, tick);
        pose.lap = lap;
        return pose;
    };
    // True when every copy holds the host's table.
    const auto allHold = [&] {
        bool same = true;
        for (auto& c : lan.clients) {
            RaceResults t;
            same = same && c->Results(t) && t == lan.host->Results();
        }
        return same;
    };

    // No race runs yet: nothing is taken.
    ana.Finish(report(ResultKind::Retired, 0));
    lan.Tick(10);
    CHECK(refusedOne() && !ana.Results(table));
    CHECK(!lan.host->Finish(report(ResultKind::Retired, 0), lan.now));

    lan.Tick(510);
    CHECK(lan.AllIn(Phase::Racing) && lan.host->Room(lan.now).phaseTotalMs == kRaceCapMs);
    lan.Tick(1250); // twenty seconds of racing
    lan.host->SendPoses(0, { PoseOf(0, 100) });
    ana.SendPoses(0, { PoseOf(1, 100) });
    bo.SendPoses(0, { PoseOf(2, 100) });
    lan.Tick(4);
    CHECK(lan.host->Refused() == refused);

    // Ana says she is done, and her own poses say lap 1 of 3: refused, and nobody gets a table.
    ana.Finish(report(ResultKind::Finished, 15000));
    lan.Tick(6);
    CHECK(refusedOne() && lan.host->Results().entries.empty() && !ana.Results(table) && !wim.Results(table));
    // A watcher has no race to end, and nobody ends another race.
    wim.Finish(report(ResultKind::Retired, 0));
    lan.Tick(6);
    CHECK(refusedOne());
    bo.Finish(report(ResultKind::Retired, 0, 1));
    lan.Tick(6);
    CHECK(refusedOne() && lan.HostPhase() == Phase::Racing);

    // Her pose names the last lap: now it counts, and every copy holds the same table. The host game's
    // order places the two who are still driving.
    lan.host->SetStanding({ 2, 0, 1 });
    ana.SendPoses(0, { lastLap(1, 200, 3) });
    lan.Tick(4);
    const uint32_t anaTime = raced();
    ana.Finish(report(ResultKind::Finished, anaTime));
    lan.Tick(6);
    CHECK(lan.host->Refused() == refused && allHold());
    table = lan.host->Results();
    CHECK(!table.final && table.raceIndex == 0 && table.entries.size() == 3);
    if (table.entries.size() != 3) {
        return; // the checks below read the three rows
    }
    CHECK(table.entries[0] == (ResultEntry{ 1, ResultKind::Finished, anaTime, 100 }));
    CHECK(table.entries[1] == (ResultEntry{ 2, ResultKind::Unfinished, 0, 93 }));
    CHECK(table.entries[2] == (ResultEntry{ 0, ResultKind::Unfinished, 0, 87 }));
    // The winner is in: the rest have a minute, and every copy shows that timer.
    const RoomState told = lan.host->Room(lan.now);
    CHECK(told.phase == Phase::Racing && told.phaseTotalMs >= anaTime + kAfterWinnerMs &&
          told.phaseTotalMs <= anaTime + kAfterWinnerMs + 100);
    CHECK(ana.Room(lan.now).phaseTotalMs == told.phaseTotalMs && wim.Room(lan.now).phaseTotalMs == told.phaseTotalMs);
    CHECK(FindIn(lan.host->Roster(), 1)->points == 0); // points come with the final table
    // One result a race.
    ana.Finish(report(ResultKind::Retired, 0));
    lan.Tick(6);
    CHECK(refusedOne());

    // Someone who joins now is given the table too.
    LobbyClient& dee = lan.AddClient("Dee", Seat::Watch);
    lan.Settle();
    CHECK(dee.GetState() == LobbyClient::State::Joined && dee.Results(table) && table == lan.host->Results() &&
          lan.HostPhase() == Phase::Racing);

    // Bo is on the last lap and names a time the race has not reached: refused.
    bo.SendPoses(0, { lastLap(2, 300, 3) });
    lan.Tick(4);
    bo.Finish(report(ResultKind::Finished, raced() + 5000));
    lan.Tick(6);
    CHECK(refusedOne() && lan.host->Results() == table);

    // The host player is held to the same rules.
    CHECK(!lan.host->Finish(report(ResultKind::Finished, raced()), lan.now));
    lan.host->SendPoses(0, { lastLap(0, 300, 4) });
    const uint32_t hostTime = raced();
    CHECK(hostTime > anaTime && lan.host->Finish(report(ResultKind::Finished, hostTime), lan.now));
    CHECK(!lan.host->Finish(report(ResultKind::Retired, 0), lan.now));
    lan.Tick(6);
    CHECK(allHold() && lan.HostPhase() == Phase::Racing);
    table = lan.host->Results();
    CHECK(!table.final && table.entries.size() == 3 &&
          table.entries[1] == (ResultEntry{ 0, ResultKind::Finished, hostTime, 93 }) &&
          table.entries[2] == (ResultEntry{ 2, ResultKind::Unfinished, 0, 87 }));

    // The host player leaves the race for the room: every copy hears once that the CPU machines are out
    // of it, and the race runs on.
    CHECK(!ana.Room(lan.now).cpusOut && !lan.host->Room(lan.now).cpusOut);
    CHECK(lan.host->HostLeftRace(lan.now));
    CHECK(!lan.host->HostLeftRace(lan.now));
    lan.Tick(6);
    CHECK(lan.AllIn(Phase::Racing) && lan.host->Room(lan.now).cpusOut && allHold());
    for (auto& c : lan.clients) {
        CHECK(c->Room(lan.now).cpusOut && c->Refused() == 0);
    }

    // Bo goes out: everyone has a result, so the race is over, and the points are in the roster.
    bo.Finish(report(ResultKind::Retired, 0));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Results) && allHold() && lan.host->Refused() == refused);
    table = lan.host->Results();
    CHECK(table.final && table.raceIndex == 0 && table.entries.size() == 3);
    CHECK(table.entries.size() == 3 && table.entries[0] == (ResultEntry{ 1, ResultKind::Finished, anaTime, 100 }) &&
          table.entries[1] == (ResultEntry{ 0, ResultKind::Finished, hostTime, 93 }) &&
          table.entries[2] == (ResultEntry{ 2, ResultKind::Retired, 0, 0 }));
    CHECK(lan.host->LastOrder() == (std::vector<uint8_t>{ 1, 0, 2 }));
    const std::vector<PlayerInfo> roster = lan.host->Roster();
    CHECK(FindIn(roster, 1)->points == 100 && FindIn(roster, 0)->points == 93 && FindIn(roster, 2)->points == 0 &&
          FindIn(roster, 3)->points == 0);
    for (auto& c : lan.clients) {
        CHECK(SameRoster(c->Roster(), roster) && c->Refused() == 0);
    }
    // The race is over: a late report is refused.
    bo.Finish(report(ResultKind::Retired, 0));
    lan.Tick(6);
    CHECK(refusedOne());

    // The table stays up until the next race has one of its own.
    lan.Tick(760);
    CHECK(lan.HostPhase() != Phase::Results && lan.host->Room(lan.now).raceIndex == 1);
    CHECK(!lan.host->Room(lan.now).cpusOut && !ana.Room(lan.now).cpusOut); // the next race has its CPUs
    CHECK(allHold() && lan.host->Results().final && lan.host->Results().raceIndex == 0);
    CHECK(FindIn(ana.Roster(), 1)->points == 100);
}

// The poses a raw peer was sent, in the order they came.
std::vector<StatePose> StatesOf(const RawPeer& peer) {
    std::vector<StatePose> all;
    for (const Message& m : peer.got) {
        RaceState state;
        if (m.type != kMsgState) {
            continue;
        }
        CHECK(!m.reliable && DecodeState(m.payload.data(), m.payload.size(), state));
        all.insert(all.end(), state.poses.begin(), state.poses.end());
    }
    return all;
}

void TestRacePoses() {
    std::printf("the poses of a race over the network\n");
    Lan lan;
    // Raw races and Eve watches, and both skip a client's own checks. Bo races and Wim watches by the book.
    RawPeer raw(lan, 220, kProtocolVersion);
    RawPeer eve(lan, 221, kProtocolVersion);
    const auto run = [&](int frames) {
        for (int i = 0; i < frames; i++) {
            lan.Tick();
            raw.Turn();
            eve.Turn();
        }
    };
    run(20);
    JoinRequest join;
    join.build = kBuild;
    join.name = "Raw";
    join.colour = 0x445566;
    join.machine = 2;
    raw.Send(kMsgJoin, EncodeJoin(join));
    run(10);
    LobbyClient& bo = lan.AddClient("Bo");
    run(90);
    LobbyClient& wim = lan.AddClient("Wim", Seat::Watch);
    run(90);
    join.name = "Eve";
    join.seat = Seat::Watch;
    eve.Send(kMsgJoin, EncodeJoin(join));
    run(10);
    CHECK(lan.host->Roster().size() == 5 && bo.LocalId() == 2 && wim.LocalId() == 3);
    CHECK(FindIn(lan.host->Roster(), 1) != nullptr && !FindIn(lan.host->Roster(), 1)->watcher);
    CHECK(FindIn(lan.host->Roster(), 4) != nullptr && FindIn(lan.host->Roster(), 4)->watcher);

    const auto hostGot = [&] {
        std::vector<StatePose> poses;
        uint16_t race = 0;
        lan.host->TakePoses(race, poses);
        return poses;
    };
    const auto clientGot = [](LobbyClient& client) {
        std::vector<StatePose> poses;
        uint16_t race = 0;
        client.TakePoses(race, poses);
        return poses;
    };
    const auto loose = [&](RawPeer& peer, const RaceState& state) {
        peer.SendLoose(kMsgState, EncodeState(state));
        run(4);
    };
    // True when no game was handed a pose since the last look.
    const auto quiet = [&] {
        const bool host = hostGot().empty();
        const bool player = clientGot(bo).empty();
        const bool watcher = clientGot(wim).empty();
        return host && player && watcher;
    };

    // No race yet: a pose is nobody's fault, and goes nowhere.
    uint32_t refused = lan.host->Refused();
    loose(raw, { 0, { PoseOf(1, 2) } });
    lan.host->SendPoses(0, { PoseOf(0, 2) });
    bo.SendPoses(0, { PoseOf(2, 2) });
    run(4);
    CHECK(lan.host->Refused() == refused && quiet() && StatesOf(raw).empty());

    CHECK(lan.host->SetLastPlacePicks(true, lan.now)); // the first race is drawn: no vote to wait out
    CHECK(lan.host->Start(lan.now));
    run(10);
    CHECK(lan.HostPhase() == Phase::Machine);
    raw.Send(kMsgLock, EncodeLock(LockOf(5, 0, 50)));
    bo.Lock(LockOf(7, 1, 50));
    run(10);
    CHECK(lan.host->Lock(LockOf(2, 3, 50), lan.now));
    run(10);
    CHECK(lan.HostPhase() == Phase::GridSync);
    const Grid grid = lan.host->RaceGrid();
    CHECK(grid.humans.size() == 3 && grid.cpus == kGridSlots - 3 && grid.raceIndex == 0);
    CHECK(grid.humans.size() == 3 && grid.humans[0].id == kHostId && grid.humans[1].id == 1 && grid.humans[2].id == 2);
    refused = lan.host->Refused();

    // The host's part of the field: its own machine and the 27 CPUs. That is more than one message holds.
    std::vector<StatePose> field = { PoseOf(0, 10) };
    for (uint8_t slot = 3; slot < kGridSlots; slot++) {
        field.push_back(PoseOf(slot, 10));
    }
    std::vector<StatePose> more = field;
    more.push_back(PoseOf(1, 10)); // Raw's machine is not the host's to speak for
    StatePose wild = PoseOf(4, 12);
    wild.pos[1] = std::numeric_limits<float>::infinity();
    more.push_back(wild);
    lan.host->SendPoses(0, more);
    lan.host->SendPoses(1, field); // another race
    run(3);
    CHECK(field.size() == 28 && clientGot(bo) == field && clientGot(wim) == field);
    CHECK(StatesOf(raw) == field && StatesOf(eve) == field && raw.Count(kMsgState) == 2);
    CHECK(hostGot().empty());

    // Each of the two messages is whole on its own: with the larger one lost, the other ten poses arrive.
    const NetAddress boAddr = lan.clientAddrs[0];
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t len) {
        return from == lan.hostAddr && to == boAddr && len > 700;
    });
    // A newer tick: a copy of one already taken never reaches the game a second time.
    std::vector<StatePose> next;
    for (const StatePose& pose : field) {
        next.push_back(PoseOf(pose.slot, 11));
    }
    lan.host->SendPoses(0, next);
    run(3);
    lan.net.SetFilter(nullptr);
    CHECK(clientGot(bo) == std::vector<StatePose>(next.begin() + kStatePoses, next.end()));
    CHECK(clientGot(wim) == next);
    lan.host->SendPoses(0, next);
    run(3);
    CHECK(clientGot(bo).size() == kStatePoses && clientGot(wim).empty());

    // What a race costs: sixty sends of the whole field, with one pose back from each player.
    size_t down = 0;
    size_t downGrams = 0;
    size_t up = 0;
    size_t upGrams = 0;
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t len) {
        if (from == lan.hostAddr && to == boAddr) {
            down += len;
            downGrams++;
        }
        if (from == boAddr && to == lan.hostAddr) {
            up += len;
            upGrams++;
        }
        return false;
    });
    const size_t sends = 60;
    size_t hostHeard = 0;
    size_t boHeard = 0;
    size_t wimHeard = 0;
    bool ownSeen = false;
    for (size_t i = 0; i <= sends + 2; i++) {
        if (i < sends) {
            const uint32_t tick = 100 + 2 * static_cast<uint32_t>(i);
            std::vector<StatePose> now;
            for (const StatePose& pose : field) {
                now.push_back(PoseOf(pose.slot, tick));
            }
            lan.host->SendPoses(0, now);
            bo.SendPoses(0, { PoseOf(2, tick) });
            raw.SendLoose(kMsgState, EncodeState({ 0, { PoseOf(1, tick) } }));
        }
        run(2);
        hostHeard += hostGot().size();
        for (const StatePose& pose : clientGot(bo)) {
            ownSeen = ownSeen || pose.slot == 2;
            boHeard++;
        }
        wimHeard += clientGot(wim).size();
    }
    lan.net.SetFilter(nullptr);
    CHECK(hostHeard == 2 * sends);                 // one from each player
    CHECK(boHeard == (field.size() + 1) * sends);  // every machine but her own
    CHECK(wimHeard == (field.size() + 2) * sends); // a watcher: all 30
    CHECK(!ownSeen && lan.host->Refused() == refused && bo.Refused() == 0 && wim.Refused() == 0);
    const double perSend = 1.0 / static_cast<double>(sends);
    const double downBytes = static_cast<double>(down) * perSend;
    const double upBytes = static_cast<double>(up) * perSend;
    const double downCount = static_cast<double>(downGrams) * perSend;
    const double upCount = static_cast<double>(upGrams) * perSend;
    std::printf("  one send of a field of 30: host to a player %.0f bytes in %.1f datagrams, a player to the host %.0f "
                "bytes in %.1f\n",
                downBytes, downCount, upBytes, upCount);
    std::printf("  at 30 sends a second, 28 bytes of UDP and IP on each datagram: %.1f kB a second to each player, "
                "%.1f from each\n",
                30.0 * (downBytes + 28.0 * downCount) / 1000.0, 30.0 * (upBytes + 28.0 * upCount) / 1000.0);
    CHECK(downBytes > 29.0 * kStatePoseBytes && downBytes < 29.0 * kStatePoseBytes + 200.0);
    CHECK(upBytes > 1.0 * kStatePoseBytes && upBytes < 1.0 * kStatePoseBytes + 100.0);

    // A player's pose reaches the host's own game and, a turn later, everyone else. Not its sender.
    const std::vector<StatePose> fromRaw = { PoseOf(1, 300) };
    loose(raw, { 0, fromRaw });
    CHECK(hostGot() == fromRaw && clientGot(bo) == fromRaw && clientGot(wim) == fromRaw);
    const std::vector<StatePose> fromBo = { PoseOf(2, 300) };
    bo.SendPoses(0, { PoseOf(2, 300), PoseOf(0, 300) }); // the second is not hers: her own client drops it
    bo.SendPoses(1, { PoseOf(2, 302) });                 // nor is this race the one being run
    run(4);
    CHECK(hostGot() == fromBo && clientGot(bo).empty() && clientGot(wim) == fromBo);
    CHECK(!StatesOf(raw).empty() && StatesOf(raw).back() == PoseOf(2, 300));
    CHECK(lan.host->Refused() == refused);

    // What the host will not pass on. Each is counted once and reaches no game.
    const auto refusedOnce = [&] {
        const bool grew = lan.host->Refused() == refused + 1;
        const bool silent = quiet();
        refused = lan.host->Refused();
        return grew && silent;
    };
    loose(raw, { 0, { PoseOf(2, 310) } }); // Bo's machine
    CHECK(refusedOnce());
    loose(raw, { 0, { PoseOf(1, 310), PoseOf(0, 310) } }); // one of the two is not its own: neither is taken
    CHECK(refusedOnce());
    loose(raw, { 0, { PoseOf(3, 310) } }); // a CPU is the host's
    CHECK(refusedOnce());
    loose(eve, { 0, { PoseOf(1, 310) } }); // a watcher drives nothing
    CHECK(refusedOnce());
    raw.SendLoose(kMsgState, { 1, 2, 3 });
    run(4);
    CHECK(refusedOnce());
    raw.Send(kMsgState, EncodeState({ 0, { PoseOf(1, 310) } })); // sent the reliable way
    run(6);
    CHECK(refusedOnce());
    loose(raw, { 5, { PoseOf(1, 310) } }); // another race: late, not wrong
    CHECK(lan.host->Refused() == refused && quiet());

    // A peer that sends faster than any game does: four poses a turn are passed on, the rest are dropped
    // without a word. The next turn counts from nothing again.
    for (uint32_t i = 0; i < 10; i++) {
        raw.SendLoose(kMsgState, EncodeState({ 0, { PoseOf(1, 400 + 2 * i) } }));
    }
    run(4);
    const std::vector<StatePose> passed = hostGot();
    CHECK(passed.size() == kPosesPerPeerPerUpdate && !passed.empty() && passed.front() == PoseOf(1, 400));
    CHECK(clientGot(bo) == passed && clientGot(wim) == passed);
    RaceState six;
    for (uint32_t i = 0; i < 6; i++) {
        six.poses.push_back(PoseOf(1, 440 + 2 * i));
    }
    loose(raw, six);
    CHECK(hostGot().size() == kPosesPerPeerPerUpdate && clientGot(bo).size() == kPosesPerPeerPerUpdate);
    loose(raw, { 0, { PoseOf(1, 500) } });
    CHECK(hostGot().size() == 1 && clientGot(bo).size() == 1 && lan.host->Refused() == refused);
    clientGot(wim);

    // The tracks are compared. Raw loaded another one: it is out of this race, and so are its poses.
    ReadyToRace ready;
    ready.raceIndex = 0;
    CHECK(lan.host->GridReady(ready, lan.now));
    bo.GridReady(ready);
    ReadyToRace other = ready;
    other.digest[5] = 1;
    raw.Send(kMsgReadyToRace, EncodeReadyToRace(other));
    run(10);
    CHECK(lan.HostPhase() == Phase::Countdown && !FindIn(lan.host->Roster(), 1)->inRace);
    refused = lan.host->Refused();
    loose(raw, { 0, { PoseOf(1, 600) } });
    CHECK(refusedOnce());
    // It watches from here on: the field reaches it, Bo's machine included.
    const std::vector<StatePose> later = { PoseOf(2, 600) };
    bo.SendPoses(0, later);
    run(4);
    CHECK(hostGot() == later && clientGot(wim) == later && StatesOf(raw).back() == PoseOf(2, 600));

    // The same while the race runs.
    run(510);
    CHECK(lan.HostPhase() == Phase::Racing);
    bo.SendPoses(0, { PoseOf(2, 700) });
    lan.host->SendPoses(0, { PoseOf(0, 700), PoseOf(29, 700) });
    run(4);
    CHECK(hostGot().size() == 1 && clientGot(bo).size() == 2 && clientGot(wim).size() == 3);

    // The two racers go out and the race is over: what is still on its way is dropped without a word.
    FinishReport out;
    out.raceIndex = 0;
    out.kind = ResultKind::Retired;
    bo.Finish(out);
    run(6);
    CHECK(lan.HostPhase() == Phase::Racing && lan.host->Finish(out, lan.now));
    run(10);
    CHECK(lan.HostPhase() == Phase::Results);
    refused = lan.host->Refused();
    bo.SendPoses(0, { PoseOf(2, 800) });
    lan.host->SendPoses(0, { PoseOf(0, 800) });
    loose(raw, { 0, { PoseOf(1, 800) } });
    CHECK(lan.host->Refused() == refused && quiet());
    CHECK(bo.Refused() == 0 && wim.Refused() == 0);

    // Nobody was sent their own machine at any point.
    for (const StatePose& pose : StatesOf(raw)) {
        ownSeen = ownSeen || pose.slot == 1;
    }
    CHECK(!ownSeen);
}

// A saved course with forks past 64 points, several chunks long, as in gdx_net_track_flow_tests.cpp.
uint32_t Binding(const gdx::snapshot::Generation& g) {
    return gdx_course_registry_crc32(reinterpret_cast<const unsigned char*>(g.courseId.data()), g.courseId.size());
}

bool BuildTrack(int count, std::vector<uint8_t>& bytes, track::TrackId& id) {
    using gdx::snapshot::Metadata;
    gdx::snapshot::Generation g;
    g.disk = 0x1020304050607080ULL;
    g.directory = 3;
    g.courseId = "ROOM TEST.CRSD";
    g.pointCount = uint8_t(count);
    g.base.assign(gdx::snapshot::BaseBytes, 0);
    g.base[1] = uint8_t(std::min(count, 64));
    g.base[4] = 1;
    for (size_t i = 16; i < gdx::snapshot::CourseBytes; i++) {
        g.base[i] = uint8_t(i * 7);
    }
    gdx::snapshot::Bytes buffer(65536);
    auto store = [&](Metadata section, int n) {
        if (n > 0) {
            g.metadata[size_t(section)].assign(buffer.begin(), buffer.begin() + n);
        }
    };
    store(Metadata::Title, gdx_course_title_encode("Room test", Binding(g), buffer.data(), buffer.size()));
    GdxCourseSettings settings{};
    settings.ringCount = g.pointCount;
    settings.magnetRange = 200;
    store(Metadata::Settings, gdx_settings_encode(&settings, Binding(g), buffer.data(), buffer.size()));
    GdxBranchSet forks{};
    forks.ringCount = g.pointCount;
    forks.extendedCount = count - 64;
    forks.baseCrc = gdx_course_registry_crc32(g.base.data(), gdx::snapshot::CourseBytes);
    std::memcpy(&forks.courseChecksum, g.base.data() + 4, 4);
    for (int i = 0; i < forks.extendedCount; ++i) {
        auto& p = forks.extended[i];
        p.pos[0] = float(i * 10);
        p.pos[1] = 250.0f;
        p.pos[2] = float(i * 17);
        p.radiusLeft = 100;
        p.radiusRight = 200;
        p.pit = p.dash = p.dirt = p.ice = p.jump = p.landmine = p.gate = p.building = p.sign = -1;
    }
    store(Metadata::Forks, gdx_branch_encode(&forks, Binding(g), buffer.data(), buffer.size()));
    gdx::snapshot::Bytes content;
    if (!gdx::snapshot::content(g, content)) {
        return false;
    }
    bytes.assign(content.begin(), content.end());
    id = track::Identify(bytes.data(), bytes.size());
    return true;
}

// One copy's disk: its own tracks and its cache in one map.
struct Disk {
    std::map<track::TrackId, std::vector<uint8_t>> tracks;

    TrackStore Store() {
        TrackStore s;
        s.load = [this](const track::TrackId& id, std::vector<uint8_t>& bytes) {
            auto it = tracks.find(id);
            if (it == tracks.end()) {
                return false;
            }
            bytes = it->second;
            return true;
        };
        s.store = [this](const track::TrackId& id, const std::vector<uint8_t>& bytes) { tracks[id] = bytes; };
        return s;
    }
};

// Host, Ana and Bo on LAST PLACE PICKS, driven to the phase, with Ana the successor. Results comes after a
// race run to its cap, Ana first, the host second and Bo last.
void DriveTo(Lan& lan, Phase phase) {
    LobbyClient& ana = *lan.clients[0];
    LobbyClient& bo = *lan.clients[1];
    if (phase == Phase::Lobby) {
        return;
    }
    CHECK(lan.host->SetLastPlacePicks(true, lan.now)); // the first race is drawn: no vote to wait out
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Machine));
    ana.Lock(LockOf(7, 1, 50));
    lan.Tick(10);
    if (phase == Phase::Machine) {
        return;
    }
    bo.Lock(LockOf(9, 0, 50));
    lan.Tick(10);
    CHECK(lan.host->Lock(LockOf(2, 3, 50), lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::GridSync));
    lan.LoadAll();
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Countdown));
    if (phase == Phase::Countdown) {
        return;
    }
    for (int i = 0; i < 1000 && lan.HostPhase() != Phase::Racing; i++) {
        lan.Tick();
    }
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Racing));
    if (phase == Phase::Racing) {
        return;
    }
    lan.CapRace({ 1, 0, 2 });
    lan.Tick(60);
    CHECK(lan.AllIn(Phase::Results));
}

const PlayerInfo* NamedIn(const std::vector<PlayerInfo>& roster, const std::string& name) {
    for (const PlayerInfo& p : roster) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

// The host is lost in each phase of a race with Ana and Bo (D28.4). Ana hosts from her mirror; Bo comes back
// under his id. Lobby: the room stays in the lobby. Machine: the track is chosen again under the same race
// index. Countdown and Racing: that race scores nothing and the next is chosen under a new index. Results:
// the page runs out on Ana's clock, then last place picks.
void TestHostLostIn(Phase phase) {
    std::printf("the host is lost in %s (D28.4)\n", PhaseName(phase));
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.Settle();
    CHECK(lan.clients[0]->LocalId() == 1 && lan.clients[1]->LocalId() == 2);
    DriveTo(lan, phase);
    const RoomState was = lan.host->Room(lan.now);
    CHECK(was.phase == phase && was.successor == 1);
    const uint16_t boPoints = NamedIn(lan.host->Roster(), "Bo") != nullptr ? NamedIn(lan.host->Roster(), "Bo")->points : 0;
    const uint16_t anaPoints =
        NamedIn(lan.host->Roster(), "Ana") != nullptr ? NamedIn(lan.host->Roster(), "Ana")->points : 0;

    lan.DropHost();
    // Both find out after kLinkTimeoutMs of silence; Bo reaches Ana within a few frames.
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    LobbyClient& bo = *lan.clients[1];
    const uint32_t t = lan.now;
    CHECK(lan.clients[0]->NextHost().kind == HandoverPlan::Kind::Promote);
    CHECK(next.PromotedFrom() == 1 && next.RejoinCount() == 1 && !next.Closed());
    CHECK(bo.GetState() == LobbyClient::State::Joined && bo.LocalId() == 2 && bo.Host() == lan.clientAddrs[0]);
    CHECK(next.Roster().size() == 2 && next.Roster()[0].name == "Ana" && next.Roster()[0].host &&
          next.Roster()[1].id == 2 && next.Roster()[1].name == "Bo");
    CHECK(SameRoster(bo.Roster(), next.Roster()));
    const RoomState s = next.Room(t);
    CHECK(s.roomId == 77 && s.successor == 2 && s.successorPort == lan.clientAddrs[1].port);
    // Bo holds a token from the new host, and as successor the table, which is empty: nobody else is left.
    CHECK(bo.HandoverTokenOwn() != 0 && bo.HandoverTokenOwn() == next.TokenOf(2) && bo.HandoverTable().empty());
    switch (phase) {
    case Phase::Lobby:
        CHECK(lan.NextIn(Phase::Lobby) && s.raceIndex == 0);
        break;
    case Phase::Machine:
        CHECK(lan.NextIn(Phase::Machine) && s.raceIndex == 0 && !next.Roster()[0].locked);
        break;
    case Phase::Countdown:
    case Phase::Racing:
        CHECK(lan.NextIn(Phase::Machine) && s.raceIndex == was.raceIndex + 1);
        CHECK(next.Roster()[0].points == 0 && next.Roster()[1].points == 0 && next.Results().entries.empty());
        break;
    case Phase::Results: {
        CHECK(lan.NextIn(Phase::Results) && s.raceIndex == was.raceIndex);
        CHECK(next.Roster()[0].points == anaPoints && anaPoints == 100 && next.Roster()[1].points == boPoints);
        RaceResults table;
        CHECK(bo.Results(table) && table.final && table.entries.size() == 2 && table.entries[0].id == kHostId);
        for (int i = 0; i < 1000 && next.Room(lan.now).phase == Phase::Results; i++) {
            lan.TickFollow(1);
        }
        lan.TickFollow(5); // Bo hears of it a frame later
        CHECK(lan.NextIn(Phase::Pick) && next.Room(lan.now).picker == 2 &&
              next.Room(lan.now).raceIndex == was.raceIndex + 1);
        break;
    }
    default:
        CHECK(false);
        break;
    }
    // The window runs out with everyone back: nobody loses a seat, and the room goes on.
    lan.TickFollow(static_cast<int>(kSuccessorWaitMs / 16));
    CHECK(!next.Closed() && next.Roster().size() == 2 && bo.GetState() == LobbyClient::State::Joined);
    CHECK(next.Refused() == 0 && bo.Refused() == 0);
}

// The host crashes after Ana finished (D28.6, decision 17): every copy held a table that gave her the win, but
// it was not the final one. The race scores nothing, and the next is chosen under a new index.
void TestHostLostAfterAFinish() {
    std::printf("the host is lost after a racer finished (D28.6)\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.Settle();
    DriveTo(lan, Phase::Racing);
    const uint16_t race = lan.host->Room(lan.now).raceIndex;
    lan.Tick(1250); // twenty seconds of racing
    StatePose last = PoseOf(1, 200);
    last.lap = 3;
    ana.SendPoses(0, { last });
    lan.Tick(4);
    const RoomState s = lan.host->Room(lan.now);
    FinishReport report;
    report.raceIndex = race;
    report.kind = ResultKind::Finished;
    report.timeMs = s.phaseTotalMs - s.phaseLeftMs;
    ana.Finish(report);
    lan.Tick(6);
    RaceResults held;
    CHECK(ana.Results(held) && !held.final && held.raceIndex == race && !held.entries.empty() &&
          held.entries[0].id == 1 && held.entries[0].points == PlacePoints(1));
    CHECK(NamedIn(lan.host->Roster(), "Ana")->points == 0);

    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    LobbyClient& bo = *lan.clients[1];
    CHECK(lan.NextIn(Phase::Machine) && next.Room(lan.now).raceIndex == race + 1);
    CHECK(next.Roster().size() == 2 && next.Roster()[0].points == 0 && next.Roster()[1].points == 0);
    CHECK(next.Results().entries.empty() && !bo.Results(held));
    lan.TickFollow(static_cast<int>(kSuccessorWaitMs / 16));
    CHECK(!next.Closed() && next.Roster().size() == 2 && next.Refused() == 0 && bo.Refused() == 0);
}

// A watcher who is gone too, a stranger with a made-up token, and a member who cannot reach the successor.
void TestRejoinRefusals() {
    std::printf("rejoin refusals and the end of the window (D28.4)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    CHECK(lan.clients[2]->LocalId() == 3 && lan.clients[2]->HandoverTokenOwn() != 0);
    const uint64_t boToken = lan.clients[1]->HandoverTokenOwn();
    // Wim's link goes with the host: he plans his way back but nothing he sends arrives.
    lan.net.Cut(lan.clientAddrs[2]);
    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    CHECK(next.RejoinCount() == 1 && next.Roster().size() == 3 && NamedIn(next.Roster(), "Wim") != nullptr);
    CHECK(lan.clients[2]->GetState() == LobbyClient::State::Connecting);

    // A copy that names Bo's seat with the wrong token, and one that names Bo's seat with the right token
    // while Bo is already back: both are refused, and Bo keeps his seat.
    const auto stranger = [&](uint8_t id, uint64_t token, int index) {
        const NetAddress addr = Addr(Ip(192, 168, 1, 90 + index), static_cast<uint16_t>(51000 + index));
        auto c = std::make_unique<LobbyClient>(kBuild, Profile("Eve", 0x0000FF, 3), Seat::Race, lan.clientAddrs[0],
                                               0x3000u + static_cast<uint32_t>(index), lan.net.SenderFor(addr));
        LobbyClient* raw = c.get();
        lan.net.Add(addr, [raw, &lan](const NetAddress& from, const uint8_t* d, size_t n) {
            raw->Receive(from, d, n, lan.now);
        });
        HandoverPlan plan;
        plan.kind = HandoverPlan::Kind::Rejoin;
        plan.address = lan.clientAddrs[0];
        plan.id = id;
        plan.token = token;
        plan.sinceMs = lan.now;
        CHECK(c->SetRejoin(plan));
        c->Start(lan.now);
        for (int i = 0; i < 60; i++) {
            lan.Tick();
            raw->Update(lan.now);
        }
        CHECK(raw->GetState() == LobbyClient::State::Closed &&
              raw->CloseText() == RejectReasonText(RejectReason::Rejoin));
        lan.net.Cut(addr);
        return c;
    };
    auto eve = stranger(2, boToken + 1, 0);
    auto eve2 = stranger(2, boToken, 1);
    auto eve3 = stranger(3, 0x1234, 2);
    CHECK(next.RejoinCount() == 1 && next.Roster().size() == 3);
    CHECK(lan.clients[1]->GetState() == LobbyClient::State::Joined && lan.clients[1]->LocalId() == 2);
    // A plan that is not a rejoin is refused.
    HandoverPlan none;
    CHECK(!eve->SetRejoin(none));

    // The window ends: Wim's seat is freed and the room goes on with Ana and Bo. Wim gave up first.
    lan.TickFollow(static_cast<int>(kSuccessorWaitMs / 16));
    CHECK(!next.Closed() && next.Roster().size() == 2 && NamedIn(next.Roster(), "Wim") == nullptr);
    CHECK(SameRoster(lan.clients[1]->Roster(), next.Roster()));
    CHECK(lan.clients[2]->GetState() == LobbyClient::State::Closed &&
          lan.clients[2]->CloseText() == "The host left, and the next host did not answer. The room is closed.");
    CHECK(next.RejoinTokenOf(3) == 0 && next.RejoinTokenOf(2) == 0);
}

// Four racers and a vote: the vote starts over on Ana's host under the same race index, with no ballots.
void TestHostLostInVote() {
    std::printf("the host is lost during a vote (D28.4)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.AddClient("Cy");
    lan.AddClient("Dee");
    lan.Settle();
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Voting));
    lan.clients[1]->Vote(3);
    CHECK(lan.host->Vote(5, lan.now));
    lan.Tick(10);
    CHECK(lan.host->Votes().size() == 2 && lan.clients[2]->Votes().size() == 2);
    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    CHECK(next.RejoinCount() == 3 && next.Roster().size() == 4 && lan.NextIn(Phase::Voting));
    const RoomState s = next.Room(lan.now);
    CHECK(s.raceIndex == 0 && s.phaseTotalMs == kVoteMs && next.Votes().empty());
    for (size_t i = 1; i < 4; i++) {
        CHECK(lan.clients[i]->GetState() == LobbyClient::State::Joined && lan.clients[i]->Votes().empty());
        CHECK(static_cast<size_t>(lan.clients[i]->LocalId()) == i + 1);
    }
    // The vote works as before: all four ballots close it.
    CHECK(next.Vote(3, lan.now));
    for (size_t i = 1; i < 4; i++) {
        lan.clients[i]->Vote(3);
    }
    lan.TickFollow(10);
    CHECK(lan.NextIn(Phase::Lottery) && next.Lottery().track == 3);
}

// A custom track on its way to Dee when the host is lost: that race is skipped, and the stream with it.
void TestHostLostInAssets() {
    std::printf("the host is lost while a custom track is on its way (D28.4)\n");
    std::vector<uint8_t> bytes;
    track::TrackId id{};
    CHECK(BuildTrack(90, bytes, id));
    Disk hostDisk;
    Disk anaDisk;
    Disk boDisk;
    Disk cyDisk;
    Disk deeDisk;
    hostDisk.tracks[id] = bytes;
    anaDisk.tracks[id] = bytes;
    boDisk.tracks[id] = bytes;
    cyDisk.tracks[id] = bytes;
    Lan lan("Host", 77, HackIdentity(), hostDisk.Store());
    lan.AddClient("Ana", Seat::Race, kBuild, 0x00FF00, anaDisk.Store());
    lan.AddClient("Bo", Seat::Race, kBuild, 0x00FF00, boDisk.Store());
    lan.AddClient("Cy", Seat::Race, kBuild, 0x00FF00, cyDisk.Store());
    lan.AddClient("Dee", Seat::Race, kBuild, 0x00FF00, deeDisk.Store());
    lan.Settle();
    CHECK(lan.host->SetCustomTracks(true, lan.now));
    CHECK(lan.host->Start(lan.now));
    lan.Tick(10);
    CHECK(lan.AllIn(Phase::Voting));
    TrackChoice custom;
    custom.track = kTrackCustom;
    std::copy(id.begin(), id.end(), custom.custom.id.begin());
    custom.custom.title = "Room test";
    // Every ballot names the track, so the lottery draws it whoever wins. Dee does not vote.
    CHECK(lan.host->Vote(custom, lan.now));
    for (size_t i = 0; i < 3; i++) {
        lan.clients[i]->Vote(custom);
    }
    for (int i = 0; i < 2500 && lan.HostPhase() == Phase::Voting; i++) {
        lan.Tick();
    }
    CHECK(lan.HostPhase() == Phase::Lottery && lan.host->Lottery().track == kTrackCustom);
    // From here Dee gets the first two chunks and then nothing large: her download stops part of the way.
    const NetAddress dee = lan.clientAddrs[3];
    int large = 0;
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t len) {
        // Only a datagram with a whole chunk in it: the room messages coalesce to over 600 bytes since protocol 7.
        return from == lan.hostAddr && to == dee && len > transfer::kChunkData && ++large > 2;
    });
    CHECK(lan.host->SkipLottery(lan.now));
    lan.Tick(10);
    CHECK(lan.host->Lock(LockOf(2, 3, 50), lan.now));
    for (size_t i = 0; i < 4; i++) {
        lan.clients[i]->Lock(LockOf(7, 0, 50));
    }
    lan.Tick(30);
    CHECK(lan.HostPhase() == Phase::Assets && !lan.host->HoldsTrack(4) && lan.host->HoldsTrack(1));
    const TrackProgress part = lan.clients[3]->Track();
    CHECK(part.active && !part.held && part.chunks == 2 && part.of > 2);

    lan.DropHost();
    lan.net.SetFilter(nullptr);
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    const RoomState s = next.Room(lan.now);
    CHECK(next.RejoinCount() == 3 && next.Roster().size() == 4);
    CHECK(lan.NextIn(Phase::Voting) && s.raceIndex == 1 && s.customTracks && s.track == kTrackNone);
    CHECK(!lan.clients[3]->Track().active && !lan.clients[3]->Track().held);
    CHECK(deeDisk.tracks.empty() && next.Track().active == false);
}

// Ana loses her own link while the host lives (decision 22). She promotes all the same; Bo and Cy still hear
// the host and stay. Nobody comes back to Ana, so her room closes at the end of the window, and the host's
// room goes on with Bo as the successor.
void TestSuccessorAlone() {
    std::printf("a successor that lost only its own link closes (D28.4)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.AddClient("Cy");
    lan.Settle();
    const NetAddress ana = lan.clientAddrs[0];
    CHECK(lan.host->Room(lan.now).successor == 1 && !lan.clients[0]->HandoverTable().empty());
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t) {
        return (from == ana && to == lan.hostAddr) || (from == lan.hostAddr && to == ana);
    });
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    LobbyHost& next = *lan.next;
    CHECK(lan.host->Roster().size() == 3 && NamedIn(lan.host->Roster(), "Ana") == nullptr);
    CHECK(lan.host->Room(lan.now).successor == 2);
    CHECK(lan.clients[1]->GetState() == LobbyClient::State::Joined && lan.clients[1]->Host() == lan.hostAddr);
    CHECK(lan.clients[1]->HandoverTable().size() == 1 && lan.clients[1]->HandoverTable()[0].id == 3);
    CHECK(!next.Closed() && next.RejoinCount() == 0);
    lan.TickFollow(static_cast<int>(kSuccessorWaitMs / 16) + 10);
    CHECK(next.Closed() && next.CloseText() == "Lost the connection to the room." && next.RejoinCount() == 0);
    CHECK(lan.clients[1]->GetState() == LobbyClient::State::Joined && lan.clients[2]->GetState() == LobbyClient::State::Joined);
    CHECK(lan.host->Roster().size() == 3 && lan.AllIn(Phase::Lobby));
}

// Decision 60: in a directory room, a member goes back only to a successor address it may send to.
void TestHandoverDialable() {
    std::printf("a directory room's member refuses a successor it should not send to (decision 60)\n");
    // Pass 0: an Open room, the successor on loopback, refused. Pass 1: the same with a local test directory.
    // Pass 2: a LAN room, never checked.
    for (int pass = 0; pass < 3; pass++) {
        Lan lan("Host", 82, {}, TrackStore(), pass == 2 ? RoomType::Lan : RoomType::Open);
        lan.clientAt = Addr(Ip(127, 0, 0, 1), 50000);
        lan.AddClient("Ana");
        lan.AddClient("Bo");
        if (pass == 1) {
            lan.clients[1]->AllowLoopback(true);
        }
        lan.Settle();
        CHECK(lan.host->Room(lan.now).successor == 1 &&
              lan.clients[1]->GetState() == LobbyClient::State::Joined);
        lan.DropHost();
        lan.Tick(static_cast<int>(kLinkTimeoutMs / 16) + 30);
        CHECK(lan.clients[0]->GetState() == LobbyClient::State::Closed &&
              lan.clients[0]->NextHost().kind == HandoverPlan::Kind::Promote);
        const HandoverPlan plan = lan.clients[1]->NextHost();
        CHECK(lan.clients[1]->GetState() == LobbyClient::State::Closed);
        if (pass == 0) {
            CHECK(plan.kind == HandoverPlan::Kind::None);
            CHECK(lan.clients[1]->CloseText().find("does not send to") != std::string::npos);
        } else {
            CHECK(plan.kind == HandoverPlan::Kind::Rejoin && plan.address == lan.clientAddrs[0]);
        }
    }
}

// A member with no token, and a successor with nobody to host, have nothing to go back to.
void TestNothingToFollow() {
    std::printf("no plan without a token or anyone to host (D28.4)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.Settle();
    CHECK(lan.clients[0]->HandoverTable().empty());
    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.clients[0]->GetState() == LobbyClient::State::Closed &&
          lan.clients[0]->NextHost().kind == HandoverPlan::Kind::None &&
          lan.clients[0]->CloseText() == "Lost the connection to the host.");
    CHECK(lan.next == nullptr);
}

// Ticks until the old host has handed the room over and gone, following every plan as it comes. Returns the
// frames it took; texts collects what each client said when it closed, before it was replaced.
int TickUntilHandedOver(Lan& lan, std::vector<std::string>& texts, int limit) {
    texts.assign(lan.clients.size(), std::string());
    for (int frame = 1; frame <= limit; frame++) {
        lan.Tick();
        for (size_t i = 0; i < lan.clients.size(); i++) {
            const bool fresh = lan.followed.size() <= i || !lan.followed[i];
            if (fresh && lan.clients[i]->GetState() == LobbyClient::State::Closed) {
                texts[i] = lan.clients[i]->CloseText();
            }
        }
        lan.FollowPlans();
        if (lan.host->Closed()) {
            return frame;
        }
    }
    return limit + 1;
}

// The host leaves mid-race (D28.5, decision 17): its machine is out and its CPUs with it, but the race runs
// to its results under the old host, and only then does the room go to Ana, at once and not after the silence.
void TestHostLeavesMidRace() {
    std::printf("the host leaves mid-race and the room goes on (D28.5)\n");
    Lan lan;
    lan.AddClient("Ana");
    lan.AddClient("Bo");
    lan.Settle();
    DriveTo(lan, Phase::Racing);
    const uint16_t race = lan.host->Room(lan.now).raceIndex;
    FinishReport out;
    out.raceIndex = race;
    out.kind = ResultKind::Retired;
    CHECK(lan.host->Finish(out, lan.now));
    CHECK(lan.host->HostLeftRace(lan.now));
    lan.host->Leave(lan.now);
    CHECK(lan.host->Leaving() && !lan.host->Closed() && lan.host->Successor() == 1);

    // Ten seconds on, the race still runs under the old host and nobody was told to move.
    lan.Tick(625);
    CHECK(lan.AllIn(Phase::Racing) && !lan.host->Closed());
    CHECK(lan.clients[0]->GetState() == LobbyClient::State::Joined &&
          lan.clients[1]->GetState() == LobbyClient::State::Joined);
    CHECK(lan.clients[0]->NextHost().kind == HandoverPlan::Kind::None);

    // The race ends; the results go out, and with them the Handover.
    lan.CapRace({ 1, 2 });
    CHECK(lan.host->Results().final && lan.host->Results().entries.size() == 3);
    std::vector<std::string> texts;
    const int frames = TickUntilHandedOver(lan, texts, static_cast<int>(kHandoverLingerMs / 16) + 30);
    CHECK(lan.host->Closed() && lan.host->CloseText() == "You left the room. Ana hosts it now.");
    CHECK(texts[1] == "The host left the room.");
    CHECK(frames * 16 < static_cast<int>(kLinkTimeoutMs)); // the message, not the silence
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    lan.TickFollow(30);
    LobbyHost& next = *lan.next;
    LobbyClient& bo = *lan.clients[1];
    CHECK(next.RejoinCount() == 1 && bo.GetState() == LobbyClient::State::Joined && bo.LocalId() == 2);
    CHECK(next.Roster().size() == 2 && NamedIn(next.Roster(), "Host") == nullptr);
    // The race scored: Ana and Bo keep the points it gave them. The old host's row left the table with it.
    const PlayerInfo* ana = NamedIn(next.Roster(), "Ana");
    const PlayerInfo* bob = NamedIn(next.Roster(), "Bo");
    CHECK(ana != nullptr && bob != nullptr && ana->points == PlacePoints(1) && bob->points == PlacePoints(2));
    RaceResults table;
    CHECK(bo.Results(table) && table.final && table.raceIndex == race && table.entries.size() == 2);
    CHECK(lan.NextIn(Phase::Results) && next.Room(lan.now).raceIndex == race);
    for (int i = 0; i < 1000 && next.Room(lan.now).phase == Phase::Results; i++) {
        lan.TickFollow(1);
    }
    lan.TickFollow(5);
    CHECK(lan.NextIn(Phase::Pick) && next.Room(lan.now).raceIndex == race + 1);
    lan.TickFollow(static_cast<int>(kSuccessorWaitMs / 16));
    CHECK(!next.Closed() && next.Roster().size() == 2 && next.Refused() == 0 && bo.Refused() == 0);
}

// ---------------------------------------------------------------------------------------------------------
// Direct member links (decisions 61 to 68)

int RaceSlotIn(const Grid& grid, uint8_t id) {
    for (size_t i = 0; i < grid.humans.size(); i++) {
        if (grid.humans[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// A pose of the slot at the tick, stamped with the host clock now, as the race code stamps it.
StatePose PoseAt(uint8_t slot, uint32_t tick, uint32_t hostMs) {
    StatePose pose = PoseOf(slot, tick);
    pose.timeMs = hostMs;
    return pose;
}

// Two members race with the host 100 ms from each and 60 ms from each other. Ana is behind a NAT that drops
// what comes in until she has sent to its source. The link opens, the poses go both ways, the copy that comes
// first is the one the game gets, and when the direct path breaks the host relay carries on alone.
void TestDirectLinksRace() {
    std::printf("members race over direct links, and the host relay stays\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    const NetAddress hostAddr = lan.hostAddr;
    const NetAddress anaAddr = lan.clientAddrs[0];
    std::vector<NetAddress> anaSentTo;
    bool cut = false;
    size_t linkDatagrams = 0;
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t) {
        if (from == hostAddr || to == hostAddr) {
            return false;
        }
        linkDatagrams++;
        if (cut) {
            return true;
        }
        if (from == anaAddr && std::find(anaSentTo.begin(), anaSentTo.end(), to) == anaSentTo.end()) {
            anaSentTo.push_back(to);
        }
        return to == anaAddr && std::find(anaSentTo.begin(), anaSentTo.end(), from) == anaSentTo.end();
    });
    lan.Settle();
    const uint8_t anaId = ana.LocalId();
    const uint8_t boId = bo.LocalId();
    CHECK(anaId != kSystemId && boId != kSystemId && anaId != boId);
    // The host told each about the other once both sent Paths, and the punches opened the pair at once.
    CHECK(ana.Links().PeerCount() == 1 && ana.Links().Known(boId) && bo.Links().Known(anaId));
    links::PathKind best = links::PathKind::Relay;
    CHECK(ana.Links().Open(boId, lan.now, best) && best == links::PathKind::Public);
    CHECK(bo.Links().Open(anaId, lan.now + lan.skew[1], best) && best == links::PathKind::Public);

    DriveTo(lan, Phase::Racing);
    const Grid grid = lan.host->RaceGrid();
    const int anaSlot = RaceSlotIn(grid, anaId);
    const int boSlot = RaceSlotIn(grid, boId);
    CHECK(anaSlot > 0 && boSlot > 0);
    if (anaSlot <= 0 || boSlot <= 0) {
        return;
    }
    lan.net.SetDelay([&](const NetAddress& from, const NetAddress& to) {
        return from == hostAddr || to == hostAddr ? 5 : 3; // 96 ms and 64 ms at 16 ms a Step
    });
    lan.Tick(20);

    std::map<uint8_t, size_t> boGot;
    std::map<uint8_t, size_t> anaGot;
    const auto drain = [&] {
        std::vector<StatePose> poses;
        uint16_t race = 0;
        bo.TakePoses(race, poses);
        for (const StatePose& pose : poses) {
            boGot[pose.slot]++;
        }
        ana.TakePoses(race, poses);
        for (const StatePose& pose : poses) {
            anaGot[pose.slot]++;
        }
    };
    drain();
    boGot.clear();
    anaGot.clear();
    const uint32_t sends = 60;
    for (uint32_t i = 0; i < sends; i++) {
        ana.SendPoses(grid.raceIndex, { PoseAt(static_cast<uint8_t>(anaSlot), 1000 + i, lan.now) });
        bo.SendPoses(grid.raceIndex, { PoseAt(static_cast<uint8_t>(boSlot), 1000 + i, lan.now) });
        lan.Tick();
        drain();
    }
    lan.Tick(30);
    drain();
    // Every pose reached the other game once, and each came first by the direct link: 64 ms against 192.
    CHECK(boGot[static_cast<uint8_t>(anaSlot)] == sends && anaGot[static_cast<uint8_t>(boSlot)] == sends);
    const PosePathStats& seenByBo = bo.PathStats();
    const size_t a = static_cast<size_t>(anaSlot);
    CHECK(seenByBo.raceIndex == grid.raceIndex && seenByBo.first[a][kPathDirect] == sends);
    CHECK(seenByBo.first[a][kPathHost] == 0 && seenByBo.first[a][kPathRelay] == 0);
    CHECK(seenByBo.aged[a][kPathDirect] == sends);
    const uint64_t directAge = seenByBo.aged[a][kPathDirect] > 0
                                   ? seenByBo.ageSumMs[a][kPathDirect] / seenByBo.aged[a][kPathDirect]
                                   : 0;
    CHECK(directAge >= 48 && directAge <= 112);
    CHECK(ana.PathStats().first[static_cast<size_t>(boSlot)][kPathDirect] == sends);
    CHECK(bo.Links().Stats().posesReceived[static_cast<size_t>(links::PathKind::Public)] == sends);
    CHECK(ana.Links().Stats().refused == 0 && bo.Links().Stats().refused == 0);
    CHECK(ana.Refused() == 0 && bo.Refused() == 0 && lan.host->Refused() == 0);

    // The direct path breaks. Nothing is lost: the host's copies arrive, now first, and the link closes.
    cut = true;
    for (uint32_t i = 0; i < sends; i++) {
        ana.SendPoses(grid.raceIndex, { PoseAt(static_cast<uint8_t>(anaSlot), 2000 + i, lan.now) });
        lan.Tick();
        drain();
    }
    lan.Tick(30);
    drain();
    CHECK(boGot[static_cast<uint8_t>(anaSlot)] == 2 * sends);
    CHECK(bo.PathStats().first[a][kPathHost] == sends && bo.PathStats().first[a][kPathDirect] == sends);
    const uint64_t hostAge = bo.PathStats().aged[a][kPathHost] > 0
                                 ? bo.PathStats().ageSumMs[a][kPathHost] / bo.PathStats().aged[a][kPathHost]
                                 : 0;
    CHECK(hostAge > directAge + 64);
    lan.Tick(static_cast<int>(links::kSilenceMs / 16) + 2);
    CHECK(!bo.Links().Open(anaId, lan.now + lan.skew[1], best));

    // A datagram made without the pair's secret is refused, and nothing reaches the game.
    cut = false;
    std::vector<StatePose> none;
    RaceState forged;
    forged.raceIndex = grid.raceIndex;
    forged.poses = { PoseAt(static_cast<uint8_t>(anaSlot), 5000, lan.now) };
    const std::vector<uint8_t> body = EncodeState(forged);
    const std::vector<uint8_t> datagram =
        links::BuildDatagram(Bytes32{ 9 }, links::kKindPoses, grid.raceIndex, anaId, boId, body.data(), body.size());
    const uint32_t refusedBefore = bo.Links().Stats().refused;
    bo.Receive(anaAddr, datagram.data(), datagram.size(), lan.now + lan.skew[1]);
    drain();
    CHECK(bo.Links().Stats().refused == refusedBefore + 1 && boGot[static_cast<uint8_t>(anaSlot)] == 2 * sends);
    CHECK(bo.Refused() == 0 && linkDatagrams > 0);

    // The link opens again. The race ends; Bo hears it a second before Ana, whose poses still come direct in
    // that second. Bo's game gets none of them: the links keep to the phases the host relays poses in.
    lan.Tick(20);
    CHECK(bo.Links().Open(anaId, lan.now + lan.skew[1], best));
    lan.net.SetDelay([&](const NetAddress& from, const NetAddress& to) {
        return from == hostAddr && to == anaAddr ? 60 : 0;
    });
    FinishReport retired;
    retired.raceIndex = grid.raceIndex;
    retired.kind = ResultKind::Retired;
    CHECK(lan.host->Finish(retired, lan.now));
    ana.Finish(retired);
    bo.Finish(retired);
    for (int i = 0; i < 20 && bo.Room(lan.now + lan.skew[1]).phase != Phase::Results; i++) {
        lan.Tick();
    }
    CHECK(bo.Room(lan.now + lan.skew[1]).phase == Phase::Results && ana.Room(lan.now).phase == Phase::Racing);
    drain();
    const uint32_t direct = bo.Links().Stats().posesReceived[static_cast<size_t>(links::PathKind::Public)];
    for (uint32_t i = 0; i < 10; i++) {
        ana.SendPoses(grid.raceIndex, { PoseAt(static_cast<uint8_t>(anaSlot), 6000 + i, lan.now) });
        lan.Tick();
    }
    drain();
    CHECK(ana.Room(lan.now).phase == Phase::Racing);
    CHECK(bo.Links().Stats().posesReceived[static_cast<size_t>(links::PathKind::Public)] > direct);
    CHECK(boGot[static_cast<uint8_t>(anaSlot)] == 2 * sends);
}

// Bo is behind a NAT that gives each destination its own outside port, so nothing he sends to Ana comes from
// the address the host listed. The pair never opens; the race runs on the host relay, and Ana's punches stop.
// Cy, watching, has direct links off: the host never tells anyone his address, and nobody sends him a link
// datagram.
void TestDirectLinksBlocked() {
    std::printf("a pair that cannot open, and a member with direct links off\n");
    Lan lan;
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& cy = lan.AddClient("Cy", Seat::Watch); // a watcher: three racers keep DriveTo on the pick path
    PathsInfo off;
    off.direct = false;
    cy.SetPaths(off);
    const NetAddress hostAddr = lan.hostAddr;
    const NetAddress boAddr = lan.clientAddrs[1];
    const NetAddress cyAddr = lan.clientAddrs[2];
    size_t toCy = 0;
    size_t fromCy = 0;
    lan.net.SetFilter([&](const NetAddress& from, const NetAddress& to, size_t) {
        if (from == hostAddr || to == hostAddr) {
            return false;
        }
        toCy += to == cyAddr ? 1 : 0;
        fromCy += from == cyAddr ? 1 : 0;
        return from == boAddr || to == boAddr;
    });
    lan.Settle();
    const uint8_t anaId = ana.LocalId();
    const uint8_t boId = bo.LocalId();
    const uint8_t cyId = cy.LocalId();
    CHECK(ana.Links().Known(boId) && !ana.Links().Known(cyId) && !bo.Links().Known(cyId));
    CHECK(cy.Links().PeerCount() == 0);
    DriveTo(lan, Phase::Racing);
    const Grid grid = lan.host->RaceGrid();
    const int boSlot = RaceSlotIn(grid, boId);
    CHECK(boSlot > 0);
    if (boSlot <= 0) {
        return;
    }
    // Long past the windows the entry and the race opened.
    for (int i = 0; i < 2 * static_cast<int>(links::kPunchWindowMs / 16); i++) {
        if (i % 4 == 0) {
            bo.SendPoses(grid.raceIndex, { PoseAt(static_cast<uint8_t>(boSlot), 3000 + static_cast<uint32_t>(i), lan.now) });
        }
        lan.Tick();
    }
    links::PathKind best = links::PathKind::Relay;
    CHECK(!ana.Links().Open(boId, lan.now, best));
    const size_t b = static_cast<size_t>(boSlot);
    CHECK(ana.PathStats().first[b][kPathDirect] == 0 && ana.PathStats().first[b][kPathHost] > 0);
    const uint32_t punches = ana.Links().Stats().punchesSent;
    CHECK(punches > 0 && punches <= 2 * (links::kPunchWindowMs / links::kPunchIntervalMs) + 2);
    lan.Tick(200);
    CHECK(ana.Links().Stats().punchesSent == punches); // no window is open
    CHECK(toCy == 0 && fromCy == 0 && cy.Links().Stats().punchesSent == 0);
    CHECK(ana.Refused() == 0 && bo.Refused() == 0 && cy.Refused() == 0 && lan.host->Refused() == 0);

    // Cy turns direct links on: now the others hear of him and link to him.
    PathsInfo on;
    cy.SetPaths(on);
    lan.Tick(60);
    CHECK(ana.Links().Known(cyId) && cy.Links().Known(anaId) && cy.Links().Known(boId));
    CHECK(ana.Links().Open(cyId, lan.now, best) && best == links::PathKind::Public);
    // And off again: the host tells the others to forget him.
    cy.SetPaths(off);
    lan.Tick(30);
    CHECK(!ana.Links().Known(cyId) && !bo.Links().Known(cyId) && cy.Links().PeerCount() == 0);
}

// Ana and Bo share one public address, behind one router, and each told the host its LAN address. The host
// gives each the other's LAN address, and the pair links over the LAN. Cy, elsewhere, never hears of them.
void TestDirectLinksLan() {
    std::printf("two members behind one router link over their LAN\n");
    Lan lan;
    const uint32_t shared = Ip(203, 0, 113, 5);
    lan.clientAt = Addr(shared, 50010);
    LobbyClient& ana = lan.AddClient("Ana");
    lan.clientAt = Addr(shared, 50011);
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& cy = lan.AddClient("Cy");
    const NetAddress anaLan = Addr(Ip(10, 0, 0, 2), 24302);
    const NetAddress boLan = Addr(Ip(10, 0, 0, 3), 24302);
    const NetAddress anaPub = lan.clientAddrs[0];
    const NetAddress boPub = lan.clientAddrs[1];
    lan.net.Add(anaLan, [&lan](const NetAddress& from, const uint8_t* d, size_t n) {
        lan.clients[0]->Receive(from, d, n, lan.now + lan.skew[0]);
    });
    lan.net.Add(boLan, [&lan](const NetAddress& from, const uint8_t* d, size_t n) {
        lan.clients[1]->Receive(from, d, n, lan.now + lan.skew[1]);
    });
    // On the LAN a copy's datagrams come from its LAN address.
    lan.net.SetSource([&](const NetAddress& from, const NetAddress& to) {
        if (to == boLan && from == anaPub) {
            return anaLan;
        }
        if (to == anaLan && from == boPub) {
            return boLan;
        }
        return from;
    });
    PathsInfo anaPaths;
    anaPaths.lan = { links::EndpointOf(anaLan) };
    ana.SetPaths(anaPaths);
    PathsInfo boPaths;
    boPaths.lan = { links::EndpointOf(boLan), links::EndpointOf(Addr(Ip(169, 254, 0, 9), 24302)) };
    bo.SetPaths(boPaths);
    CHECK(bo.Paths().lan.size() == 1); // a link-local address is not one the host would take
    PathsInfo cyPaths;
    cyPaths.lan = { links::EndpointOf(Addr(Ip(10, 0, 0, 4), 24302)) };
    cy.SetPaths(cyPaths);
    lan.Settle();
    const uint8_t anaId = ana.LocalId();
    const uint8_t boId = bo.LocalId();
    const uint8_t cyId = cy.LocalId();
    links::PathKind best = links::PathKind::Relay;
    CHECK(ana.Links().Open(boId, lan.now, best) && best == links::PathKind::Lan);
    CHECK(bo.Links().Open(anaId, lan.now + lan.skew[1], best) && best == links::PathKind::Lan);
    // Cy links to both by their public address only.
    CHECK(cy.Links().Open(anaId, lan.now + lan.skew[2], best) && best == links::PathKind::Public);
    CHECK(ana.Links().Open(cyId, lan.now, best) && best == links::PathKind::Public);
    CHECK(ana.Refused() == 0 && bo.Refused() == 0 && cy.Refused() == 0 && lan.host->Refused() == 0);
}

// A member's Paths the host will not take, the Peers a raw member is sent, and a stray link datagram.
void TestDirectLinksRefusals() {
    std::printf("the host and a member refuse link messages that break the rules\n");
    Lan lan;
    RawPeer raw(lan, 40, kProtocolVersion);
    raw.Run(20);
    JoinRequest join;
    join.build = kBuild;
    join.name = "Raw";
    join.colour = 0x445566;
    join.machine = 2;
    raw.Send(kMsgJoin, EncodeJoin(join));
    raw.Run(10);
    LobbyClient& ana = lan.AddClient("Ana");
    PathsInfo anaPaths;
    anaPaths.lan = { links::EndpointOf(Addr(Ip(10, 0, 0, 2), 24302)) };
    anaPaths.relay = links::EndpointOf(Addr(Ip(198, 51, 100, 7), 40001));
    ana.SetPaths(anaPaths);
    raw.Run(90);
    const uint8_t rawId = 1;
    CHECK(FindIn(lan.host->Roster(), rawId) != nullptr && ana.LocalId() == 2);
    // Ana sent Paths; Raw has not, so neither hears of the other.
    CHECK(ana.Links().PeerCount() == 0 && raw.Count(kMsgPeers) == 0);
    const uint32_t refused = lan.host->Refused();
    std::vector<uint8_t> bad = EncodePaths(PathsInfo());
    bad.back() = 2; // direct is 0 or 1
    raw.Send(kMsgPaths, bad);
    raw.Run(10);
    CHECK(lan.host->Refused() == refused + 1 && ana.Links().PeerCount() == 0);
    // A good Paths: Ana hears of Raw, and Raw is told Ana's public address. Not her LAN one: she is not behind
    // Raw's address. Nor her relay one: Raw has none to send from.
    raw.Send(kMsgPaths, EncodePaths(PathsInfo()));
    raw.Run(20);
    CHECK(lan.host->Refused() == refused + 1 && ana.Links().Known(rawId) && raw.Count(kMsgPeers) == 1);
    std::vector<PeerEntry> told;
    const Message* last = raw.Last(kMsgPeers);
    CHECK(last != nullptr && DecodePeers(last->payload.data(), last->payload.size(), told) && told.size() == 1 &&
          told[0].id == 2 && told[0].pub == links::EndpointOf(lan.clientAddrs[0]) && told[0].lan.empty() &&
          told[0].relay.None() && !told[0].Forget());
    const Bytes32 secret = told.empty() ? Bytes32{} : told[0].secret;
    // The same Paths again changes nothing and sends nothing.
    raw.Send(kMsgPaths, EncodePaths(PathsInfo()));
    raw.Run(20);
    CHECK(raw.Count(kMsgPeers) == 1);
    // Raw turns direct links off: Ana is told to forget it, and Raw to forget Ana.
    PathsInfo off;
    off.direct = false;
    raw.Send(kMsgPaths, EncodePaths(off));
    raw.Run(20);
    CHECK(!ana.Links().Known(rawId) && raw.Count(kMsgPeers) == 2);
    last = raw.Last(kMsgPeers);
    CHECK(last != nullptr && DecodePeers(last->payload.data(), last->payload.size(), told) && told.size() == 1 &&
          told[0].id == 2 && told[0].Forget());
    // On again, now with a relay: a pair keeps its secret while both stay in the room, and the relays meet.
    PathsInfo relayed;
    relayed.relay = links::EndpointOf(Addr(Ip(198, 51, 100, 8), 40002));
    raw.Send(kMsgPaths, EncodePaths(relayed));
    raw.Run(20);
    last = raw.Last(kMsgPeers);
    CHECK(raw.Count(kMsgPeers) == 3 && last != nullptr &&
          DecodePeers(last->payload.data(), last->payload.size(), told) && told.size() == 1 &&
          told[0].secret == secret && told[0].relay == anaPaths.relay && !told[0].pub.None());
    CHECK(ana.Refused() == 0);
    // A copy that has not joined takes no link datagram, and counts nothing for one.
    LobbyClient late(kBuild, Profile("Late", 1, 1), Seat::Race, lan.hostAddr, 0x9999u,
                     lan.net.SenderFor(Addr(Ip(192, 168, 1, 41), 50041)), TrackStore());
    const std::vector<uint8_t> punch = links::BuildDatagram(secret, links::kKindPunch, 0, rawId, 2, nullptr, 0);
    late.Receive(lan.clientAddrs[0], punch.data(), punch.size(), lan.now);
    CHECK(late.Links().Stats().refused == 0 && late.Refused() == 0);
}

// Between races the room goes over at once. With nobody to host it, Leave is a close.
void TestHostLeavesBetweenRaces() {
    std::printf("the host leaves between races (D28.5)\n");
    {
        Lan lan;
        lan.AddClient("Ana");
        lan.AddClient("Bo");
        lan.AddClient("Cy", Seat::Watch);
        lan.Settle();
        lan.host->Leave(lan.now);
        lan.host->Leave(lan.now); // the second call changes nothing
        CHECK(lan.host->Leaving() && !lan.host->Closed());
        std::vector<std::string> texts;
        const int frames = TickUntilHandedOver(lan, texts, static_cast<int>(kHandoverLingerMs / 16) + 30);
        CHECK(lan.host->Closed() && frames * 16 <= static_cast<int>(kHandoverLingerMs) + 32);
        CHECK(texts[0] == "The host left the room." && texts[1] == texts[0] && texts[2] == texts[0]);
        lan.TickFollow(30);
        CHECK(lan.next != nullptr && lan.nextAt == 0);
        if (lan.next != nullptr) {
            CHECK(lan.next->RejoinCount() == 2 && lan.next->Roster().size() == 3);
            CHECK(lan.clients[1]->LocalId() == 2 && lan.clients[2]->LocalId() == 3);
            CHECK(lan.NextIn(Phase::Lobby) && lan.next->Room(lan.now).successor == 2);
        }
        // The old host answers nobody now, not even a search.
        const size_t before = lan.host->PeerCount();
        lan.host->Update(lan.now);
        CHECK(before == 0 && lan.host->PeerCount() == 0);
    }
    {
        // Ana alone is the successor with nobody to host: she is told, and the room ends with the host gone.
        Lan lan;
        lan.AddClient("Ana");
        lan.Settle();
        lan.host->Leave(lan.now);
        std::vector<std::string> texts;
        TickUntilHandedOver(lan, texts, static_cast<int>(kHandoverLingerMs / 16) + 30);
        CHECK(lan.host->Closed() && texts[0] == "The host left the room." && lan.next == nullptr);
        CHECK(lan.clients[0]->NextHost().kind == HandoverPlan::Kind::None);
    }
    {
        Lan lan;
        lan.Settle();
        lan.host->Leave(lan.now);
        CHECK(lan.host->Closed() && lan.host->CloseText() == "You closed the room. Nobody was left to host it.");
    }
}

void TestHandoverTokens() {
    std::printf("handover tokens and a promoted host (D28.3)\n");
    Lan lan;
    uint64_t next = 0x1000;
    lan.host->SetTokenSource([&next]() { return ++next; });
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& wim = lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    const uint8_t anaId = ana.LocalId();
    const uint8_t boId = bo.LocalId();
    const uint8_t wimId = wim.LocalId();

    // Each member holds a token of its own, the one the host keeps for that id.
    CHECK(ana.HandoverTokenOwn() != 0 && ana.HandoverTokenOwn() == lan.host->TokenOf(anaId));
    CHECK(bo.HandoverTokenOwn() != 0 && bo.HandoverTokenOwn() == lan.host->TokenOf(boId));
    CHECK(wim.HandoverTokenOwn() != 0 && wim.HandoverTokenOwn() == lan.host->TokenOf(wimId));
    CHECK(ana.HandoverTokenOwn() != bo.HandoverTokenOwn() && bo.HandoverTokenOwn() != wim.HandoverTokenOwn());
    CHECK(lan.host->TokenOf(kHostId) == 0);
    // The first racer to join is next, and only it holds the table, which leaves out its own token.
    CHECK(lan.host->Room(lan.now).successor == anaId && bo.Room(lan.now).successor == anaId);
    CHECK(bo.HandoverTable().empty() && wim.HandoverTable().empty());
    const std::vector<HandoverTokenEntry>& table = ana.HandoverTable();
    CHECK(table.size() == 2);
    CHECK(table.size() == 2 && table[0].id == boId && table[0].token == bo.HandoverTokenOwn() &&
          table[1].id == wimId && table[1].token == wim.HandoverTokenOwn());

    // A member leaves: every token is reissued, and the table follows.
    const uint64_t anaOld = ana.HandoverTokenOwn();
    const uint64_t boOld = bo.HandoverTokenOwn();
    wim.Leave(lan.now);
    lan.Tick(20);
    CHECK(lan.host->TokenOf(wimId) == 0);
    CHECK(ana.HandoverTokenOwn() != anaOld && ana.HandoverTokenOwn() == lan.host->TokenOf(anaId));
    CHECK(bo.HandoverTokenOwn() != boOld && bo.HandoverTokenOwn() == lan.host->TokenOf(boId));
    CHECK(ana.HandoverTable().size() == 1);
    CHECK(ana.HandoverTable().size() == 1 && ana.HandoverTable()[0].id == boId &&
          ana.HandoverTable()[0].token == bo.HandoverTokenOwn());

    // Ana's mirror becomes a host of its own: Ana is player 0, Bo keeps his seat and id and has to come
    // back with his token, and the room keeps its id.
    std::optional<RoomMachine> machine = RoomMachine::Promote(ana.Mirror(lan.now), anaId, lan.now);
    CHECK(machine.has_value());
    if (!machine.has_value()) {
        return;
    }
    const NetAddress newAddr = Addr(Ip(192, 168, 1, 40), 50040);
    LobbyHost promoted(kBuild, Profile("Ana", 0x00FF00, 3), newAddr.port, std::move(*machine), ana.HandoverTable(),
                       anaId, lan.net.SenderFor(newAddr));
    lan.net.Add(newAddr,
                [&](const NetAddress& from, const uint8_t* d, size_t n) { promoted.Receive(from, d, n, lan.now); });
    CHECK(promoted.PromotedFrom() == anaId);
    CHECK(promoted.RejoinTokenOf(boId) == bo.HandoverTokenOwn());
    CHECK(promoted.RejoinTokenOf(kHostId) == 0 && promoted.RejoinTokenOf(anaId) == 0 &&
          promoted.RejoinTokenOf(wimId) == 0);
    CHECK(promoted.Roster().size() == 2 && promoted.Roster()[0].id == kHostId &&
          promoted.Roster()[0].name == "Ana" && promoted.Roster()[1].id == boId);
    CHECK(promoted.Room(lan.now).roomId == 77 && promoted.Room(lan.now).phase == Phase::Lobby);

    // It takes a new member like any host. Bo holds his seat without a link, so the newcomer is next.
    const NetAddress cyAddr = Addr(Ip(192, 168, 1, 41), 50041);
    LobbyClient cy(kBuild, Profile("Cy", 0x0000FF, 3), Seat::Race, newAddr, 0x2000, lan.net.SenderFor(cyAddr));
    lan.net.Add(cyAddr, [&](const NetAddress& from, const uint8_t* d, size_t n) { cy.Receive(from, d, n, lan.now); });
    cy.Start(lan.now);
    for (int i = 0; i < 90; i++) {
        lan.now += 16;
        lan.net.Step();
        promoted.Update(lan.now);
        cy.Update(lan.now);
    }
    CHECK(cy.GetState() == LobbyClient::State::Joined);
    CHECK(cy.Roster().size() == 3 && cy.Room(lan.now).roomId == 77);
    CHECK(cy.LocalId() != boId && cy.LocalId() != kHostId);
    CHECK(cy.HandoverTokenOwn() != 0 && cy.HandoverTokenOwn() == promoted.TokenOf(cy.LocalId()));
    CHECK(cy.Room(lan.now).successor == cy.LocalId());
    CHECK(promoted.TokenOf(boId) == 0 && promoted.RejoinTokenOf(boId) == bo.HandoverTokenOwn());
}

void TestBrowser() {
    std::printf("browser\n");
    Lan lan("LanHost");
    lan.AddClient("Ana");
    lan.AddClient("Wim", Seat::Watch);
    lan.Settle();
    const NetAddress me = Addr(Ip(192, 168, 1, 77), 41000);
    const std::vector<NetAddress> targets = { Addr(0xFFFFFFFFu, kDefaultPort), Addr(Ip(127, 0, 0, 1), kDefaultPort) };
    lan.browsers.push_back(std::make_unique<LanBrowser>(targets, 0xABCD, lan.net.SenderFor(me)));
    LanBrowser& browser = *lan.browsers.back();
    lan.net.Add(me, [&](const NetAddress& from, const uint8_t* d, size_t n) { browser.Receive(from, d, n, lan.now); });
    lan.Tick(10);
    CHECK(browser.Rooms().size() == 1);
    if (!browser.Rooms().empty()) {
        const LanBrowser::FoundRoom& found = browser.Rooms()[0];
        CHECK(found.address == lan.hostAddr);
        CHECK(found.room.hostName == "LanHost" && found.room.racers == 2 && found.room.racerCap == kDefaultRacers &&
              found.room.watchers == 1 && found.room.phase == Phase::Lobby && found.room.build == kBuild);
        CHECK(found.room.RaceSeatFree() && found.room.WatchSeatFree());
    }
    // The list follows the room: the next probe's answer says the track vote is open.
    CHECK(lan.host->Start(lan.now));
    lan.Tick(100);
    CHECK(browser.Rooms().size() == 1 && browser.Rooms()[0].room.phase == Phase::Voting);
    // A reply for another browser's probe, and noise, are not rooms.
    RoomInfo other;
    other.port = 1;
    other.racers = 1;
    other.racerCap = 8;
    other.build = "b";
    other.hostName = "Other";
    const std::vector<uint8_t> foreign = EncodeRoom(0x1111, other);
    CHECK(!browser.Receive(Addr(Ip(1, 2, 3, 4), 5), foreign.data(), foreign.size(), lan.now));
    const uint8_t junk[] = { 'G', 'D', 'X', 'L', 2 };
    CHECK(!browser.Receive(Addr(Ip(1, 2, 3, 4), 5), junk, sizeof(junk), lan.now));
    CHECK(browser.Rooms().size() == 1);
    // The host goes away: the room drops off the list after kRoomExpireMs.
    lan.net.Cut(lan.hostAddr);
    lan.Tick(200);
    CHECK(browser.Rooms().size() == 1);
    lan.Tick(200);
    CHECK(browser.Rooms().empty());
}

} // namespace

// ---- O7.3: friend keys and the join proof ----

using Seed = std::array<unsigned char, gdx::net::friends::kSeedBytes>;

Seed SeedOf(unsigned char b) {
    Seed seed;
    seed.fill(b);
    return seed;
}

Bytes32 FriendKeyOf(const Seed& seed) {
    gdx::net::friends::Key key{};
    gdx::net::friends::DeriveFriendKey(seed.data(), key);
    Bytes32 out{};
    std::memcpy(out.data(), key.data(), out.size());
    return out;
}

enum class Tamper { None, Proof, Room, Host };

// What the game's signer does with a profile seed, or one of the ways a proof goes wrong.
JoinSigner SignerOf(const Seed& seed, Tamper tamper = Tamper::None) {
    return [seed, tamper](const JoinNonce& nonce, Bytes32& key, std::array<uint8_t, 64>& proof) {
        gdx::net::friends::Key host{};
        gdx::net::friends::Nonce bytes{};
        gdx::net::friends::Signature signature{};
        std::memcpy(host.data(), nonce.hostKey.data(), host.size());
        std::memcpy(bytes.data(), nonce.nonce.data(), bytes.size());
        if (tamper == Tamper::Host) {
            host[0] ^= 1;
        }
        const uint32_t room = tamper == Tamper::Room ? nonce.roomId + 1 : nonce.roomId;
        gdx::net::friends::JoinProofSign(seed.data(), room, host, bytes, signature);
        if (tamper == Tamper::Proof) {
            signature[10] ^= 0x40;
        }
        key = FriendKeyOf(seed);
        std::memcpy(proof.data(), signature.data(), proof.size());
        return true;
    };
}

void SignNext(Lan& lan, const Seed& seed, Tamper tamper = Tamper::None) {
    lan.clientSigner = SignerOf(seed, tamper);
    lan.clientKey = FriendKeyOf(seed);
}

bool ClosedWith(const LobbyClient& c, RejectReason reason) {
    return c.GetState() == LobbyClient::State::Closed && c.CloseText() == RejectReasonText(reason);
}

void TestKeyCodecs() {
    std::printf("nonce and keys codecs (O7.3)\n");
    JoinNonce nonce;
    nonce.roomId = 0xA1B2C3D4u;
    nonce.hostKey = FriendKeyOf(SeedOf(1));
    nonce.nonce.fill(0x5A);
    std::vector<uint8_t> bytes = EncodeNonce(nonce);
    JoinNonce back;
    CHECK(bytes.size() == 68 && DecodeNonce(bytes.data(), bytes.size(), back) && back == nonce);
    bytes.push_back(0);
    CHECK(!DecodeNonce(bytes.data(), bytes.size(), back));
    bytes.resize(67);
    CHECK(!DecodeNonce(bytes.data(), bytes.size(), back));

    const std::vector<MemberKey> keys = { { 0, FriendKeyOf(SeedOf(1)) }, { 3, FriendKeyOf(SeedOf(2)) } };
    std::vector<MemberKey> got;
    bytes = EncodeKeys(keys);
    CHECK(bytes.size() == 1 + 2 * 33 && DecodeKeys(bytes.data(), bytes.size(), got) && got == keys);
    CHECK(DecodeKeys(EncodeKeys({}).data(), 1, got) && got.empty());
    // Ids out of order, twice, past the last seat, and an all-zero key are refused.
    const std::vector<std::vector<MemberKey>> bad = {
        { keys[1], keys[0] },
        { keys[0], { 0, keys[1].key } },
        { { static_cast<uint8_t>(kMaxPlayers), keys[0].key } },
        { { 2, Bytes32{} } },
    };
    for (const std::vector<MemberKey>& list : bad) {
        got = keys;
        bytes = EncodeKeys(list);
        CHECK(!DecodeKeys(bytes.data(), bytes.size(), got) && got == keys);
    }
    bytes = EncodeKeys(keys);
    bytes.push_back(0);
    CHECK(!DecodeKeys(bytes.data(), bytes.size(), got));

    // A Join round trips its key and proof; a proof with no key is refused.
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    join.colour = 0x112233;
    join.machine = 4;
    join.key = FriendKeyOf(SeedOf(2));
    join.proof.fill(0x33);
    bytes = EncodeJoin(join);
    JoinRequest read;
    CHECK(DecodeJoin(bytes.data(), bytes.size(), read) && read.key == join.key && read.proof == join.proof);
    join.key = Bytes32{};
    bytes = EncodeJoin(join);
    CHECK(!DecodeJoin(bytes.data(), bytes.size(), read));
    join.proof.fill(0);
    bytes = EncodeJoin(join);
    CHECK(DecodeJoin(bytes.data(), bytes.size(), read) && IsZero32(read.key));
}

// A LAN room lets in a key with a good proof and a copy with none; every member learns the keys; the host bans a
// key and that key stays out.
void TestKeysInARoom() {
    std::printf("keys in a room and ban by key (O7.3)\n");
    const Seed hostSeed = SeedOf(1);
    const Seed anaSeed = SeedOf(2);
    Lan lan;
    lan.host->SetIdentity(FriendKeyOf(hostSeed));
    SignNext(lan, anaSeed);
    LobbyClient& ana = lan.AddClient("Ana");
    LobbyClient& bo = lan.AddClient("Bo");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined && bo.GetState() == LobbyClient::State::Joined);
    const std::vector<MemberKey> keys = lan.host->Keys();
    CHECK(keys.size() == 2 && keys[0].id == kHostId && keys[0].key == FriendKeyOf(hostSeed));
    CHECK(keys.size() == 2 && keys[1].id == ana.LocalId() && keys[1].key == FriendKeyOf(anaSeed));
    CHECK(ana.Keys() == keys && bo.Keys() == keys);

    CHECK(lan.host->Ban(ana.LocalId(), lan.now));
    lan.Settle();
    CHECK(ClosedWith(ana, RejectReason::Banned));
    CHECK(lan.host->Banned().size() == 1 && lan.host->Banned()[0] == FriendKeyOf(anaSeed));
    CHECK(bo.Keys().size() == 1 && bo.Keys()[0].id == kHostId); // a member who goes takes its key along

    // The key is refused when it comes back, under any name.
    SignNext(lan, anaSeed);
    LobbyClient& again = lan.AddClient("Anna");
    lan.Settle();
    CHECK(ClosedWith(again, RejectReason::Banned));
    CHECK(lan.host->Roster().size() == 2);

    // A player with no key is removed, and nothing is added to the list. Nobody else can be banned.
    CHECK(lan.host->Ban(bo.LocalId(), lan.now));
    lan.Settle();
    CHECK(ClosedWith(bo, RejectReason::Banned) && lan.host->Banned().size() == 1);
    // Its address is refused instead, from any port; so is Ana's, even with no key. Another address still joins.
    lan.clientAt = Addr(Ip(192, 168, 1, 21), 50999);
    LobbyClient& boAgain = lan.AddClient("Bo");
    lan.clientAt = Addr(Ip(192, 168, 1, 20), 50998);
    LobbyClient& anaKeyless = lan.AddClient("Ana");
    LobbyClient& cy = lan.AddClient("Cy");
    lan.Settle();
    CHECK(ClosedWith(boAgain, RejectReason::Banned) && ClosedWith(anaKeyless, RejectReason::Banned));
    CHECK(cy.GetState() == LobbyClient::State::Joined);
    CHECK(!lan.host->Ban(kHostId, lan.now) && !lan.host->Ban(9, lan.now) && !lan.host->Ban(200, lan.now));
}

// A Friends room admits the keys on its host's list, and only with a proof made for this room, this host and this
// link's nonce.
void TestFriendsRoom() {
    std::printf("a room for friends (O7.3)\n");
    const Seed hostSeed = SeedOf(1);
    const Seed anaSeed = SeedOf(2);
    const Seed boSeed = SeedOf(3);
    Lan lan("Host", 78, {}, TrackStore(), RoomType::Friends);
    lan.host->SetIdentity(FriendKeyOf(hostSeed));
    lan.host->SetFriends({ FriendKeyOf(anaSeed) });
    SignNext(lan, anaSeed);
    LobbyClient& ana = lan.AddClient("Ana");
    SignNext(lan, boSeed);
    LobbyClient& bo = lan.AddClient("Bo");
    LobbyClient& none = lan.AddClient("Cy");
    SignNext(lan, anaSeed, Tamper::Proof);
    LobbyClient& forged = lan.AddClient("Dee");
    SignNext(lan, anaSeed, Tamper::Room);
    LobbyClient& elsewhere = lan.AddClient("Eve");
    SignNext(lan, anaSeed, Tamper::Host);
    LobbyClient& otherHost = lan.AddClient("Fay");
    lan.Settle();
    CHECK(ana.GetState() == LobbyClient::State::Joined);
    CHECK(ClosedWith(bo, RejectReason::NotFriend));        // a key the host never added
    CHECK(ClosedWith(none, RejectReason::NotFriend));      // no key at all
    CHECK(ClosedWith(forged, RejectReason::BadProof));     // a listed key, and a signature that does not check
    CHECK(ClosedWith(elsewhere, RejectReason::BadProof));  // a proof made for another room
    CHECK(ClosedWith(otherHost, RejectReason::BadProof));  // a proof made for another host
    CHECK(lan.host->Roster().size() == 2);

    // A proof that was good on one link does not open another: each link signs its own nonce.
    RawPeer first(lan, 230, kProtocolVersion);
    RawPeer second(lan, 231, kProtocolVersion);
    for (int i = 0; i < 20; i++) {
        lan.Tick();
        first.Turn();
        second.Turn();
    }
    const Message* seen = first.Last(kMsgNonce);
    JoinNonce nonce;
    CHECK(seen != nullptr && DecodeNonce(seen->payload.data(), seen->payload.size(), nonce));
    CHECK(nonce.roomId == 78 && nonce.hostKey == FriendKeyOf(hostSeed));
    CHECK(nonce.nonce == lan.host->NonceFor(first.addr) && nonce.nonce != lan.host->NonceFor(second.addr));
    JoinRequest join;
    join.build = kBuild;
    join.name = "Gil";
    join.colour = 0x445566;
    join.machine = 2;
    CHECK(SignerOf(anaSeed)(nonce, join.key, join.proof));
    second.Send(kMsgJoin, EncodeJoin(join));
    first.Send(kMsgJoin, EncodeJoin(join));
    for (int i = 0; i < 20; i++) {
        lan.Tick();
        first.Turn();
        second.Turn();
    }
    RejectReason why = RejectReason::Full;
    const Message* refusal = second.Last(kMsgReject);
    CHECK(refusal != nullptr && DecodeReject(refusal->payload.data(), refusal->payload.size(), why) &&
          why == RejectReason::BadProof);
    CHECK(first.Last(kMsgWelcome) != nullptr && first.Last(kMsgReject) == nullptr);
    CHECK(lan.host->Roster().size() == 3);
}

// A client that knows which host it wants refuses a Nonce from any other, before it sends a byte of its profile.
void TestExpectedHostKey() {
    std::printf("the host key a client expects (O7.3)\n");
    Lan lan;
    lan.host->SetIdentity(FriendKeyOf(SeedOf(1)));
    lan.clientExpect = FriendKeyOf(SeedOf(9));
    LobbyClient& wrong = lan.AddClient("Ana");
    lan.clientExpect = FriendKeyOf(SeedOf(1));
    LobbyClient& right = lan.AddClient("Bo");
    lan.Settle();
    CHECK(wrong.GetState() == LobbyClient::State::Closed &&
          wrong.CloseText() == "The room at that address is not the room you asked for.");
    CHECK(right.GetState() == LobbyClient::State::Joined && lan.host->Roster().size() == 2);
}

// The keys outlive the host: the successor learns them from its mirror and sends them again.
void TestKeysAfterHandover() {
    std::printf("keys after the host is lost (O7.3)\n");
    const Seed anaSeed = SeedOf(2);
    const Seed boSeed = SeedOf(3);
    Lan lan;
    lan.host->SetIdentity(FriendKeyOf(SeedOf(1)));
    SignNext(lan, anaSeed);
    lan.AddClient("Ana");
    SignNext(lan, boSeed);
    lan.AddClient("Bo");
    lan.Settle();
    CHECK(lan.clients[1]->Keys().size() == 3);
    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 60);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    // Ana hosts as player 0; Bo kept id 2.
    const std::vector<MemberKey> keys = lan.next->Keys();
    CHECK(keys.size() == 2 && keys[0].id == kHostId && keys[0].key == FriendKeyOf(anaSeed));
    CHECK(keys.size() == 2 && keys[1].id == 2 && keys[1].key == FriendKeyOf(boSeed));
    CHECK(lan.clients[1]->GetState() == LobbyClient::State::Joined && lan.clients[1]->Keys() == keys);
}

// Protocol 9 (decisions 50 to 52): the reachable flag in Join, Reach, and the bans in the handover table.
void TestReachAndBanCodecs() {
    std::printf("reach and ban codecs (protocol 9)\n");
    JoinRequest join;
    join.build = kBuild;
    join.name = "Ana";
    join.colour = 0x112233;
    join.machine = 4;
    std::vector<uint8_t> bytes = EncodeJoin(join);
    JoinRequest read;
    read.reachable = true;
    CHECK(DecodeJoin(bytes.data(), bytes.size(), read) && !read.reachable && bytes.back() == 0);
    join.reachable = true;
    bytes = EncodeJoin(join);
    CHECK(DecodeJoin(bytes.data(), bytes.size(), read) && read.reachable && bytes.back() == 1);
    bytes.back() = 2;
    CHECK(!DecodeJoin(bytes.data(), bytes.size(), read));
    bytes.pop_back(); // a protocol 8 Join, with no flag
    CHECK(!DecodeJoin(bytes.data(), bytes.size(), read));

    bool reach = false;
    bytes = EncodeReach(true);
    CHECK(bytes.size() == 1 && DecodeReach(bytes.data(), bytes.size(), reach) && reach);
    bytes = EncodeReach(false);
    CHECK(DecodeReach(bytes.data(), bytes.size(), reach) && !reach);
    const uint8_t two = 2;
    reach = true;
    CHECK(!DecodeReach(&two, 1, reach) && reach);
    CHECK(!DecodeReach(nullptr, 0, reach));
    bytes = { 1, 0 };
    CHECK(!DecodeReach(bytes.data(), bytes.size(), reach));

    // The bans ride after the takeover secret.
    BanList bans;
    bans.keys = { FriendKeyOf(SeedOf(4)), FriendKeyOf(SeedOf(5)) };
    bans.prefixes = { MappedV4(Ip(192, 168, 1, 30)), IpBytes{ 0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2 } };
    const std::vector<HandoverTokenEntry> tokens = { { 1, 11 }, { 2, 22 } };
    Bytes32 secret{};
    secret.fill(0x77);
    bytes = EncodeHandoverTokens(tokens, secret, bans);
    std::vector<HandoverTokenEntry> tokensOut;
    Bytes32 secretOut{};
    BanList bansOut;
    CHECK(bytes.size() == 1 + 2 * 9 + 32 + 1 + 2 * 32 + 1 + 2 * 16);
    CHECK(DecodeHandoverTokens(bytes.data(), bytes.size(), tokensOut, secretOut, bansOut) && tokensOut == tokens &&
          secretOut == secret && bansOut == bans);
    CheckEdges(bytes, [](const uint8_t* d, size_t n) {
        std::vector<HandoverTokenEntry> o;
        Bytes32 s{};
        BanList b;
        return DecodeHandoverTokens(d, n, o, s, b);
    });
    // Each refusal leaves every output alone.
    const auto refuses = [&](const BanList& list) {
        const std::vector<uint8_t> e = EncodeHandoverTokens(tokens, secret, list);
        std::vector<HandoverTokenEntry> o = { { 3, 3 } };
        Bytes32 s{};
        BanList b = bans;
        return !DecodeHandoverTokens(e.data(), e.size(), o, s, b) && o.size() == 1 && IsZero32(s) && b == bans;
    };
    BanList bad;
    bad.keys = { Bytes32{} };
    CHECK(refuses(bad)); // an all-zero key
    bad = BanList();
    bad.prefixes = { MappedV4(Ip(127, 0, 0, 1)) };
    CHECK(refuses(bad)); // IPv4 loopback
    bad.prefixes = { IpBytes{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
    CHECK(refuses(bad)); // IPv6 loopback
    bad.prefixes = { IpBytes{ 0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 9 } };
    CHECK(refuses(bad)); // an IPv6 address with its host part
    {
        // The encoder keeps the newest kBanMax of each; a count past it is refused.
        BanList many;
        for (size_t i = 0; i < kBanMax + 3; i++) {
            Bytes32 key{};
            key[0] = static_cast<uint8_t>(i + 1);
            many.keys.push_back(key);
            many.prefixes.push_back(MappedV4(Ip(10, 0, 0, static_cast<int>(i + 1))));
        }
        bytes = EncodeHandoverTokens({}, Bytes32{}, many);
        CHECK(DecodeHandoverTokens(bytes.data(), bytes.size(), tokensOut, secretOut, bansOut) &&
              bansOut.keys.size() == kBanMax && bansOut.prefixes.size() == kBanMax &&
              bansOut.keys.front()[0] == 4 && bansOut.keys.back()[0] == kBanMax + 3 &&
              bansOut.prefixes.front() == MappedV4(Ip(10, 0, 0, 4)));
        const size_t keyCountAt = 1 + 32;
        bytes[keyCountAt] = static_cast<uint8_t>(kBanMax + 1);
        CHECK(!DecodeHandoverTokens(bytes.data(), bytes.size(), tokensOut, secretOut, bansOut));
    }
    // The two shorter forms still read a table that carries bans.
    bytes = EncodeHandoverTokens(tokens, secret, bans);
    CHECK(DecodeHandoverTokens(bytes.data(), bytes.size(), tokensOut, secretOut) && tokensOut == tokens);
    CHECK(DecodeHandoverTokens(bytes.data(), bytes.size(), tokensOut) && tokensOut == tokens);
}

// Protocol 10 (decisions 62, 63, 67, 68, 73): Peers, host to one member, and Paths, member to host.
void TestPeersAndPathsCodecs() {
    std::printf("peers and paths codecs (protocol 10)\n");
    CHECK(kLobbyVersion >= 10); // protocol 10 brought them
    CHECK(kPeersPerMessage >= 1 && 1 + kPeersPerMessage * kPeerEntryMaxBytes <= gdx::net::kMaxMessagePayload);

    const auto endpoint = [](int a, int b, int c, int d, uint16_t port) {
        PeerEndpoint e;
        e.ip = MappedV4(Ip(a, b, c, d));
        e.port = port;
        return e;
    };
    Bytes32 secretA{};
    secretA.fill(0x11);
    Bytes32 secretB{};
    secretB.fill(0x22);

    PeerEntry ana; // direct on, behind the same router: public, relay and LAN
    ana.id = 1;
    ana.secret = secretA;
    ana.pub = endpoint(203, 0, 113, 7, 24302);
    ana.relay = endpoint(198, 51, 100, 9, 40001);
    ana.lan = { endpoint(192, 168, 1, 20, 24302), endpoint(10, 0, 0, 5, 24302) };
    PeerEntry bo; // direct off: the relay only
    bo.id = 3;
    bo.secret = secretB;
    bo.relay = endpoint(198, 51, 100, 10, 40002);
    PeerEntry gone; // forget member 4
    gone.id = 4;
    const std::vector<PeerEntry> peers = { ana, bo, gone };

    std::vector<uint8_t> bytes = EncodePeers(peers);
    std::vector<PeerEntry> got;
    CHECK(bytes.size() == 1 + 3 * (1 + 32 + 2 * kPeerEndpointBytes + 1) + 2 * kPeerEndpointBytes);
    CHECK(DecodePeers(bytes.data(), bytes.size(), got) && got == peers);
    CHECK(got[2].Forget() && !got[0].Forget());
    CheckEdges(bytes, [](const uint8_t* d, size_t n) {
        std::vector<PeerEntry> o;
        return DecodePeers(d, n, o);
    });

    // An IPv6 public endpoint passes as it is.
    {
        PeerEntry six = ana;
        six.pub.ip = IpBytes{ 0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 9 };
        bytes = EncodePeers({ six });
        CHECK(DecodePeers(bytes.data(), bytes.size(), got) && got.size() == 1 && got[0] == six);
    }
    // Loopback passes: copies on one machine.
    {
        PeerEntry local = bo;
        local.relay = PeerEndpoint();
        local.pub = endpoint(127, 0, 0, 1, 24303);
        bytes = EncodePeers({ local });
        CHECK(DecodePeers(bytes.data(), bytes.size(), got) && got[0] == local);
    }

    // Each refusal leaves the output alone.
    const auto refuses = [&](const std::vector<PeerEntry>& list) {
        const std::vector<uint8_t> e = EncodePeers(list);
        std::vector<PeerEntry> o = { ana };
        return !DecodePeers(e.data(), e.size(), o) && o.size() == 1 && o[0] == ana;
    };
    CHECK(refuses({})); // nothing to say
    PeerEntry bad = ana;
    bad.id = kHostId;
    CHECK(refuses({ bad })); // the host is not a peer
    bad.id = static_cast<uint8_t>(kMaxPlayers);
    CHECK(refuses({ bad })); // past the last id
    CHECK(refuses({ bo, ana })); // not ascending
    CHECK(refuses({ ana, ana })); // twice
    bad = ana;
    bad.pub = PeerEndpoint();
    bad.relay = PeerEndpoint();
    bad.lan.clear();
    CHECK(refuses({ bad })); // a secret with no way to reach it
    bad = gone;
    bad.relay = bo.relay;
    CHECK(refuses({ bad })); // a forget that names a path
    bad = ana;
    bad.pub.port = 0;
    CHECK(refuses({ bad })); // an address with no port
    bad = ana;
    bad.pub = endpoint(224, 0, 0, 1, 24302);
    CHECK(refuses({ bad })); // multicast
    bad = ana;
    bad.relay = endpoint(255, 255, 255, 255, 24302);
    CHECK(refuses({ bad })); // broadcast
    bad = ana;
    bad.lan = { endpoint(169, 254, 3, 4, 24302) };
    CHECK(refuses({ bad })); // link-local
    bad = ana;
    bad.lan = { PeerEndpoint() };
    CHECK(refuses({ bad })); // an empty LAN entry
    {
        // A LAN count past kPeerLanMax: the encoder cuts it, a forged count is refused.
        PeerEntry many = ana;
        many.lan.clear();
        for (size_t i = 0; i < kPeerLanMax + 2; i++) {
            many.lan.push_back(endpoint(192, 168, 1, static_cast<int>(i + 1), 24302));
        }
        bytes = EncodePeers({ many });
        CHECK(DecodePeers(bytes.data(), bytes.size(), got) && got[0].lan.size() == kPeerLanMax);
        const size_t lanCountAt = 1 + 1 + 32 + 2 * kPeerEndpointBytes;
        bytes[lanCountAt] = static_cast<uint8_t>(kPeerLanMax + 1);
        bytes.insert(bytes.end(), kPeerEndpointBytes, 1);
        CHECK(!DecodePeers(bytes.data(), bytes.size(), got));
    }
    {
        // The encoder writes at most kPeersPerMessage entries, and the largest of them still fit; a forged count
        // past it is refused.
        std::vector<PeerEntry> full;
        for (size_t i = 0; i < kPeersPerMessage + 2; i++) {
            PeerEntry e = ana;
            e.id = static_cast<uint8_t>(i + 1);
            e.lan.assign(kPeerLanMax, endpoint(192, 168, 1, 9, 24302));
            full.push_back(e);
        }
        bytes = EncodePeers(full);
        CHECK(bytes.size() == 1 + kPeersPerMessage * kPeerEntryMaxBytes);
        CHECK(DecodePeers(bytes.data(), bytes.size(), got) && got.size() == kPeersPerMessage);
        bytes[0] = static_cast<uint8_t>(kPeersPerMessage + 1);
        CHECK(!DecodePeers(bytes.data(), bytes.size(), got));
    }

    // Paths.
    PathsInfo paths;
    paths.relay = endpoint(198, 51, 100, 9, 40001);
    paths.lan = { endpoint(192, 168, 1, 20, 24302) };
    paths.direct = false;
    bytes = EncodePaths(paths);
    PathsInfo pathsGot;
    CHECK(bytes.size() == kPeerEndpointBytes + 1 + kPeerEndpointBytes + 1);
    CHECK(DecodePaths(bytes.data(), bytes.size(), pathsGot) && pathsGot == paths);
    CheckEdges(bytes, [](const uint8_t* d, size_t n) {
        PathsInfo o;
        return DecodePaths(d, n, o);
    });
    PathsInfo none; // no relay, no LAN, direct on
    bytes = EncodePaths(none);
    CHECK(bytes.size() == kPeerEndpointBytes + 2 && DecodePaths(bytes.data(), bytes.size(), pathsGot) &&
          pathsGot == none);
    const auto pathsRefused = [&](const std::vector<uint8_t>& e) {
        PathsInfo o = paths;
        return !DecodePaths(e.data(), e.size(), o) && o == paths;
    };
    bytes = EncodePaths(paths);
    bytes.back() = 2;
    CHECK(pathsRefused(bytes)); // direct is 0 or 1
    PathsInfo badPaths = paths;
    badPaths.relay.port = 0;
    CHECK(pathsRefused(EncodePaths(badPaths))); // a relay address with no port
    badPaths = paths;
    badPaths.lan = { endpoint(0, 1, 2, 3, 24302) };
    CHECK(pathsRefused(EncodePaths(badPaths))); // this network
    bytes = EncodePaths(paths);
    bytes[kPeerEndpointBytes] = static_cast<uint8_t>(kPeerLanMax + 1);
    CHECK(pathsRefused(bytes)); // a LAN count past the cap
}

// Decision 51: in a directory room a reachable member is elected before an earlier one that is not; in a LAN room
// join order alone decides. Reach moves it later, and a member that comes back to a promoted host says it again.
void TestReachableSuccessor() {
    std::printf("a reachable member takes over a directory room (decision 51)\n");
    for (RoomType type : { RoomType::Open, RoomType::Lan }) {
        Lan lan("Host", 83, {}, TrackStore(), type);
        LobbyClient& ana = lan.AddClient("Ana");
        lan.clientReach = true;
        LobbyClient& bo = lan.AddClient("Bo");
        lan.Settle();
        CHECK(ana.GetState() == LobbyClient::State::Joined && bo.GetState() == LobbyClient::State::Joined);
        const uint8_t expected = type == RoomType::Lan ? ana.LocalId() : bo.LocalId();
        CHECK(lan.host->Room(lan.now).successor == expected);
        CHECK(ana.Room(lan.now).successor == expected);
        if (type != RoomType::Open) {
            continue;
        }
        // Reach after the join: Bo loses its mapping, then Ana gains one.
        bo.SetReachable(false);
        lan.Settle();
        CHECK(lan.host->Room(lan.now).successor == ana.LocalId());
        bo.SetReachable(true);
        lan.Settle();
        CHECK(lan.host->Room(lan.now).successor == bo.LocalId());
        ana.SetReachable(true);
        lan.Settle();
        CHECK(lan.host->Room(lan.now).successor == ana.LocalId());
        CHECK(lan.host->Refused() == 0);
    }

    // Bo, reachable, takes over; Ana (not reachable) and Cy (reachable) come back. Bo's room then elects Cy, who
    // joined after Ana, because Cy said again that it is reachable.
    Lan lan("Host", 84, {}, TrackStore(), RoomType::Open);
    lan.AddClient("Ana");
    lan.clientReach = true;
    lan.AddClient("Bo");
    lan.clientReach = true;
    lan.AddClient("Cy");
    lan.Settle();
    CHECK(lan.host->Room(lan.now).successor == lan.clients[1]->LocalId());
    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 120);
    CHECK(lan.next != nullptr && lan.nextAt == 1);
    if (lan.next == nullptr) {
        return;
    }
    CHECK(lan.clients[0]->GetState() == LobbyClient::State::Joined &&
          lan.clients[2]->GetState() == LobbyClient::State::Joined);
    CHECK(lan.next->Room(lan.now).successor == lan.clients[2]->LocalId());
}

// Decision 52: a ban made before the host is lost still keeps the player out under the successor, by key and by
// address, and the bans the successor holds are capped.
void TestBansAfterHandover() {
    std::printf("bans go with the handover (decision 52)\n");
    const Seed cySeed = SeedOf(6);
    Lan lan;
    lan.host->SetIdentity(FriendKeyOf(SeedOf(1)));
    SignNext(lan, SeedOf(2));
    lan.AddClient("Ana");
    SignNext(lan, SeedOf(3));
    lan.AddClient("Bo");
    SignNext(lan, cySeed);
    LobbyClient& cy = lan.AddClient("Cy");
    lan.Settle();
    const IpBytes cyIp = lan.clientAddrs[2].ip;
    CHECK(lan.clients[0]->Bans().keys.empty()); // the table went out before any ban
    CHECK(lan.host->Ban(cy.LocalId(), lan.now));
    lan.Settle();
    CHECK(ClosedWith(cy, RejectReason::Banned));
    // The ban reached the successor in a new table.
    const BanList held = lan.clients[0]->Bans();
    CHECK(held == lan.host->Bans() && held.keys.size() == 1 && held.keys[0] == FriendKeyOf(cySeed));
    CHECK(held.prefixes.size() == 1 && held.prefixes[0] == cyIp);

    lan.DropHost();
    lan.TickFollow(static_cast<int>(kLinkTimeoutMs / 16) + 120);
    CHECK(lan.next != nullptr && lan.nextAt == 0);
    if (lan.next == nullptr) {
        return;
    }
    CHECK(lan.next->Bans() == held);
    CHECK(lan.clients[1]->GetState() == LobbyClient::State::Joined);
    // Cy tries the new host: with its key from another address, then with no key from its old address.
    lan.hostAddr = lan.clientAddrs[0];
    SignNext(lan, cySeed);
    LobbyClient& keyed = lan.AddClient("Cy");
    NetAddress cyAgain = lan.clientAddrs[2];
    cyAgain.port = 51000;
    lan.clientAt = cyAgain;
    LobbyClient& bare = lan.AddClient("Cyrus");
    lan.Settle();
    CHECK(ClosedWith(keyed, RejectReason::Banned));
    CHECK(ClosedWith(bare, RejectReason::Banned));
    // Someone new still gets in.
    LobbyClient& dee = lan.AddClient("Dee");
    lan.Settle();
    CHECK(dee.GetState() == LobbyClient::State::Joined);

    // SetBans keeps the newest kBanMax of each.
    BanList many;
    for (size_t i = 0; i < kBanMax + 5; i++) {
        Bytes32 key{};
        key[0] = static_cast<uint8_t>(i + 1);
        many.keys.push_back(key);
        many.prefixes.push_back(MappedV4(Ip(10, 0, 1, static_cast<int>(i + 1))));
    }
    lan.next->SetBans(many);
    CHECK(lan.next->Banned().size() == kBanMax && lan.next->Banned().front()[0] == 6);
    CHECK(lan.next->Bans().prefixes.size() == kBanMax && lan.next->Bans().prefixes.back() == many.prefixes.back());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash keeps the lines before it
    TestJoinCodec();
    TestRosterCodec();
    TestRoomCodec();
    TestHandoverCodec();
    TestResultsCodec();
    TestRaceCodec();
    TestStateCodec();
    TestClockSync();
    TestDiscoveryCodec();
    TestSanitizeAndFilter();
    TestCustomTrackWire();
    TestMachineRecordBounds();
    TestDecoderSweep();
    TestHackIdentity();
    TestJoinReadyEmotes();
    TestEmoteRateLimit();
    TestRejects();
    TestLeaveVanishAndClose();
    TestNoHost();
    TestHostIgnoresStrangers();
    TestHelloGate();
    TestProbeNotAmplified();
    TestUploadTotals();
    TestRoomTypeWire();
    TestTakeoverSecret();
    TestOldVersionRefused();
    TestHostChecksEveryMessage();
    TestKeyCodecs();
    TestKeysInARoom();
    TestFriendsRoom();
    TestExpectedHostKey();
    TestKeysAfterHandover();
    TestReachAndBanCodecs();
    TestPeersAndPathsCodecs();
    TestReachableSuccessor();
    TestBansAfterHandover();
    TestVoteFlow();
    TestPickFlow();
    TestWatcherRacesNext();
    TestRaceStart();
    TestRaceResults();
    TestRacePoses();
    TestHandoverTokens();
    TestHostLostIn(Phase::Lobby);
    TestHostLostIn(Phase::Machine);
    TestHostLostIn(Phase::Countdown);
    TestHostLostIn(Phase::Racing);
    TestHostLostIn(Phase::Results);
    TestHostLostAfterAFinish();
    TestHostLostInVote();
    TestHostLostInAssets();
    TestRejoinRefusals();
    TestSuccessorAlone();
    TestHandoverDialable();
    TestNothingToFollow();
    TestHostLeavesMidRace();
    TestHostLeavesBetweenRaces();
    TestDirectLinksRace();
    TestDirectLinksBlocked();
    TestDirectLinksLan();
    TestDirectLinksRefusals();
    TestBrowser();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
