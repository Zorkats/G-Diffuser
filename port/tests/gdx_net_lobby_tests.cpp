// port/tests/gdx_net_lobby_tests.cpp: the LAN room wire format (version 2), the chat filter, and the host,
// client and room browser sessions (port/net/gdx_net_lobby*.h).
//
// The session tests run every peer in this process on a fake network and a fake clock: datagrams go
// into a queue and arrive on the next Step, a broadcast reaches every endpoint on its port, and a test can
// cut an endpoint off to play a peer that vanished. No socket is opened. The room's own rules are tested
// without a network in gdx_net_room_tests.cpp; here they are followed from the host to the clients.

#include "../net/gdx_net_channel.h"
#include "../net/gdx_net_lobby.h"
#include "../net/gdx_net_lobby_session.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
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
const char* const kTooFast = "You are sending messages too fast. Wait a moment.";

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
            if (!Alive(from) || (filter_ && filter_(from, to, len))) {
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

    // Asked about every datagram as it is sent: a test counts with it, or loses the datagram by saying true.
    using Filter = std::function<bool(const NetAddress& from, const NetAddress& to, size_t len)>;

    void SetFilter(Filter filter) {
        filter_ = std::move(filter);
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
    Filter filter_;
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

// One host and any number of clients on the fake network, ticked together every 16 ms.
struct Lan {
    FakeNet net;
    uint32_t now = 1000;
    NetAddress hostAddr = Addr(Ip(192, 168, 1, 10), kDefaultPort);
    std::unique_ptr<LobbyHost> host;
    std::vector<std::unique_ptr<LobbyClient>> clients;
    std::vector<NetAddress> clientAddrs;
    // Each client's clock is the host's plus its skew: two computers never share one.
    std::vector<uint32_t> skew;
    uint32_t nextSkew = 0; // the skew of the next client added
    std::vector<std::unique_ptr<LanBrowser>> browsers;

    explicit Lan(const std::string& hostName = "Host", uint32_t roomId = 77) {
        host = std::make_unique<LobbyHost>(kBuild, Profile(hostName, 0xFF0000, 0), kDefaultPort, ConfigFor(roomId),
                                           net.SenderFor(hostAddr));
        net.Add(hostAddr,
                [this](const NetAddress& from, const uint8_t* d, size_t n) { host->Receive(from, d, n, now); });
    }

    LobbyClient& AddClient(const std::string& name, Seat seat = Seat::Race, const std::string& build = kBuild,
                           uint32_t colour = 0x00FF00) {
        const size_t index = clients.size();
        const NetAddress addr =
            Addr(Ip(192, 168, 1, static_cast<int>(20 + index)), static_cast<uint16_t>(50000 + index));
        clients.push_back(std::make_unique<LobbyClient>(build, Profile(name, colour, 3), seat, hostAddr,
                                                        0x1000u + static_cast<uint32_t>(index), net.SenderFor(addr)));
        clientAddrs.push_back(addr);
        skew.push_back(nextSkew);
        net.Add(addr, [this, index](const NetAddress& from, const uint8_t* d, size_t n) {
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
            host->Update(now);
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
    for (uint8_t value = 1; value <= 6; value++) {
        b = EncodeReject(static_cast<RejectReason>(value));
        CHECK(DecodeReject(b.data(), b.size(), reason) && static_cast<uint8_t>(reason) == value);
        CHECK(RejectReasonText(reason)[0] != '\0');
    }
    const uint8_t zero = 0;
    const uint8_t seven = 7;
    CHECK(!DecodeReject(&zero, 1, reason) && !DecodeReject(&seven, 1, reason));

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

// The bytes of a grid message: 18 of header, then three per human.
constexpr size_t kGridLaps = 6;
constexpr size_t kGridFlags = 7;
constexpr size_t kGridRange = 8;
constexpr size_t kGridCpus = 16;
constexpr size_t kGridCount = 17;
constexpr size_t kGridHumans = 18;

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
    CHECK(g.size() == kGridHumans + 3 * grid.humans.size());
    CHECK(g[kGridLaps] == 5 && g[kGridCpus] == 27 && g[kGridCount] == 3 && g[kGridHumans] == 3);
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
    CHECK(refuses(kGridHumans + 3, 3));                              // the same player twice
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
    // The CPU machines are in the race or out of it: the last byte is 0 or 1 and nothing else.
    {
        RoomState plain = state;
        plain.cpusOut = false;
        CHECK(!(plain == state));
        std::vector<uint8_t> bytes = EncodeRoomState(plain);
        CHECK(bytes.size() == stateBytes.size() && bytes.back() == 0 && stateBytes.back() == 1);
        CHECK(DecodeRoomState(bytes.data(), bytes.size(), stateOut) && stateOut == plain && !stateOut.cpusOut);
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
    badState.racerCap = 7;
    CHECK(refusesState(badState));
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
    bad.racerCap = 7;
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
    int accepted = 0;
    for (int round = 0; round < 30000; round++) {
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
        const uint8_t* d = bytes.data();
        const size_t n = bytes.size();
        JoinRequest j;
        if (DecodeJoin(d, n, j)) {
            accepted++;
            CHECK(NameOk(j.name) && MachineOk(j.machine) && ColourOk(j.colour));
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
            CHECK(EncodeRoomState(s) == bytes);
        }
        std::vector<Ballot> v;
        if (DecodeVotes(d, n, v)) {
            accepted++;
            CHECK(v.size() <= kMaxRacers && EncodeVotes(v) == bytes);
        }
        LotteryResult l;
        if (DecodeLottery(d, n, l)) {
            accepted++;
            CHECK(l.track < kStockTracks && (l.ballots.empty() ? l.winner == kNoWinner : l.winner < l.ballots.size()));
            CHECK(EncodeLottery(l) == bytes);
        }
        Picked p;
        if (DecodePicked(d, n, p)) {
            accepted++;
            CHECK(p.track < kStockTracks && EncodePicked(p) == bytes);
        }
        LockRequest k;
        if (DecodeLock(d, n, k)) {
            accepted++;
            CHECK(k.machine < kRosterMachines && k.skin < kMachineSkins && k.engine <= kEngineMax);
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
    LobbyClient& old = lan.AddClient("Old", Seat::Race, "1.0.9/zzz");
    lan.Settle();
    CHECK(old.GetState() == LobbyClient::State::Closed);
    CHECK(old.CloseText() == RejectReasonText(RejectReason::Build));
    CHECK(lan.host->Roster().size() == 1);
    lan.Tick(40);
    CHECK(lan.host->PeerCount() == 0);

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

void TestHostIgnoresStrangers() {
    std::printf("host ignores strangers\n");
    Lan lan;
    const uint8_t junk[] = { 1, 2, 3, 4, 5 };
    lan.host->Receive(Addr(Ip(10, 1, 1, 1), 1), junk, sizeof(junk), lan.now);
    CHECK(lan.host->PeerCount() == 0 && lan.host->Refused() == 1);
    // A flood of hellos from many addresses holds at most kPendingPeerCap channels, and they expire.
    const uint8_t hello[] = { 'G', 'D', 0, 0x10, 0, 0, 0, static_cast<uint8_t>(kProtocolVersion), 0 };
    for (int i = 0; i < 40; i++) {
        lan.host->Receive(Addr(Ip(10, 2, 0, i), 9999), hello, sizeof(hello), lan.now);
    }
    CHECK(lan.host->PeerCount() == kPendingPeerCap);
    lan.Tick(400);
    CHECK(lan.host->PeerCount() == 0);
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
    CHECK(!raw.got.empty() && raw.got.front().type == kMsgWelcome); // the id comes before the roster
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

    // Every racer ready: the series starts. Three racers and nobody has finished last yet, so the room draws.
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

    // The lock-in screen opens: he takes a race seat and is on the grid.
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
    lan.host->SendPoses(0, field);
    run(3);
    lan.net.SetFilter(nullptr);
    CHECK(clientGot(bo) == std::vector<StatePose>(field.begin() + kStatePoses, field.end()));
    CHECK(clientGot(wim) == field);

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
    // The list follows the room: the next probe's answer says a race is being set up.
    CHECK(lan.host->Start(lan.now));
    lan.Tick(100);
    CHECK(browser.Rooms().size() == 1 && browser.Rooms()[0].room.phase == Phase::Machine);
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

int main() {
    TestJoinCodec();
    TestRosterCodec();
    TestRoomCodec();
    TestResultsCodec();
    TestRaceCodec();
    TestStateCodec();
    TestClockSync();
    TestDiscoveryCodec();
    TestSanitizeAndFilter();
    TestDecoderSweep();
    TestJoinReadyEmotes();
    TestEmoteRateLimit();
    TestRejects();
    TestLeaveVanishAndClose();
    TestNoHost();
    TestHostIgnoresStrangers();
    TestOldVersionRefused();
    TestHostChecksEveryMessage();
    TestVoteFlow();
    TestPickFlow();
    TestWatcherRacesNext();
    TestRaceStart();
    TestRaceResults();
    TestRacePoses();
    TestBrowser();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
