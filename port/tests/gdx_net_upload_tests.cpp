// port/tests/gdx_net_upload_tests.cpp: what the host uploads in a race and in a track burst (G-NET L2).
// Copyright (c) Zorkats
//
// Runs the real LobbyHost and LobbyClient, and the real transfer Relay over Channel pairs, on a fake network and a
// fake 10 ms clock. Each datagram the host hands to the network is counted as its length plus the IPv4 header
// estimate of decision 46, and the counts are kept per guest link. Nothing opens a socket or runs the game, so the
// numbers are what the protocol sends, not what a PC sends. The four-copy run that calibrates them is not in here.
//
// "Humans" are the racers, the host included. Two watchers join after them, so a race with H humans has H - 1 racer
// links and 2 watcher links. The 12-human race therefore has the 13 guest links that the transfer burst uses.
//
// The race is reached the way the room reaches it: a vote, the lottery (skipped), the lock-in, the grid, every
// racer's track check, and the countdown. Each human copy sends its own machine. The host sends its machine and the
// CPUs. The 128 KB burst sends synthetic bytes of the track's exact size. The transfer layer never reads them, and the
// receiver's whole-track check runs only after the last chunk and fails them. That check is not in the byte count.

#include "gdx_net_channel.h"
#include "gdx_net_lobby.h"
#include "gdx_net_lobby_session.h"
#include "gdx_net_track_pack.h"
#include "gdx_net_transfer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace gdx::net;
using namespace gdx::net::lobby;
namespace xfer = gdx::net::transfer;

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

// xorshift32, for the loss rolls and the burst's bytes. Every scenario reseeds it, so a run repeats exactly.
uint32_t gRng = 1;

uint32_t NextRandom() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}

const std::string kBuild = "1.1.0/upload";
constexpr uint32_t kTickMs = 10;          // the owner's tick, as the pose and clock notes in gdx_net_lobby_session.h assume
constexpr uint32_t kGameHz = 60;          // the game's tick: a pose leaves on every second one
constexpr uint32_t kPoseHz = kGameHz / 2; // 30 Hz for each machine
constexpr uint32_t kWindowMs = 60000;     // the race time that is measured
constexpr uint32_t kWindowSeconds = kWindowMs / 1000;
constexpr uint32_t kHeaderBytes = kUdpIpv4HeaderBytes;
constexpr size_t kWatcherSeats = 2;
constexpr size_t kBurstSeats = 13;
constexpr uint32_t kBurstLimitMs = 300000; // a burst that has not finished in five minutes has failed
constexpr size_t kBurstBytes = 128u * 1024u; // the track size the burst streams: the snapshot format's own cap

NetAddress Addr(uint32_t ip, uint16_t port) {
    return NetAddress::FromV4(ip, port);
}

uint32_t Ip(int a, int b, int c, int d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

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

LockRequest LockOf(uint8_t machine, uint8_t skin, uint8_t engine) {
    LockRequest lock;
    lock.machine = machine;
    lock.skin = skin;
    lock.engine = engine;
    return lock;
}

// A machine somewhere on a track. Every field moves with the tick, so no two datagrams are the same bytes.
StatePose PoseFor(uint8_t slot, uint32_t tick, uint32_t hostMs) {
    StatePose pose;
    pose.slot = slot;
    pose.tick = tick;
    pose.timeMs = hostMs;
    pose.pos = { 100.0f * static_cast<float>(slot), -20.5f, 0.6f * static_cast<float>(tick) };
    pose.vel = { 0.25f * static_cast<float>(slot), -0.5f, 0.6f };
    pose.forward = { 0.0f, 0.0f, 1.0f };
    pose.up = { 0.0f, 1.0f, 0.0f };
    pose.lap = 1;
    pose.flags = 0x80000000u | tick;
    pose.attack = static_cast<uint8_t>(slot % 3);
    return pose;
}

// The race slot of a human is its place among the humans of the grid, the same order the room uses.
int SlotOf(const Grid& grid, uint8_t id) {
    for (size_t i = 0; i < grid.humans.size(); i++) {
        if (grid.humans[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The fake network. A datagram sent now arrives on the next Step unless the loss roll drops it. Every endpoint is
// one address, and every datagram in this test is unicast.
class FakeNetwork {
public:
    using Deliver = std::function<void(const NetAddress& from, const uint8_t* data, size_t len)>;

    void Add(const NetAddress& addr, Deliver deliver) {
        ends_.push_back({ addr, std::move(deliver) });
    }

    void SetLoss(uint32_t percent) {
        lossPercent_ = percent;
    }

    DatagramSend SenderFor(const NetAddress& from) {
        return [this, from](const NetAddress& to, const uint8_t* data, size_t len) {
            if (lossPercent_ > 0 && NextRandom() % 100 < lossPercent_) {
                return;
            }
            queue_.push_back({ from, to, std::vector<uint8_t>(data, data + len) });
        };
    }

    void Step() {
        std::vector<Datagram> now;
        now.swap(queue_);
        for (const Datagram& d : now) {
            for (const Endpoint& e : ends_) {
                if (e.addr == d.to) {
                    e.deliver(d.from, d.bytes.data(), d.bytes.size());
                    break;
                }
            }
        }
    }

private:
    struct Endpoint {
        NetAddress addr;
        Deliver deliver;
    };

    struct Datagram {
        NetAddress from;
        NetAddress to;
        std::vector<uint8_t> bytes;
    };

    std::vector<Endpoint> ends_;
    std::vector<Datagram> queue_;
    uint32_t lossPercent_ = 0;
};

// What the host sends one guest in the race window.
struct GuestLink {
    NetAddress addr;
    bool watcher = false;
    uint64_t bytes = 0;     // datagram length plus the header estimate
    uint64_t datagrams = 0;
    uint64_t poses = 0;     // poses this guest took in from the room
};

struct RaceRow {
    uint32_t humans = 0;
    uint32_t loss = 0;
    size_t racers = 0;   // guest racer links
    size_t watchers = 0; // guest watcher links
    double hostKbps = 0;
    double racerKbps = 0;   // mean over the racer links
    double watcherKbps = 0; // mean over the watcher links
    double datagramsPerSecond = 0;
};

// One lobby on the fake network, at one loss rate: a host, the racers and the watchers.
class LobbyRig {
public:
    explicit LobbyRig(uint32_t lossPercent) : loss_(lossPercent) {
        wire_.SetLoss(lossPercent);
        const DatagramSend hostLink = wire_.SenderFor(hostAddr_);
        host_ = std::make_unique<LobbyHost>(
            kBuild, Profile("Host", 0xFF0000, kNoMachine), kDefaultPort, ConfigFor(77),
            [this, hostLink](const NetAddress& to, const uint8_t* data, size_t len) {
                CountHostSend(to, len);
                hostLink(to, data, len);
            });
        wire_.Add(hostAddr_, [this](const NetAddress& from, const uint8_t* data, size_t len) {
            host_->Receive(from, data, len, now_);
        });
    }

    // Runs one race with humans racers (the host included) and the watchers, and returns what the host uploaded.
    RaceRow Scenario(uint32_t humans, uint64_t poseBytes) {
        if (humans == 12) {
            // The room's default cap is 8. The brief's 12-human race needs all 12 seats.
            CHECK(host_->SetRacerCap(12, now_));
        }
        for (uint32_t i = 1; i < humans; i++) {
            AddGuest("Racer " + std::to_string(i), Seat::Race);
        }
        CHECK(RunUntil([this] { return AllJoined(); }, 15000));
        // The watchers join last, as they do in a room that is already full of racers.
        for (size_t w = 0; w < kWatcherSeats; w++) {
            AddGuest("Watcher " + std::to_string(w + 1), Seat::Watch);
        }
        CHECK(RunUntil([this] { return AllJoined(); }, 15000));
        CHECK(host_->Roster().size() == humans + kWatcherSeats);
        CHECK(host_->Start(now_));
        const bool reached = ReachRace();
        CHECK(reached);
        if (!reached) {
            return Row(humans);
        }
        const Grid& grid = host_->RaceGrid();
        CHECK(grid.humans.size() == humans && static_cast<size_t>(grid.cpus) == kGridSlots - humans);

        Race();

        const uint64_t emits = emits_;
        CHECK(emits == static_cast<uint64_t>(kWindowMs) * kPoseHz / 1000);
        CHECK(HostPhase() == Phase::Racing);
        CHECK(host_->Refused() == 0 && strays_ == 0);
        bool guestRefused = false;
        for (const auto& guest : guests_) {
            guestRefused = guestRefused || guest->Refused() != 0;
        }
        CHECK(!guestRefused);

        // Floors and ceilings on what each guest link carries. The floor is the pose payload the link is owed: every
        // other machine, at 30 Hz, in 64-byte poses. The ceiling is twice that, which a duplicating relay would break.
        bool racerAtLeast = true;
        bool racerAtMost = true;
        bool watcherAtLeast = true;
        bool watcherAtMost = true;
        bool delivered = true;
        for (const GuestLink& g : links_) {
            const uint64_t owed = static_cast<uint64_t>(g.watcher ? kGridSlots : kGridSlots - 1) * kPoseHz * poseBytes;
            const uint64_t floor = owed * kWindowSeconds;
            const bool low = g.bytes < floor;
            const bool high = g.bytes > 2 * floor;
            if (g.watcher) {
                watcherAtLeast = watcherAtLeast && !low;
                watcherAtMost = watcherAtMost && !high;
            } else {
                racerAtLeast = racerAtLeast && !low;
                racerAtMost = racerAtMost && !high;
            }
            const uint64_t expected = static_cast<uint64_t>(g.watcher ? kGridSlots : kGridSlots - 1) * emits;
            delivered = delivered && g.poses * 100 >= expected * 95;
        }
        CHECK(racerAtLeast);
        CHECK(racerAtMost);
        CHECK(watcherAtLeast);
        CHECK(watcherAtMost);
        CHECK(delivered);
        CHECK(hostPoses_ * 100 >= static_cast<uint64_t>(humans - 1) * emits * 95);
        return Row(humans);
    }

private:
    void AddGuest(const std::string& name, Seat seat) {
        const size_t index = guests_.size();
        GuestLink link;
        link.addr = Addr(Ip(192, 168, 1, static_cast<int>(20 + index)), static_cast<uint16_t>(50000 + index));
        link.watcher = seat == Seat::Watch;
        guests_.push_back(std::make_unique<LobbyClient>(kBuild, Profile(name, 0x00FF00, kNoMachine), seat, hostAddr_,
                                                        0x1000u + static_cast<uint32_t>(index),
                                                        wire_.SenderFor(link.addr)));
        wire_.Add(link.addr, [this, index](const NetAddress& from, const uint8_t* data, size_t len) {
            guests_[index]->Receive(from, data, len, now_);
        });
        links_.push_back(link);
        guests_.back()->Start(now_);
    }

    void Tick() {
        now_ += kTickMs;
        wire_.Step();
        host_->Update(now_);
        for (auto& guest : guests_) {
            guest->Update(now_);
        }
    }

    template <typename Done>
    bool RunUntil(Done done, uint32_t limitMs) {
        const uint32_t end = now_ + limitMs;
        while (!done() && now_ < end) {
            Tick();
        }
        return done();
    }

    bool AllJoined() const {
        if (guests_.empty()) {
            return false;
        }
        for (const auto& guest : guests_) {
            if (guest->GetState() != LobbyClient::State::Joined) {
                return false;
            }
        }
        return true;
    }

    Phase HostPhase() const {
        return host_->Room(now_).phase;
    }

    // True when the host and every joined guest are in the phase.
    bool AllIn(Phase phase) const {
        if (HostPhase() != phase) {
            return false;
        }
        for (const auto& guest : guests_) {
            if (guest->GetState() == LobbyClient::State::Joined && guest->Room(now_).phase != phase) {
                return false;
            }
        }
        return true;
    }

    // Runs the room from the lobby to the countdown. Racers act once every copy is in the phase, as a copy does, so
    // a request is never sent for a phase the room has left.
    bool ReachRace() {
        bool voted = false;
        bool racersLocked = false;
        bool hostLocked = false;
        bool loaded = false;
        const uint32_t limit = now_ + 60000;
        while (HostPhase() != Phase::Racing && now_ < limit) {
            const Phase phase = HostPhase();
            if (phase == Phase::Voting && !voted && AllIn(phase)) {
                voted = true;
                for (size_t i = 0; i < guests_.size(); i++) {
                    if (!links_[i].watcher) {
                        guests_[i]->Vote(static_cast<uint8_t>(0));
                    }
                }
                CHECK(host_->Vote(static_cast<uint8_t>(0), now_));
            }
            if (phase == Phase::Lottery) {
                host_->SkipLottery(now_);
            }
            if (phase == Phase::Machine && AllIn(phase)) {
                // The host locks last, one tick after the racers, so the room sees every lock before the host's own.
                if (!racersLocked) {
                    racersLocked = true;
                    // Machines are distinct: the guest racers take 0 to 10, the host takes the last one.
                    uint8_t machine = 0;
                    for (size_t i = 0; i < guests_.size(); i++) {
                        if (!links_[i].watcher) {
                            guests_[i]->Lock(LockOf(machine, 0, 50));
                            machine++;
                        }
                    }
                } else if (!hostLocked) {
                    hostLocked = true;
                    CHECK(host_->Lock(LockOf(static_cast<uint8_t>(kMaxRacers - 1), 0, 50), now_));
                }
            }
            if (phase == Phase::GridSync && !loaded && AllIn(phase)) {
                loaded = true;
                ReadyToRace ready;
                ready.raceIndex = host_->RaceGrid().raceIndex;
                ready.loadMs = 500;
                CHECK(host_->GridReady(ready, now_));
                for (size_t i = 0; i < guests_.size(); i++) {
                    if (!links_[i].watcher) {
                        guests_[i]->GridReady(ready);
                    }
                }
            }
            Tick();
        }
        return HostPhase() == Phase::Racing;
    }

    // The measured window: kWindowMs of race time. The game ticks at 60 Hz and a machine sends on every second tick,
    // so each copy sends 30 poses a second, the rate the pose budget in gdx_net_lobby_session.h is sized for.
    void Race() {
        const uint32_t start = now_;
        const uint16_t raceIndex = host_->RaceGrid().raceIndex;
        measuring_ = true;
        int64_t sent = -1;
        while (now_ - start < kWindowMs) {
            const int64_t game = static_cast<int64_t>(static_cast<uint64_t>(now_ - start) * kGameHz / 1000);
            for (int64_t tick = sent + 1; tick <= game; tick++) {
                if (tick % 2 == 0) {
                    SendPoses(static_cast<uint32_t>(tick), raceIndex);
                    emits_++;
                }
            }
            sent = game;
            Tick();
            Drain();
        }
        measuring_ = false;
    }

    // Each human copy sends its own machine. The host copy sends its own machine and the CPUs.
    void SendPoses(uint32_t tick, uint16_t raceIndex) {
        for (size_t i = 0; i < guests_.size(); i++) {
            if (links_[i].watcher) {
                continue;
            }
            Grid grid;
            if (!guests_[i]->RaceGrid(grid)) {
                continue;
            }
            const int slot = SlotOf(grid, guests_[i]->LocalId());
            if (slot >= 0) {
                guests_[i]->SendPoses(raceIndex, { PoseFor(static_cast<uint8_t>(slot), tick, now_) });
            }
        }
        const Grid& grid = host_->RaceGrid();
        std::vector<StatePose> mine;
        const int own = SlotOf(grid, kHostId);
        if (own >= 0) {
            mine.push_back(PoseFor(static_cast<uint8_t>(own), tick, now_));
        }
        for (size_t slot = grid.humans.size(); slot < grid.humans.size() + grid.cpus; slot++) {
            mine.push_back(PoseFor(static_cast<uint8_t>(slot), tick, now_));
        }
        host_->SendPoses(raceIndex, mine);
    }

    // Every copy takes what arrived, as the game would each frame. A copy that did not would fill its box and drop.
    void Drain() {
        uint16_t raceIndex = 0;
        std::vector<StatePose> got;
        host_->TakePoses(raceIndex, got);
        if (measuring_) {
            hostPoses_ += got.size();
        }
        for (size_t i = 0; i < guests_.size(); i++) {
            got.clear();
            guests_[i]->TakePoses(raceIndex, got);
            if (measuring_) {
                links_[i].poses += got.size();
            }
        }
    }

    void CountHostSend(const NetAddress& to, size_t len) {
        if (!measuring_) {
            return;
        }
        for (GuestLink& link : links_) {
            if (link.addr == to) {
                link.bytes += len + kHeaderBytes;
                link.datagrams++;
                return;
            }
        }
        strays_++;
    }

    RaceRow Row(uint32_t humans) const {
        RaceRow row;
        row.humans = humans;
        row.loss = loss_;
        double all = 0;
        double racerBytes = 0;
        double watcherBytes = 0;
        double datagrams = 0;
        for (const GuestLink& g : links_) {
            all += static_cast<double>(g.bytes);
            datagrams += static_cast<double>(g.datagrams);
            if (g.watcher) {
                watcherBytes += static_cast<double>(g.bytes);
                row.watchers++;
            } else {
                racerBytes += static_cast<double>(g.bytes);
                row.racers++;
            }
        }
        const double seconds = static_cast<double>(kWindowSeconds);
        const auto kbit = [seconds](double bytes) { return bytes * 8.0 / 1000.0 / seconds; };
        row.hostKbps = kbit(all);
        row.racerKbps = row.racers > 0 ? kbit(racerBytes / static_cast<double>(row.racers)) : 0.0;
        row.watcherKbps = row.watchers > 0 ? kbit(watcherBytes / static_cast<double>(row.watchers)) : 0.0;
        row.datagramsPerSecond = datagrams / seconds;
        return row;
    }

private:
    uint32_t loss_ = 0;
    FakeNetwork wire_;
    uint32_t now_ = 1000;
    NetAddress hostAddr_ = Addr(Ip(192, 168, 1, 10), kDefaultPort);
    std::unique_ptr<LobbyHost> host_;
    std::vector<std::unique_ptr<LobbyClient>> guests_;
    std::vector<GuestLink> links_; // in the same order as guests_
    bool measuring_ = false;
    uint64_t emits_ = 0;     // game ticks whose poses went out in the window
    uint64_t hostPoses_ = 0; // poses the host took in from the racers in the window
    uint64_t strays_ = 0;    // host datagrams to an address that is no guest here
};

// One guest's end of a burst link: the host's Channel and the guest's Channel, the datagrams in flight each way, and
// the guest's receiver.
struct BurstPipe {
    std::unique_ptr<Channel> server;
    std::unique_ptr<Channel> client;
    std::deque<std::vector<uint8_t>> toClient;
    std::deque<std::vector<uint8_t>> toServer;
    std::unique_ptr<xfer::Receiver> receiver;
    bool offered = false;
    bool done = false;
    uint32_t accepted = 0;
};

// The loss roll keeps or drops a datagram. The sender has counted it already, as the host's wire does.
void Carry(std::deque<std::vector<uint8_t>>& queue, const uint8_t* data, size_t len, uint32_t lossPercent) {
    if (lossPercent > 0 && NextRandom() % 100 < lossPercent) {
        return;
    }
    queue.emplace_back(data, data + len);
}

struct Burst {
    uint32_t loss = 0;         // percent of datagrams the fake link drops
    uint64_t hostBytes = 0;    // the host's wire: datagram length plus the header estimate, lost or not
    uint64_t channelBytes = 0; // the same total, as the host's Channels count it
    size_t seatsDone = 0;      // seats that hold every chunk
    uint32_t refused = 0;      // messages that Admit or a decoder turned away
    uint32_t ms = 0;           // from the first tick to the last seat
    bool timedOut = false;
};

// Streams one track to kBurstSeats guests over Channel pairs, the way the host's relay does, and counts the bytes.
Burst RunBurst(const std::vector<uint8_t>& bytes, const track::TrackId& id, uint32_t lossPercent) {
    Burst out;
    out.loss = lossPercent;
    gRng = 0xB0C70000u + lossPercent;
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    const size_t chunks = xfer::ChunkCount(size);

    xfer::Offer offer;
    offer.id = id;
    offer.size = size;
    offer.title = "Upload test";
    std::vector<uint8_t> offerBytes;
    CHECK(xfer::EncodeOffer(offer, offerBytes));

    // The host holds every chunk of the track, so its relay streams from a complete buffer.
    std::shared_ptr<xfer::TrackBuffer> held = xfer::TrackBuffer::Empty(id, size);
    bool stored = held != nullptr;
    for (size_t i = 0; stored && i < chunks; i++) {
        stored = held->Put(i, bytes.data() + i * xfer::kChunkData, xfer::ChunkLength(size, i));
    }
    CHECK(stored);
    if (!stored) {
        return out;
    }
    xfer::Relay relay(held);

    uint64_t hostWire = 0;
    std::vector<std::unique_ptr<BurstPipe>> pipes;
    for (size_t g = 0; g < kBurstSeats; g++) {
        auto pipe = std::make_unique<BurstPipe>();
        BurstPipe* p = pipe.get();
        ChannelConfig serverConfig;
        serverConfig.role = Role::Server;
        serverConfig.timeoutMs = kLinkTimeoutMs;
        p->server = std::make_unique<Channel>(serverConfig, [p, &hostWire, lossPercent](const uint8_t* data, size_t len) {
            hostWire += len + kHeaderBytes;
            Carry(p->toClient, data, len, lossPercent);
        });
        ChannelConfig clientConfig;
        clientConfig.role = Role::Client;
        clientConfig.connectionNonce = 0x3000u + static_cast<uint32_t>(g);
        clientConfig.timeoutMs = kLinkTimeoutMs;
        p->client = std::make_unique<Channel>(clientConfig, [p, lossPercent](const uint8_t* data, size_t len) {
            Carry(p->toServer, data, len, lossPercent);
        });
        p->client->Connect(1000);
        p->receiver = std::make_unique<xfer::Receiver>(id);
        pipes.push_back(std::move(pipe));
    }

    xfer::Counters admit;
    uint32_t now = 1000;
    const uint32_t start = now;
    const uint32_t limit = now + kBurstLimitMs;
    size_t finished = 0;
    while (finished < kBurstSeats && now < limit) {
        now += kTickMs;
        for (auto& pipe : pipes) {
            BurstPipe& p = *pipe;
            while (!p.toClient.empty()) {
                std::vector<uint8_t> d = std::move(p.toClient.front());
                p.toClient.pop_front();
                p.client->Receive(d.data(), d.size(), now);
            }
            while (!p.toServer.empty()) {
                std::vector<uint8_t> d = std::move(p.toServer.front());
                p.toServer.pop_front();
                p.server->Receive(d.data(), d.size(), now);
            }
            p.server->Update(now);
            p.client->Update(now);
        }

        // The host: the offer once the link is up, then the guests' requests go to the relay.
        for (size_t g = 0; g < pipes.size(); g++) {
            BurstPipe& p = *pipes[g];
            if (!p.offered && p.server->State() == LinkState::Connected) {
                p.offered = p.server->SendReliable(kMsgTrackOffer, offerBytes.data(), offerBytes.size()) == QueueResult::Ok;
            }
            Message msg;
            while (p.server->PopMessage(msg)) {
                xfer::Route route;
                route.localIsHost = true;
                route.from = xfer::Peer::Guest;
                route.transferOpen = true;
                track::TrackId asked{};
                if (msg.type == kMsgTrackNeed && xfer::Admit(xfer::Kind::Need, route, admit) &&
                    xfer::DecodeNeed(msg.payload.data(), msg.payload.size(), asked) && asked == id) {
                    relay.Need(static_cast<uint8_t>(g));
                } else {
                    out.refused++;
                }
            }
        }
        for (size_t g = 0; g < pipes.size(); g++) {
            Channel* link = pipes[g]->server.get();
            relay.Pump(static_cast<uint8_t>(g), now, link->ReliableOutstanding(),
                       [link](const std::vector<uint8_t>& payload) {
                           return link->SendReliable(kMsgTrackChunk, payload.data(), payload.size()) == QueueResult::Ok;
                       });
        }

        // The guests: the offer is admitted and asks for the track, then each chunk is admitted and stored.
        for (auto& pipe : pipes) {
            BurstPipe& p = *pipe;
            Message msg;
            while (p.client->PopMessage(msg)) {
                xfer::Route route;
                route.from = xfer::Peer::Host;
                route.transferOpen = true;
                if (msg.type == kMsgTrackOffer) {
                    xfer::Offer got;
                    if (!xfer::Admit(xfer::Kind::Offer, route, admit) ||
                        !xfer::DecodeOffer(msg.payload.data(), msg.payload.size(), got) || !p.receiver->OnOffer(got, now)) {
                        out.refused++;
                        continue;
                    }
                    const std::vector<uint8_t> need = xfer::EncodeNeed(id);
                    if (p.client->SendReliable(kMsgTrackNeed, need.data(), need.size()) != QueueResult::Ok) {
                        out.refused++;
                    }
                } else if (msg.type == kMsgTrackChunk) {
                    xfer::Chunk chunk;
                    if (!xfer::Admit(xfer::Kind::Chunk, route, admit) ||
                        !xfer::DecodeChunk(msg.payload.data(), msg.payload.size(), chunk)) {
                        out.refused++;
                        continue;
                    }
                    if (p.receiver->OnChunk(chunk, now)) {
                        p.accepted++;
                    }
                } else {
                    out.refused++;
                }
            }
            if (!p.done && p.accepted == chunks) {
                p.done = true;
                finished++;
                out.ms = now - start;
            }
        }
    }

    out.timedOut = finished < kBurstSeats;
    out.seatsDone = finished;
    out.hostBytes = hostWire;
    for (auto& pipe : pipes) {
        out.channelBytes += pipe->server->Stats().bytesSent;
    }
    return out;
}

}  // namespace

int main() {
    // The floors assume a pose is 64 bytes on the wire. The difference of a two-pose and a one-pose state is one pose.
    const size_t oneBytes = EncodeState(RaceState{ 0, { PoseFor(0, 0, 0) } }).size();
    const size_t twoBytes = EncodeState(RaceState{ 0, { PoseFor(0, 0, 0), PoseFor(1, 0, 0) } }).size();
    const uint64_t poseBytes = twoBytes - oneBytes;
    CHECK(poseBytes == 64);

    std::vector<RaceRow> races;
    for (uint32_t humans : { 4u, 8u, 12u }) {
        for (uint32_t loss : { 0u, 2u }) {
            gRng = 0xA11CE000u + humans * 16 + loss;
            LobbyRig rig(loss);
            races.push_back(rig.Scenario(humans, poseBytes));
        }
    }

    // The track: the largest the format allows, 128 KB, of synthetic bytes identified by their own hash.
    CHECK(kBurstBytes <= track::kMaxBytes);
    std::vector<uint8_t> bytes(kBurstBytes);
    gRng = 0xB0C7u;
    for (uint8_t& b : bytes) {
        b = static_cast<uint8_t>(NextRandom());
    }
    const track::TrackId id = track::Identify(bytes.data(), bytes.size());
    std::vector<Burst> bursts;
    for (uint32_t loss : { 0u, 2u }) {
        bursts.push_back(RunBurst(bytes, id, loss));
        const Burst& b = bursts.back();
        const uint64_t content = static_cast<uint64_t>(kBurstSeats) * bytes.size();
        CHECK(!b.timedOut && b.seatsDone == kBurstSeats);
        CHECK(b.refused == 0);
        CHECK(b.hostBytes == b.channelBytes);
        // Every seat takes the whole track at least once. The top of the range allows the per-chunk framing (83 bytes
        // on 1100), acknowledgements and the Offer and Need messages, and at 2% loss the resends, within 25%.
        CHECK(b.hostBytes >= content && b.hostBytes <= content + content / 4);
    }

    std::printf("\nhost upload in a race: 30 racers, %u s of racing, poses at 30 Hz, wire bytes (datagram + %u B)\n",
                kWindowSeconds, kHeaderBytes);
    std::printf("humans  loss  guest links  host kbit/s  per racer kbit/s  per watcher kbit/s  datagrams/s\n");
    for (const RaceRow& r : races) {
        std::printf("%6u  %3u%%  %11zu  %11.1f  %16.1f  %18.1f  %11.1f\n", r.humans, r.loss, r.racers + r.watchers,
                    r.hostKbps, r.racerKbps, r.watcherKbps, r.datagramsPerSecond);
    }

    const uint64_t content = static_cast<uint64_t>(kBurstSeats) * bytes.size();
    std::printf("\n128 KB track (%zu chunks) to %zu seats: host bytes on the wire\n", xfer::ChunkCount(bytes.size()),
                kBurstSeats);
    std::printf("loss  host bytes  content bytes  overhead  seconds to last seat\n");
    for (size_t i = 0; i < bursts.size(); i++) {
        const Burst& b = bursts[i];
        const double overhead = 100.0 * (static_cast<double>(b.hostBytes) / static_cast<double>(content) - 1.0);
        std::printf("%3u%%  %10llu  %13llu  %7.1f%%  %20.2f\n", b.loss,
                    static_cast<unsigned long long>(b.hostBytes), static_cast<unsigned long long>(content), overhead,
                    static_cast<double>(b.ms) / 1000.0);
    }

    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
