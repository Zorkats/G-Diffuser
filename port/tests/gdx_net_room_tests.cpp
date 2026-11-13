// port/tests/gdx_net_room_tests.cpp: the room state machine (port/net/gdx_net_room.h) on a fake clock.
//
// No network and no thread: each test calls the machine as the host session would and reads back its state
// and the messages it asked to send. Every timeout is checked one millisecond before it runs out and on it.

#include "../net/gdx_net_lottery.h"
#include "../net/gdx_net_room.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace gdx::net::lobby;
using gdx::net::IsZeroIp;
using gdx::net::MappedV4;

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

constexpr uint8_t kHostMachine = 4;

RoomConfig Config(uint32_t roomId, uint32_t pool = kPoolAll) {
    RoomConfig config;
    config.roomId = roomId;
    config.pool = pool;
    return config;
}

PlayerInfo HostInfo() {
    PlayerInfo host;
    host.name = "Host";
    host.colour = 0xFF0000;
    host.machine = kHostMachine;
    return host;
}

LockRequest LockOf(uint8_t machine, uint8_t skin, uint8_t engine) {
    LockRequest lock;
    lock.machine = machine;
    lock.skin = skin;
    lock.engine = engine;
    return lock;
}

// True when the payload passes the decoder of its message type: the machine must never ask the host to send
// something a client would refuse.
bool Decodes(const Outgoing& o) {
    const uint8_t* d = o.payload.data();
    const size_t n = o.payload.size();
    switch (o.type) {
    case kMsgRoster: {
        std::vector<PlayerInfo> v;
        return DecodeRoster(d, n, v);
    }
    case kMsgRoom: {
        RoomState v;
        return DecodeRoomState(d, n, v);
    }
    case kMsgVotes: {
        std::vector<Ballot> v;
        return DecodeVotes(d, n, v);
    }
    case kMsgLottery: {
        LotteryResult v;
        return DecodeLottery(d, n, v);
    }
    case kMsgPicked: {
        Picked v;
        return DecodePicked(d, n, v);
    }
    case kMsgEmoteLine: {
        EmoteLine v;
        return DecodeEmoteLine(d, n, v);
    }
    case kMsgGrid: {
        Grid v;
        return DecodeGrid(d, n, v);
    }
    case kMsgStart: {
        StartSignal v;
        return DecodeStart(d, n, v);
    }
    case kMsgResults: {
        RaceResults v;
        return DecodeResults(d, n, v);
    }
    default:
        return false;
    }
}

size_t Count(const std::vector<Outgoing>& out, uint8_t type) {
    size_t n = 0;
    for (const Outgoing& o : out) {
        if (o.type == type) {
            n++;
        }
    }
    return n;
}

std::vector<uint8_t> Ids(const Grid& grid) {
    std::vector<uint8_t> ids;
    for (const GridEntry& e : grid.humans) {
        ids.push_back(e.id);
    }
    return ids;
}

// One room, its clock and every message it has asked to send.
struct Rig {
    RoomMachine m;
    std::vector<Outgoing> out;
    uint32_t now = 1000;
    uint32_t go = 0; // when the race ToRacing started began: the host's count of it runs from here

    explicit Rig(const RoomConfig& config = Config(77)) : m(config, HostInfo()) {
    }

    JoinRequest Request(const std::string& name, Seat seat, uint8_t machine) const {
        JoinRequest join;
        join.build = "b";
        join.name = name;
        join.colour = 0x112233;
        join.machine = machine;
        join.seat = seat;
        return join;
    }

    uint8_t Add(const std::string& name, Seat seat = Seat::Race, uint8_t machine = 3) {
        uint8_t id = kSystemId;
        RejectReason why = RejectReason::Kicked;
        CHECK(m.Join(Request(name, seat, machine), now, id, why, out));
        return id;
    }

    // The reason a join is refused with. Kicked, which Join never gives, when it was accepted.
    RejectReason Refusal(const JoinRequest& join) {
        uint8_t id = kSystemId;
        RejectReason why = RejectReason::Kicked;
        const size_t seated = m.Roster().size();
        CHECK(!m.Join(join, now, id, why, out));
        CHECK(m.Roster().size() == seated);
        return why;
    }

    void At(uint32_t t) {
        now = t;
        m.Update(now, out);
    }

    void After(uint32_t ms) {
        At(now + ms);
    }

    Phase P() const {
        return m.GetPhase();
    }

    RoomState S() const {
        return m.State(now);
    }

    const PlayerInfo* Who(uint8_t id) const {
        for (const PlayerInfo& p : m.Roster()) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    }

    std::vector<uint8_t> OnGrid() const {
        std::vector<uint8_t> ids;
        for (const PlayerInfo& p : m.Roster()) {
            if (p.inRace) {
                ids.push_back(p.id);
            }
        }
        return ids;
    }

    const Outgoing* Last(uint8_t type) const {
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            if (it->type == type) {
                return &*it;
            }
        }
        return nullptr;
    }

    void LockAll() {
        for (uint8_t id : OnGrid()) {
            if (!Who(id)->locked) {
                CHECK(m.Lock(id, LockOf(1, 0, 50), now, out));
            }
        }
    }

    // The ready message of a copy that loaded the track whose digest starts with `track`.
    ReadyToRace Loaded(uint8_t track = 0, uint16_t loadMs = 0) const {
        ReadyToRace ready;
        ready.raceIndex = m.State(now).raceIndex;
        ready.digest[0] = track;
        ready.loadMs = loadMs;
        return ready;
    }

    bool GridReady(uint8_t id, uint8_t track = 0) {
        return m.GridReady(id, Loaded(track), now, out);
    }

    void GridAll() {
        for (uint8_t id : OnGrid()) {
            CHECK(GridReady(id));
        }
    }

    // From the lock-in screen to the results, with the given finish order.
    void Race(const std::vector<uint8_t>& order) {
        CHECK(P() == Phase::Machine);
        LockAll();
        CHECK(P() == Phase::GridSync);
        GridAll();
        CHECK(P() == Phase::Countdown);
        After(kCountdownMs);
        CapRace(order);
    }

    // The race that is running goes to its cap with nobody over the line. order is the host game's order
    // of those still driving, or empty when the game said nothing.
    void CapRace(const std::vector<uint8_t>& order) {
        CHECK(P() == Phase::Racing);
        if (!order.empty()) {
            m.SetStanding(order);
        }
        After(kRaceCapMs);
        CHECK(P() == Phase::Results);
    }

    // From the lock-in screen to a race that runs.
    void ToRacing() {
        CHECK(P() == Phase::Machine);
        LockAll();
        CHECK(P() == Phase::GridSync);
        GridAll();
        CHECK(P() == Phase::Countdown);
        After(kCountdownMs);
        CHECK(P() == Phase::Racing);
        go = now;
    }

    // From the results to the next lock-in screen, whichever way the track is chosen.
    void NextRace() {
        CHECK(P() == Phase::Results);
        After(kResultsMs);
        if (P() == Phase::Voting) {
            SkipVote();
        } else if (P() == Phase::Pick) {
            After(kPickMs);
        }
        CHECK(P() == Phase::Machine);
    }

    FinishReport Report(ResultKind kind, uint32_t timeMs) const {
        FinishReport report;
        report.raceIndex = m.State(now).raceIndex;
        report.kind = kind;
        report.timeMs = timeMs;
        return report;
    }

    // id crosses the line: its poses named the last lap, and its own game counted timeMs.
    bool Finish(uint8_t id, uint32_t timeMs) {
        m.NoteLap(id, static_cast<int16_t>(m.LastGrid().rules.laps));
        return m.ReportFinish(id, Report(ResultKind::Finished, timeMs), now, out);
    }

    bool Retire(uint8_t id) {
        return m.ReportFinish(id, Report(ResultKind::Retired, 0), now, out);
    }

    // The table last sent to everyone, which is the one the room keeps.
    RaceResults Table() const {
        RaceResults table;
        const Outgoing* sent = nullptr;
        for (auto it = out.rbegin(); it != out.rend() && sent == nullptr; ++it) {
            if (it->type == kMsgResults && it->to == kEveryone) {
                sent = &*it;
            }
        }
        CHECK(sent != nullptr && DecodeResults(sent->payload.data(), sent->payload.size(), table) &&
              table == m.Results());
        return table;
    }

    // Starts the series and goes on to the lock-in screen: the vote that opens by default (decision 75)
    // runs out with no ballot cast, and a room on LAST PLACE PICKS draws its first race.
    void Begin() {
        CHECK(m.Start(now, out));
        if (P() == Phase::Voting) {
            SkipVote();
        }
        CHECK(P() == Phase::Machine);
    }

    // From an open vote to the lock-in screen, with no ballot cast.
    void SkipVote() {
        CHECK(P() == Phase::Voting);
        After(kVoteMs);
        CHECK(P() == Phase::Lottery);
        CHECK(m.SkipLottery(now, out));
        CHECK(P() == Phase::Machine);
    }

    void Verify() {
        for (const Outgoing& o : out) {
            CHECK(Decodes(o));
        }
    }
};

// The phase began at r.now: it holds one millisecond before its timer runs out and is left exactly on it.
void ExpectTimeout(Rig& r, Phase during, uint32_t lengthMs, Phase after) {
    const uint32_t start = r.now;
    CHECK(r.P() == during);
    CHECK(r.S().phaseTotalMs == lengthMs && r.S().phaseLeftMs == lengthMs);
    r.At(start + lengthMs - 1);
    CHECK(r.P() == during);
    CHECK(r.S().phaseLeftMs == 1);
    r.At(start + lengthMs);
    CHECK(r.P() == after);
}

// ---------------------------------------------------------------------------------------------------------

void TestDraws() {
    std::printf("draws\n");
    for (uint32_t h = 0; h < 2000; h++) {
        const uint8_t t = DrawTrack(h * 2654435761u, kPoolAll, 5);
        CHECK(t < kStockTracks && t != 5);
    }
    const uint32_t two = (1u << 3) | (1u << 9);
    for (uint32_t h = 0; h < 50; h++) {
        CHECK(DrawTrack(h, two, 3) == 9);
        CHECK(DrawTrack(h, 1u << 5, 5) == 5); // the pool holds nothing else
        const uint8_t t = DrawTrack(h, two, kTrackNone);
        CHECK(t == 3 || t == 9);
    }
    CHECK(DrawTrack(123, 0, kTrackNone) == 0);

    const std::vector<Ballot> a = { { 0, 4 }, { 2, 9 } };
    const std::vector<Ballot> b = { { 0, 4 }, { 2, 10 } };
    CHECK(RoomHash(1, 0, a) == RoomHash(1, 0, a));
    CHECK(RoomHash(1, 0, a) != RoomHash(2, 0, a));
    CHECK(RoomHash(1, 0, a) != RoomHash(1, 1, a));
    CHECK(RoomHash(1, 0, a) != RoomHash(1, 0, b));
    CHECK(RoomHash(1, 0, a) != RoomHash(1, 0, std::vector<Ballot>()));

    // The host and a client reach the same result whatever order the ballots are held in.
    const std::vector<Ballot> sorted = { { 0, 4 }, { 1, 9 }, { 3, kTrackRandom }, { 7, 4 } };
    const std::vector<Ballot> shuffled = { { 7, 4 }, { 1, 9 }, { 0, 4 }, { 3, kTrackRandom } };
    for (uint16_t race = 0; race < 200; race++) {
        const LotteryResult one = RunLottery(0xC0FFEE, race, sorted, kPoolAll, 4);
        const LotteryResult other = RunLottery(0xC0FFEE, race, shuffled, kPoolAll, 4);
        CHECK(one == other);
        CHECK(one.ballots == sorted);
        CHECK(one.winner < sorted.size() && one.track < kStockTracks);
        const uint8_t voted = sorted[one.winner].track;
        CHECK(voted == kTrackRandom ? one.track != 4 : one.track == voted); // a Random win avoids the last track
        const std::vector<uint8_t> bytes = EncodeLottery(one);
        LotteryResult decoded;
        CHECK(DecodeLottery(bytes.data(), bytes.size(), decoded) && decoded == one);
    }

    // Nobody voted: no winner, and the track is the plain draw.
    const LotteryResult empty = RunLottery(9, 3, std::vector<Ballot>(), kPoolAll, 6);
    CHECK(empty.winner == kNoWinner && empty.ballots.empty());
    CHECK(empty.track == DrawnTrack(9, 3, kPoolAll, 6) && empty.track != 6);

    // A track's odds are its share of the votes: three ballots of four win about three times in four.
    const std::vector<Ballot> skewed = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 2 } };
    int ones = 0;
    const int rounds = 4000;
    for (int i = 0; i < rounds; i++) {
        ones += RunLottery(static_cast<uint32_t>(i) * 7919u + 1u, static_cast<uint16_t>(i), skewed, kPoolAll,
                           kTrackNone)
                        .track == 1
                    ? 1
                    : 0;
    }
    CHECK(ones > rounds * 70 / 100 && ones < rounds * 80 / 100);

    // The plain draw spreads over the pool rather than sticking to a few tracks.
    std::set<uint8_t> drawn;
    for (uint32_t room = 1; room <= 400; room++) {
        drawn.insert(DrawnTrack(room, 0, kPoolAll, kTrackNone));
    }
    CHECK(drawn.size() == kStockTracks);
}

void TestSeats() {
    std::printf("seats\n");
    Rig r;
    CHECK(r.m.Roster().size() == 1 && r.Who(kHostId) != nullptr && r.Who(kHostId)->host);
    CHECK(!r.Who(kHostId)->watcher && r.Who(kHostId)->machine == kHostMachine);
    for (uint8_t i = 1; i <= 7; i++) {
        CHECK(r.Add("P" + std::to_string(i)) == i);
    }
    CHECK(r.m.Racers() == 8 && r.m.Watchers() == 0);
    CHECK(r.Refusal(r.Request("Late", Seat::Race, 3)) == RejectReason::RaceSeatsFull);

    CHECK(!r.m.SetRacerCap(7, r.now, r.out)); // under the eight racers seated
    CHECK(!r.m.SetRacerCap(0, r.now, r.out));
    CHECK(!r.m.SetRacerCap(13, r.now, r.out));
    CHECK(r.S().racerCap == 8);
    CHECK(r.m.SetRacerCap(12, r.now, r.out));
    CHECK(r.S().racerCap == 12);
    for (uint8_t i = 8; i <= 11; i++) {
        CHECK(r.Add("P" + std::to_string(i)) == i);
    }
    CHECK(r.m.Racers() == kMaxRacers);
    CHECK(r.Refusal(r.Request("Thirteen", Seat::Race, 3)) == RejectReason::RaceSeatsFull); // the 13th racer
    CHECK(!r.m.SetRacerCap(8, r.now, r.out)); // never below the racers seated

    const uint8_t w1 = r.Add("W1", Seat::Watch);
    const uint8_t w2 = r.Add("W2", Seat::Watch);
    CHECK(w1 == 12 && w2 == 13 && r.Who(w1)->watcher && r.Who(w2)->watcher);
    CHECK(r.m.Racers() == kMaxRacers && r.m.Watchers() == kMaxWatchers);
    CHECK(r.Refusal(r.Request("W3", Seat::Watch, 3)) == RejectReason::Full); // the 3rd watcher
    CHECK(r.Refusal(r.Request("Thirteen", Seat::Race, 3)) == RejectReason::Full);

    r.m.Leave(5, r.now, r.out);
    CHECK(r.Who(5) == nullptr && r.m.Racers() == kMaxRacers - 1);
    CHECK(r.Refusal(r.Request("W3", Seat::Watch, 3)) == RejectReason::WatchSeatsFull);
    CHECK(r.Add("New") == 5); // the freed id is reused
    for (size_t i = 1; i < r.m.Roster().size(); i++) {
        CHECK(r.m.Roster()[i - 1].id < r.m.Roster()[i].id);
    }

    r.m.Leave(kHostId, r.now, r.out); // the host cannot leave
    r.m.Leave(200, r.now, r.out);     // nor somebody who is not here
    CHECK(r.m.Roster().size() == kMaxPlayers && r.Who(kHostId) != nullptr);

    Rig bad;
    CHECK(bad.Refusal(bad.Request(" x", Seat::Race, 3)) == RejectReason::BadProfile);
    CHECK(bad.Refusal(bad.Request("Ana", Seat::Race, 30)) == RejectReason::BadProfile);
    JoinRequest colour = bad.Request("Ana", Seat::Race, 3);
    colour.colour = 0x1000000;
    CHECK(bad.Refusal(colour) == RejectReason::BadProfile);
    r.Verify();
    bad.Verify();
}

void TestLobby() {
    std::printf("lobby\n");
    Rig r;
    CHECK(r.S().phaseTotalMs == 0);
    r.After(3600000); // the lobby has no timer
    CHECK(r.P() == Phase::Lobby);

    // Nothing to tell anyone when nothing changed.
    const size_t quiet = r.out.size();
    r.After(16);
    CHECK(r.m.SetReady(kHostId, false, r.now, r.out));
    CHECK(r.out.size() == quiet);
    r.m.SetPing(kHostId, 0);
    r.After(16);
    CHECK(r.out.size() == quiet);

    CHECK(r.m.SetReady(kHostId, true, r.now, r.out));
    CHECK(r.P() == Phase::Lobby && r.Who(kHostId)->ready); // one ready racer is not a race
    const uint8_t ana = r.Add("Ana");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(r.P() == Phase::Lobby);
    CHECK(!r.m.SetReady(wim, true, r.now, r.out)); // a watcher has no ready mark
    CHECK(!r.m.SetReady(99, true, r.now, r.out));
    r.m.SetPing(ana, 42);
    r.After(16);
    CHECK(r.Who(ana)->pingMs == 42 && r.out.back().type == kMsgRoster);

    CHECK(r.m.SetReady(ana, true, r.now, r.out)); // two racers, every one ready: the series starts
    CHECK(r.P() == Phase::Voting); // and two racers vote for the track (decision 75)
    CHECK(!r.Who(kHostId)->ready && !r.Who(ana)->ready);
    CHECK(!r.m.SetReady(ana, true, r.now, r.out)); // no ready mark outside the lobby
    CHECK(!r.m.Start(r.now, r.out));
    r.Verify();

    // The host may start alone, and a room of one votes too (decision 75).
    Rig solo;
    CHECK(solo.m.Start(solo.now, solo.out));
    CHECK(solo.P() == Phase::Voting);
    solo.SkipVote();
    CHECK(solo.P() == Phase::Machine && solo.S().racers == 1);
    solo.Verify();
}

void TestTimeouts() {
    std::printf("timeouts\n");
    Rig r;
    r.now = 0xFFFFF000u; // the vote's timer runs across the clock's wrap
    r.Add("Ana");
    r.Add("Bo");
    r.Add("Cy");
    CHECK(r.m.Start(r.now, r.out));
    ExpectTimeout(r, Phase::Voting, kVoteMs, Phase::Lottery);
    CHECK(r.m.Lottery().winner == kNoWinner && r.S().track < kStockTracks);
    ExpectTimeout(r, Phase::Lottery, kLotteryMs, Phase::Machine);
    ExpectTimeout(r, Phase::Machine, kMachineMs, Phase::GridSync);
    r.GridAll();
    ExpectTimeout(r, Phase::Countdown, kCountdownMs, Phase::Racing);
    ExpectTimeout(r, Phase::Racing, kRaceCapMs, Phase::Results);
    CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ 0, 1, 2, 3 })); // nobody finished: id order
    const uint8_t first = r.S().track;
    CHECK(r.S().raceIndex == 0);
    ExpectTimeout(r, Phase::Results, kResultsMs, Phase::Voting);
    CHECK(r.S().raceIndex == 1 && r.S().lastTrack == first && r.S().track == kTrackNone);

    // A vote nobody cast a ballot in never draws the track just raced.
    r.After(kVoteMs);
    CHECK(r.P() == Phase::Lottery && r.S().track != first);
    CHECK(r.m.SkipLottery(r.now, r.out));

    // The grid: a racer who has not loaded the track when the time runs out watches the race.
    r.LockAll();
    CHECK(r.P() == Phase::GridSync);
    CHECK(r.GridReady(kHostId));
    CHECK(r.GridReady(2));
    ExpectTimeout(r, Phase::GridSync, kGridMs, Phase::Countdown);
    CHECK(r.OnGrid() == (std::vector<uint8_t>{ 0, 2 }));
    CHECK(r.S().racers == 2 && !r.Who(1)->locked && !r.Who(1)->watcher);
    r.Verify();

    // When the host chose LAST PLACE PICKS, last place has kPickMs to pick.
    Rig p;
    p.Add("Ana");
    CHECK(p.m.SetLastPlacePicks(true, p.now, p.out));
    p.Begin();
    p.Race({ 0, 1 });
    p.After(kResultsMs);
    ExpectTimeout(p, Phase::Pick, kPickMs, Phase::Machine);
    p.Verify();

    // Nobody on the grid: there is no race to run.
    Rig g;
    g.Add("Ana");
    CHECK(g.m.SetLastPlacePicks(true, g.now, g.out));
    g.Begin();
    g.LockAll();
    ExpectTimeout(g, Phase::GridSync, kGridMs, Phase::Results);
    CHECK(g.m.LastOrder().empty() && g.OnGrid().empty());
    g.After(kResultsMs); // no last place to pick: the room draws
    CHECK(g.P() == Phase::Machine && g.m.LastPicked().drawn);
    g.Verify();
}

// O6.1 and O6.4 (decision 39): an Open room with a key commits to each lottery before the first ballot, draws it
// from the salt, and reveals the salt; a copy checks both with CheckLottery. A LAN room draws as before.
// O5.10: the directory code is set once the room is listed, travels in `room`, and the successor keeps it.
void TestRoomCode() {
    std::printf("room code (O5.10)\n");
    Rig lan;
    CHECK(!lan.m.SetCode(5) && lan.S().code == kNoRoomCode);
    RoomConfig config = Config(77);
    config.type = RoomType::Code;
    Rig r(config);
    r.Add("Ana");
    CHECK(r.S().code == kNoRoomCode);
    CHECK(!r.m.SetCode(kRoomCodeValues) && r.S().code == kNoRoomCode);
    r.After(10);
    r.out.clear();
    CHECK(r.m.SetCode(4242));
    r.After(10);
    RoomState told;
    const Outgoing* sent = r.Last(kMsgRoom);
    CHECK(sent != nullptr && DecodeRoomState(sent->payload.data(), sent->payload.size(), told) &&
          told.code == 4242);
    CHECK(r.S().code == 4242);
}

void TestCommittedLottery() {
    std::printf("committed lottery (O6.4)\n");
    RoomConfig config = Config(77);
    config.type = RoomType::Open;
    for (size_t i = 0; i < config.lotteryKey.size(); i++) {
        config.lotteryKey[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    Rig r(config);
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    const uint8_t cy = r.Add("Cy");
    CHECK(r.S().type == RoomType::Open && IsZero32(r.S().commitment)); // nothing before a vote
    CHECK(r.m.Start(r.now, r.out));
    CHECK(r.P() == Phase::Voting);
    const RoomState voting = r.S();
    CHECK(!IsZero32(voting.commitment));
    RoomState wire;
    const Outgoing* state = r.Last(kMsgRoom);
    CHECK(state != nullptr && DecodeRoomState(state->payload.data(), state->payload.size(), wire) &&
          wire.commitment == voting.commitment && wire.type == RoomType::Open);
    CHECK(r.m.Vote(kHostId, 3, r.now, r.out));
    CHECK(r.m.Vote(ana, 5, r.now, r.out));
    CHECK(r.m.Vote(bo, 9, r.now, r.out));
    CHECK(r.m.Vote(cy, kTrackRandom, r.now, r.out));
    CHECK(r.P() == Phase::Lottery);
    LotteryResult sent;
    const Outgoing* lottery = r.Last(kMsgLottery);
    CHECK(lottery != nullptr && DecodeLottery(lottery->payload.data(), lottery->payload.size(), sent));
    CHECK(sent == r.m.Lottery() && !IsZero32(sent.salt));
    CHECK(gdx::net::lottery::Verify(voting.commitment, 77, 0, sent.salt));
    CHECK(sent.salt == gdx::net::lottery::RaceSalt(config.lotteryKey, 77, 0));
    CHECK(sent == RunCommittedLottery(sent.salt, 77, 0, sent.ballots, kPoolAll, kTrackNone));
    CHECK(CheckLottery(voting, sent));
    // A wrong salt, another commitment, or an Open room that did not commit all fail the check.
    LotteryResult forged = sent;
    forged.salt[0] ^= 1;
    CHECK(!CheckLottery(voting, forged));
    RoomState other = voting;
    other.commitment[5] ^= 1;
    CHECK(!CheckLottery(other, sent));
    RoomState bare = voting;
    bare.commitment = Bytes32{};
    CHECK(!CheckLottery(bare, sent));
    // The host may not swap the winner after the reveal either.
    forged = sent;
    forged.winner = static_cast<uint8_t>((sent.winner + 1) % sent.ballots.size());
    forged.track = forged.ballots[forged.winner].track == kTrackRandom ? sent.track : forged.ballots[forged.winner].track;
    CHECK(!CheckLottery(voting, forged));
    // Each race has its own salt.
    CHECK(gdx::net::lottery::RaceSalt(config.lotteryKey, 77, 0) != gdx::net::lottery::RaceSalt(config.lotteryKey, 77, 1));

    // A successor keeps the room type and the directory code (O5.10), and commits with its own key.
    CHECK(r.m.SetCode(777));
    RoomMirror mirror;
    mirror.roster = r.m.Roster();
    mirror.room = r.S();
    Bytes32 key2{};
    key2[0] = 42;
    std::optional<RoomMachine> promoted = RoomMachine::Promote(mirror, ana, r.now, key2);
    CHECK(promoted.has_value() && promoted->State(r.now).type == RoomType::Open &&
          promoted->State(r.now).code == 777);

    // The same key in a LAN room commits to nothing, and the lottery checks as before.
    RoomConfig lanConfig = config;
    lanConfig.type = RoomType::Lan;
    Rig l(lanConfig);
    const uint8_t la = l.Add("Ana");
    const uint8_t lb = l.Add("Bo");
    const uint8_t lc = l.Add("Cy");
    CHECK(l.m.Start(l.now, l.out));
    const RoomState lanVoting = l.S();
    CHECK(lanVoting.type == RoomType::Lan && IsZero32(lanVoting.commitment));
    CHECK(l.m.Vote(kHostId, 3, l.now, l.out) && l.m.Vote(la, 5, l.now, l.out) && l.m.Vote(lb, 9, l.now, l.out) &&
          l.m.Vote(lc, 2, l.now, l.out));
    const LotteryResult lanResult = l.m.Lottery();
    CHECK(IsZero32(lanResult.salt) && CheckLottery(lanVoting, lanResult));
    CHECK(lanResult == RunLottery(77, 0, l.m.Votes(), kPoolAll, kTrackNone));
    // A salt where the room committed to nothing is refused.
    LotteryResult salted = lanResult;
    salted.salt[0] = 1;
    CHECK(!CheckLottery(lanVoting, salted));
}

void TestVote() {
    std::printf("vote\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    const uint8_t cy = r.Add("Cy");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(!r.m.Vote(ana, 3, r.now, r.out)); // no vote is open
    CHECK(r.m.Start(r.now, r.out));
    CHECK(r.P() == Phase::Voting); // four racers vote
    const Outgoing* votes = r.Last(kMsgVotes);
    CHECK(votes != nullptr && votes->to == kEveryone && votes->payload == EncodeVotes({}));

    CHECK(!r.m.Vote(wim, 3, r.now, r.out)); // a watcher has no ballot
    CHECK(!r.m.Vote(ana, 24, r.now, r.out));
    CHECK(!r.m.Vote(99, 3, r.now, r.out));
    CHECK(r.m.Vote(bo, 9, r.now, r.out));
    CHECK(!r.m.Vote(bo, 10, r.now, r.out)); // one ballot, final
    CHECK(r.m.Vote(kHostId, kTrackRandom, r.now, r.out));
    CHECK(r.m.Votes() == (std::vector<Ballot>{ { kHostId, kTrackRandom }, { bo, 9 } }));
    CHECK(r.Last(kMsgVotes)->payload == EncodeVotes(r.m.Votes()));

    // Somebody who joins mid-vote is told the ballots so far.
    const uint8_t dee = r.Add("Dee");
    CHECK(r.out.back().to == dee && r.out.back().type == kMsgVotes);
    CHECK(r.out.back().payload == EncodeVotes(r.m.Votes()));
    r.m.Leave(dee, r.now, r.out);

    r.After(5000);
    CHECK(r.m.Vote(ana, 9, r.now, r.out));
    CHECK(r.P() == Phase::Voting);
    CHECK(r.m.Vote(cy, 2, r.now, r.out)); // the last ballot closes the vote early
    CHECK(r.P() == Phase::Lottery);
    const LotteryResult result = r.m.Lottery();
    CHECK(result.ballots.size() == 4 && result.winner < 4);
    CHECK(result == RunLottery(77, 0, r.m.Votes(), kPoolAll, kTrackNone));
    CHECK(r.S().track == result.track);

    // What a client is sent is enough to run the same draw.
    LotteryResult sent;
    const Outgoing* lottery = r.Last(kMsgLottery);
    CHECK(lottery != nullptr && DecodeLottery(lottery->payload.data(), lottery->payload.size(), sent));
    RoomState room;
    const Outgoing* state = r.Last(kMsgRoom);
    CHECK(state != nullptr && DecodeRoomState(state->payload.data(), state->payload.size(), room));
    CHECK(room.phase == Phase::Lottery && room.phaseLeftMs == kLotteryMs);
    CHECK(RunLottery(room.roomId, room.raceIndex, sent.ballots, room.pool, room.lastTrack) == sent);

    // Somebody who joins while the result is shown is sent it.
    const uint8_t eve = r.Add("Eve");
    CHECK(r.out.back().to == eve && r.out.back().type == kMsgLottery);
    CHECK(!r.m.Vote(eve, 1, r.now, r.out));

    CHECK(r.m.SkipLottery(r.now, r.out));
    CHECK(r.P() == Phase::Machine);
    CHECK(!r.m.SkipLottery(r.now, r.out));
    r.Verify();

    // A missing ballot is an abstention.
    Rig t;
    t.Add("Ana");
    t.Add("Bo");
    t.Add("Cy");
    CHECK(t.m.Start(t.now, t.out));
    CHECK(t.m.Vote(1, 6, t.now, t.out));
    CHECK(t.m.Vote(3, 8, t.now, t.out));
    t.After(kVoteMs);
    CHECK(t.P() == Phase::Lottery);
    CHECK(t.m.Lottery().ballots.size() == 2 && t.m.Lottery().winner < 2);
    CHECK(t.m.Lottery().track == 6 || t.m.Lottery().track == 8);
    t.Verify();

    // A player who leaves takes the ballot along, and the vote closes when the rest have all voted.
    Rig l;
    l.Add("Ana");
    l.Add("Bo");
    l.Add("Cy");
    CHECK(l.m.Start(l.now, l.out));
    CHECK(l.m.Vote(0, 1, l.now, l.out));
    CHECK(l.m.Vote(1, 2, l.now, l.out));
    CHECK(l.m.Vote(3, 3, l.now, l.out));
    l.m.Leave(3, l.now, l.out);
    CHECK(l.P() == Phase::Voting && l.m.Votes().size() == 2);
    l.m.Leave(2, l.now, l.out); // the only one who had not voted
    CHECK(l.P() == Phase::Lottery && l.m.Lottery().ballots.size() == 2);
    l.Verify();

    // A pool of two tracks: nothing else can be voted, and a Random ballot draws from it.
    const uint32_t pool = (1u << 2) | (1u << 7);
    for (uint32_t room = 1; room <= 40; room++) {
        Rig q(Config(room, pool));
        q.Add("Ana");
        q.Add("Bo");
        q.Add("Cy");
        CHECK(q.m.Start(q.now, q.out));
        CHECK(!q.m.Vote(0, 3, q.now, q.out));
        for (uint8_t id = 0; id < 4; id++) {
            CHECK(q.m.Vote(id, kTrackRandom, q.now, q.out));
        }
        CHECK(q.P() == Phase::Lottery && (q.S().track == 2 || q.S().track == 7));
        q.Verify();
    }
}

void TestPick() {
    std::printf("pick\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    CHECK(!r.m.Pick(ana, 3, r.now, r.out));
    CHECK(r.m.SetLastPlacePicks(true, r.now, r.out));
    CHECK(r.S().lastPlacePicks);
    CHECK(r.m.Start(r.now, r.out));
    // First race of the series: nobody finished last yet, so the room draws.
    CHECK(r.P() == Phase::Machine);
    Picked sent;
    const Outgoing* picked = r.Last(kMsgPicked);
    CHECK(picked != nullptr && DecodePicked(picked->payload.data(), picked->payload.size(), sent));
    CHECK(sent == r.m.LastPicked() && sent.drawn && sent.by == kSystemId && sent.track == r.S().track);
    CHECK(sent.track == DrawnTrack(77, 0, kPoolAll, kTrackNone));
    const uint8_t first = r.S().track;

    r.Race({ bo, kHostId, ana });
    CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ bo, kHostId, ana }));
    r.After(kResultsMs);
    CHECK(r.P() == Phase::Pick && r.S().picker == ana); // last place picks
    CHECK(r.S().raceIndex == 1 && r.S().lastTrack == first);
    CHECK(!r.m.Pick(kHostId, 4, r.now, r.out)); // not the picker
    CHECK(!r.m.Pick(ana, 24, r.now, r.out));
    CHECK(!r.m.Vote(ana, 4, r.now, r.out));
    CHECK(r.P() == Phase::Pick);
    CHECK(r.m.Pick(ana, 7, r.now, r.out));
    CHECK(r.P() == Phase::Machine && r.S().track == 7 && r.S().picker == kSystemId);
    CHECK(r.m.LastPicked() == (Picked{ 7, false, ana }));

    // A racer the order leaves out ranks below the ones it names, so Bo picks. Random asks the room.
    r.Race({ kHostId });
    CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ kHostId, ana, bo }));
    r.After(kResultsMs);
    CHECK(r.S().picker == bo);
    CHECK(r.m.Pick(bo, kTrackRandom, r.now, r.out));
    CHECK(r.m.LastPicked().drawn && r.m.LastPicked().by == bo);
    CHECK(r.S().track != 7 && r.S().track < kStockTracks); // never the track just raced

    // The picker leaves: the room draws at once.
    r.Race({ kHostId, bo, ana });
    r.After(kResultsMs);
    CHECK(r.P() == Phase::Pick && r.S().picker == ana);
    r.m.Leave(ana, r.now, r.out);
    CHECK(r.P() == Phase::Machine && r.m.LastPicked().drawn && r.m.LastPicked().by == kSystemId);

    // Last place left during the results: the pick goes to the one above.
    r.Race({ kHostId, bo });
    const uint8_t cy = r.Add("Cy");
    r.m.Leave(bo, r.now, r.out);
    r.After(kResultsMs);
    CHECK(r.P() == Phase::Pick && r.S().picker == kHostId);
    CHECK(r.Who(cy) != nullptr);
    r.Verify();

    // A pick that times out draws a track, and never the one just raced.
    std::set<uint8_t> drawn;
    for (uint32_t room = 1; room <= 300; room++) {
        Rig t(Config(room));
        t.Add("Ana");
        CHECK(t.m.SetLastPlacePicks(true, t.now, t.out));
        CHECK(t.m.Start(t.now, t.out));
        const uint8_t raced = t.S().track;
        t.Race({ 0, 1 });
        t.After(kResultsMs);
        CHECK(t.P() == Phase::Pick && t.S().picker == 1);
        t.After(kPickMs);
        CHECK(t.P() == Phase::Machine && t.S().track < kStockTracks && t.S().track != raced);
        CHECK(t.m.LastPicked().drawn && t.m.LastPicked().by == kSystemId);
        drawn.insert(t.S().track);
    }
    CHECK(drawn.size() > 12);

    // The host turns LAST PLACE PICKS on during a series: the next race is picked, and the one after it
    // voted again once it is off.
    Rig v;
    v.Add("Ana");
    v.Add("Bo");
    v.Add("Cy");
    CHECK(v.m.Start(v.now, v.out));
    v.SkipVote();
    CHECK(v.m.SetLastPlacePicks(true, v.now, v.out));
    v.Race({ 3, 2, 1, 0 });
    v.m.Leave(2, v.now, v.out);
    v.After(kResultsMs);
    CHECK(v.P() == Phase::Pick && v.S().picker == kHostId);
    CHECK(v.m.Pick(kHostId, 5, v.now, v.out));
    CHECK(v.m.SetLastPlacePicks(false, v.now, v.out));
    v.Race({ 3, 1, 0 });
    v.After(kResultsMs);
    CHECK(v.P() == Phase::Voting && !v.S().lastPlacePicks);
    v.Verify();
}

void TestMachine() {
    std::printf("lock-in screen\n");
    Rig r;
    const uint8_t ana = r.Add("Ana", Seat::Race, 7);
    const uint8_t bo = r.Add("Bo", Seat::Race, kNoMachine);
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(!r.m.Lock(ana, LockOf(5, 2, 80), r.now, r.out)); // a lock outside the lock-in screen is ignored
    CHECK(r.Who(ana)->machine == 7 && !r.Who(ana)->locked);
    CHECK(r.S().racers == 0 && r.S().locked == 0);
    CHECK(r.m.Start(r.now, r.out));
    r.SkipVote();
    CHECK(r.S().racers == 3 && r.S().locked == 0); // the watcher is not counted
    CHECK(r.Who(ana)->inRace && !r.Who(wim)->inRace);

    CHECK(!r.m.Lock(wim, LockOf(5, 2, 80), r.now, r.out));
    CHECK(!r.m.Lock(ana, LockOf(30, 0, 50), r.now, r.out));
    CHECK(!r.m.Lock(ana, LockOf(5, 4, 50), r.now, r.out));
    CHECK(!r.m.Lock(ana, LockOf(5, 0, 101), r.now, r.out));
    CHECK(!r.m.Lock(99, LockOf(5, 0, 50), r.now, r.out));
    CHECK(r.m.Engine(ana) == 50);
    CHECK(r.m.Lock(ana, LockOf(5, 2, 80), r.now, r.out));
    CHECK(r.Who(ana)->machine == 5 && r.Who(ana)->skin == 2 && r.Who(ana)->locked && r.m.Engine(ana) == 80);
    CHECK(r.S().locked == 1 && r.P() == Phase::Machine);
    CHECK(!r.m.Lock(ana, LockOf(6, 0, 10), r.now, r.out)); // final for this race
    CHECK(r.Who(ana)->machine == 5);

    // A racer who joins now watches this race: the screen neither counts nor waits for them.
    const uint8_t cy = r.Add("Cy");
    CHECK(!r.Who(cy)->watcher && !r.Who(cy)->inRace && r.S().racers == 3);
    CHECK(!r.m.Lock(cy, LockOf(1, 0, 50), r.now, r.out));

    r.After(30000);
    CHECK(r.m.Lock(kHostId, LockOf(0, 0, 0), r.now, r.out));
    CHECK(r.P() == Phase::Machine && r.S().locked == 2);
    CHECK(r.m.Lock(bo, LockOf(9, 3, 100), r.now, r.out)); // the last racer locks: the screen closes
    CHECK(r.P() == Phase::GridSync);
    CHECK(!r.m.Lock(bo, LockOf(9, 3, 100), r.now, r.out));

    // The next race counts the late joiner, and nobody is locked any more.
    r.GridAll();
    r.After(kCountdownMs);
    r.CapRace({ ana, bo, kHostId });
    r.After(kResultsMs);
    r.SkipVote();
    CHECK(r.S().racers == 4 && r.S().locked == 0 && r.Who(cy)->inRace && !r.Who(ana)->locked);
    CHECK(r.Who(ana)->machine == 5); // the roster keeps the last machine locked
    r.Verify();

    // The time runs out: a racer who never locked keeps the profile machine.
    Rig t;
    const uint8_t a = t.Add("Ana", Seat::Race, 7);
    const uint8_t b = t.Add("Bo", Seat::Race, kNoMachine);
    const uint8_t c = t.Add("Cy", Seat::Race, 11);
    CHECK(t.m.Start(t.now, t.out));
    t.SkipVote();
    CHECK(t.m.Lock(a, LockOf(5, 1, 10), t.now, t.out));
    ExpectTimeout(t, Phase::Machine, kMachineMs, Phase::GridSync);
    CHECK(t.Who(kHostId)->machine == kHostMachine && !t.Who(kHostId)->locked && t.Who(kHostId)->inRace);
    CHECK(t.Who(a)->machine == 5 && t.Who(a)->locked);
    CHECK(t.Who(c)->machine == 11 && !t.Who(c)->locked);
    CHECK(t.Who(b)->machine == 0); // a profile with no machine gets the first one
    t.Verify();

    // The one racer still choosing leaves: the screen closes.
    Rig l;
    l.Add("Ana");
    l.Add("Bo");
    CHECK(l.m.Start(l.now, l.out));
    l.SkipVote();
    CHECK(l.m.Lock(0, LockOf(1, 0, 50), l.now, l.out));
    CHECK(l.m.Lock(1, LockOf(1, 0, 50), l.now, l.out));
    CHECK(l.P() == Phase::Machine);
    l.m.Leave(2, l.now, l.out);
    CHECK(l.P() == Phase::GridSync && l.S().racers == 2);
    l.Verify();
}

void TestRaceNext() {
    std::printf("race next\n");
    Rig r;
    for (int i = 1; i <= 6; i++) {
        r.Add("P" + std::to_string(i));
    }
    const uint8_t w1 = r.Add("W1", Seat::Watch);
    const uint8_t w2 = r.Add("W2", Seat::Watch);
    CHECK(!r.m.SeatNext(1, true, r.now, r.out)); // a racer already has a seat
    CHECK(!r.m.SeatNext(99, true, r.now, r.out));
    CHECK(r.m.SeatNext(w2, true, r.now, r.out)); // W2 asks first
    CHECK(r.m.SeatNext(w1, true, r.now, r.out));
    CHECK(r.Who(w1)->raceNext && r.Who(w2)->raceNext && r.Who(w2)->watcher);

    CHECK(r.m.Start(r.now, r.out));
    CHECK(!r.m.Vote(w2, 3, r.now, r.out)); // still a watcher while the vote runs
    r.SkipVote();
    // One race seat was free: first come, first served.
    CHECK(!r.Who(w2)->watcher && !r.Who(w2)->raceNext && r.Who(w2)->inRace);
    CHECK(r.Who(w1)->watcher && r.Who(w1)->raceNext && !r.Who(w1)->inRace);
    CHECK(r.m.Racers() == 8 && r.m.Watchers() == 1 && r.S().racers == 8);

    r.m.Leave(3, r.now, r.out);
    r.Race({});
    r.After(kResultsMs);
    r.SkipVote();
    CHECK(!r.Who(w1)->watcher && r.Who(w1)->inRace && r.m.Watchers() == 0);
    r.Verify();

    // A request taken back is not served.
    Rig t;
    t.Add("Ana");
    const uint8_t wim = t.Add("Wim", Seat::Watch);
    CHECK(t.m.SeatNext(wim, true, t.now, t.out));
    CHECK(t.m.SeatNext(wim, false, t.now, t.out));
    CHECK(t.m.Start(t.now, t.out));
    t.SkipVote();
    CHECK(t.P() == Phase::Machine && t.Who(wim)->watcher && !t.Who(wim)->inRace && t.S().racers == 2);
    t.Verify();
}

void TestGrid() {
    std::printf("grid\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(r.m.Start(r.now, r.out));
    r.SkipVote();
    CHECK(!r.GridReady(ana)); // not yet
    r.LockAll();
    CHECK(r.P() == Phase::GridSync);
    CHECK(!r.GridReady(wim));
    ReadyToRace stale = r.Loaded();
    stale.raceIndex = static_cast<uint16_t>(stale.raceIndex + 1);
    CHECK(!r.m.GridReady(ana, stale, r.now, r.out)); // the answer to another race
    CHECK(r.m.GridReady(ana, r.Loaded(0, 1234), r.now, r.out));
    CHECK(!r.GridReady(ana));
    CHECK(r.m.LoadMs(ana) == 1234 && r.m.LoadMs(kHostId) == 0 && r.m.LoadMs(99) == 0);
    CHECK(r.P() == Phase::GridSync && Count(r.out, kMsgStart) == 0);
    CHECK(r.GridReady(kHostId)); // the last racer is in: the countdown starts
    CHECK(r.P() == Phase::Countdown);

    // The start is one message to everyone: this race, a little ahead on the host clock.
    CHECK(Count(r.out, kMsgStart) == 1);
    StartSignal start;
    const Outgoing* sent = r.Last(kMsgStart);
    CHECK(sent != nullptr && sent->to == kEveryone &&
          DecodeStart(sent->payload.data(), sent->payload.size(), start));
    CHECK(start.raceIndex == r.S().raceIndex && start.startAtMs == r.now + kStartLeadMs);
    CHECK(!r.m.HostLeftRace(r.now, r.out)); // no race to leave yet
    r.After(kCountdownMs);
    r.CapRace({ ana, ana, wim, 99, kHostId }); // repeats and strangers are dropped
    CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ ana, kHostId }));
    CHECK(!r.m.HostLeftRace(r.now, r.out)); // and none any more
    r.Verify();

    // The one racer still loading leaves: the countdown starts.
    Rig l;
    l.Add("Ana");
    l.Add("Bo");
    l.Begin();
    l.LockAll();
    CHECK(l.GridReady(0));
    CHECK(l.GridReady(1));
    l.m.Leave(2, l.now, l.out);
    CHECK(l.P() == Phase::Countdown && l.S().racers == 2);
    l.Verify();
}

void TestGridOrder() {
    std::printf("grid order\n");
    Rig r;
    const uint8_t ana = r.Add("Ana", Seat::Race, 7);
    const uint8_t bo = r.Add("Bo", Seat::Race, kNoMachine);
    r.Add("Wim", Seat::Watch);
    CHECK(r.m.LastGrid().humans.empty());
    r.Begin();
    CHECK(Count(r.out, kMsgGrid) == 0);
    CHECK(r.m.Lock(ana, LockOf(9, 2, 50), r.now, r.out));
    CHECK(r.m.Lock(bo, LockOf(12, 0, 50), r.now, r.out));
    CHECK(r.m.Lock(kHostId, LockOf(1, 3, 50), r.now, r.out));
    CHECK(r.P() == Phase::GridSync);

    // The first race of a series: the racers in id order, and CPUs in every slot they leave.
    const Grid first = r.m.LastGrid();
    CHECK(Ids(first) == (std::vector<uint8_t>{ kHostId, ana, bo }));
    CHECK(first.cpus == kGridSlots - 3 && first.raceIndex == r.S().raceIndex && first.rules == RaceRules());
    CHECK(first.humans[0].machine == 1 && first.humans[0].skin == 3);
    CHECK(first.humans[1].machine == 9 && first.humans[1].skin == 2);
    CHECK(first.humans[2].machine == 12 && first.humans[2].skin == 0);

    // It goes out once, to everyone, as it is kept here.
    CHECK(Count(r.out, kMsgGrid) == 1);
    Grid wire;
    const Outgoing* sent = r.Last(kMsgGrid);
    CHECK(sent != nullptr && sent->to == kEveryone && DecodeGrid(sent->payload.data(), sent->payload.size(), wire));
    CHECK(wire == first);

    // The next race: the last race's winner starts last of all, and a racer with no result ahead of the rest.
    r.GridAll();
    r.After(kCountdownMs);
    r.CapRace({ bo, kHostId, ana });
    const uint8_t cy = r.Add("Cy", Seat::Race, 20);
    r.After(kResultsMs);
    if (r.P() == Phase::Voting) {
        r.SkipVote(); // four racers now
    }
    CHECK(r.P() == Phase::Machine);
    r.LockAll();
    const Grid second = r.m.LastGrid();
    CHECK(Ids(second) == (std::vector<uint8_t>{ cy, ana, kHostId, bo }));
    CHECK(second.cpus == kGridSlots - 4 && second.raceIndex == first.raceIndex + 1);
    CHECK(second.seed != first.seed);
    CHECK(Count(r.out, kMsgGrid) == 2);
    r.Verify();

    // The same room and race give the same seed on any host; another room does not.
    Rig same;
    Rig other(Config(78));
    same.Begin();
    other.Begin();
    same.LockAll();
    other.LockAll();
    Rig again;
    again.Begin();
    again.LockAll();
    CHECK(same.m.LastGrid().seed == again.m.LastGrid().seed && same.m.LastGrid().seed != other.m.LastGrid().seed);
    CHECK(Ids(same.m.LastGrid()) == std::vector<uint8_t>{ kHostId } && same.m.LastGrid().cpus == kGridSlots - 1);

    // A room with the CPUs off sends none.
    RoomConfig bare = Config(77);
    bare.cpuFill = false;
    Rig n(bare);
    n.Add("Ana");
    n.Begin();
    n.LockAll();
    CHECK(n.m.LastGrid().cpus == 0 && n.m.LastGrid().humans.size() == 2);
    n.Verify();

    // The time runs out on the lock-in screen: the grid goes out with the racers who never locked too.
    Rig e;
    const uint8_t only = e.Add("Ana");
    e.Begin();
    CHECK(e.m.Lock(only, LockOf(1, 0, 50), e.now, e.out));
    e.After(kMachineMs);
    CHECK(e.P() == Phase::GridSync && Count(e.out, kMsgGrid) == 1);
    CHECK(Ids(e.m.LastGrid()) == (std::vector<uint8_t>{ kHostId, only }));
    e.Verify();
}

void TestTrackCheck() {
    std::printf("track check\n");

    // A racer whose track is not the host's watches the race; it still runs for the others.
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    r.Begin();
    r.LockAll();
    CHECK(r.GridReady(ana, 9));
    CHECK(r.GridReady(bo, 4));
    CHECK(r.P() == Phase::GridSync);
    CHECK(r.GridReady(kHostId, 4));
    CHECK(r.P() == Phase::Countdown);
    CHECK(r.OnGrid() == (std::vector<uint8_t>{ kHostId, bo }));
    CHECK(!r.Who(ana)->inRace && !r.Who(ana)->locked && !r.Who(ana)->watcher && r.S().racers == 2);
    r.After(kCountdownMs);
    r.CapRace({ bo, kHostId });
    CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ bo, kHostId }));
    r.Verify();

    // Without the host copy there is nobody to run the CPUs: no race, and the room moves on.
    Rig h;
    const uint8_t a = h.Add("Ana");
    h.Begin();
    h.LockAll();
    CHECK(h.GridReady(a));
    ExpectTimeout(h, Phase::GridSync, kGridMs, Phase::Results);
    CHECK(h.OnGrid().empty() && h.m.LastOrder().empty() && Count(h.out, kMsgStart) == 0);
    h.Verify();

    // Everyone but the host has another track: the host races alone.
    Rig o;
    const uint8_t x = o.Add("Ana");
    o.Begin();
    o.LockAll();
    CHECK(o.GridReady(kHostId, 1));
    CHECK(o.GridReady(x, 2));
    CHECK(o.P() == Phase::Countdown && o.OnGrid() == std::vector<uint8_t>{ kHostId });
    o.Verify();
}

void TestRules() {
    std::printf("rules\n");
    Rig r;
    r.Add("Ana");
    RaceRules rules;
    rules.laps = 5;
    rules.ignoreTrackSettings = true;
    rules.lavaNoKill = true;
    rules.magnetRange = 120.0f;
    rules.magnetStrength = 2.5f;

    RaceRules bad = rules;
    bad.laps = 0;
    CHECK(!r.m.SetRules(bad));
    bad = rules;
    bad.laps = static_cast<uint8_t>(kLapsMax + 1);
    CHECK(!r.m.SetRules(bad));
    bad = rules;
    bad.magnetRange = kMagnetRangeMax + 1.0f;
    CHECK(!r.m.SetRules(bad));
    bad = rules;
    bad.magnetStrength = kMagnetStrengthMin / 2.0f;
    CHECK(!r.m.SetRules(bad));
    CHECK(r.m.Rules() == RaceRules());

    CHECK(r.m.SetRules(rules)); // in the lobby
    r.Begin();
    rules.laps = 7;
    CHECK(r.m.SetRules(rules)); // on the lock-in screen
    r.LockAll();
    CHECK(r.m.LastGrid().rules == rules);

    // Once the grid is out the race keeps its rules.
    RaceRules late = rules;
    late.laps = 2;
    CHECK(!r.m.SetRules(late));
    r.GridAll();
    CHECK(r.P() == Phase::Countdown && !r.m.SetRules(late));
    r.After(kCountdownMs);
    CHECK(r.P() == Phase::Racing && !r.m.SetRules(late));
    r.CapRace({});
    CHECK(r.P() == Phase::Results && r.m.SetRules(late));
    CHECK(r.m.LastGrid().rules == rules); // the grid of the race that ran is not rewritten
    r.Verify();
}

void TestEmotes() {
    std::printf("preset messages\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(!r.m.Emote(ana, kEmoteCount, r.now, r.out));
    CHECK(!r.m.Emote(99, 0, r.now, r.out));
    const uint32_t start = r.now;
    for (uint32_t i = 0; i < kEmoteBurst; i++) {
        r.now = start + i * 10;
        CHECK(r.m.Emote(ana, static_cast<uint8_t>(i), r.now, r.out));
    }
    EmoteLine line;
    CHECK(r.out.back().to == kEveryone && r.out.back().type == kMsgEmoteLine);
    CHECK(DecodeEmoteLine(r.out.back().payload.data(), r.out.back().payload.size(), line));
    CHECK(line.from == ana && line.emote == kEmoteBurst - 1);

    const size_t sent = r.out.size();
    r.now = start + 60;
    CHECK(!r.m.Emote(ana, 0, r.now, r.out)); // over the limit
    CHECK(r.out.size() == sent);
    CHECK(r.m.Emote(wim, 2, r.now, r.out)); // the limit is per player, and a watcher may talk
    r.now = start + kEmoteWindowMs - 1;
    CHECK(!r.m.Emote(ana, 0, r.now, r.out));
    r.now = start + kEmoteWindowMs; // the first message leaves the window
    CHECK(r.m.Emote(ana, 0, r.now, r.out));
    CHECK(!r.m.Emote(ana, 0, r.now, r.out));
    for (uint8_t e = 0; e < kEmoteCount; e++) {
        CHECK(EmoteText(e)[0] != '\0');
    }
    CHECK(EmoteText(kEmoteCount)[0] == '\0');
    r.Verify();
}

void TestSeries() {
    std::printf("series\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    CHECK(r.m.SetLastPlacePicks(true, r.now, r.out));
    CHECK(!r.m.EndSeries(r.now, r.out));
    CHECK(r.m.Start(r.now, r.out));
    CHECK(!r.m.EndSeries(r.now, r.out));
    const uint8_t first = r.S().track;
    r.Race({ ana, kHostId });
    CHECK(r.m.EndSeries(r.now, r.out));
    CHECK(r.P() == Phase::Lobby && r.S().phaseTotalMs == 0);
    CHECK(r.S().raceIndex == 1 && r.S().lastTrack == first && r.S().track == kTrackNone);
    CHECK(r.OnGrid().empty() && !r.Who(ana)->locked && !r.Who(ana)->ready);
    r.After(3600000);
    CHECK(r.P() == Phase::Lobby);

    // On LAST PLACE PICKS a new series starts with a draw again, whoever finished last in the one before.
    CHECK(r.m.Start(r.now, r.out));
    CHECK(r.P() == Phase::Machine && r.m.LastPicked().drawn && r.m.LastPicked().by == kSystemId);
    CHECK(r.S().track != first);
    r.Verify();
}

// ---------------------------------------------------------------------------------------------------------
// How a race ends

// One row of a table.
bool Row(const RaceResults& t, size_t at, uint8_t id, ResultKind kind, uint32_t timeMs, uint8_t points) {
    return at < t.entries.size() && t.entries[at].id == id && t.entries[at].kind == kind &&
           t.entries[at].timeMs == timeMs && t.entries[at].points == points;
}

uint16_t Points(const Rig& r, uint8_t id) {
    const PlayerInfo* p = r.Who(id);
    return p != nullptr ? p->points : static_cast<uint16_t>(0xFFFF);
}

void TestFinishCheck() {
    std::printf("finish reports: the host's check\n");
    CHECK(PlacePoints(0) == 0 && PlacePoints(1) == 100 && PlacePoints(2) == 93 && PlacePoints(12) == 47 &&
          PlacePoints(13) == 0);

    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    r.Begin();
    r.LockAll();
    r.GridAll();
    // Nothing is taken before the race runs.
    CHECK(r.P() == Phase::Countdown && !r.Finish(ana, 1000) && !r.Retire(ana));
    r.After(kCountdownMs);
    CHECK(r.P() == Phase::Racing && r.m.LastGrid().rules.laps == 3);
    const uint32_t go = r.now;
    r.At(go + 90000);
    const auto report = [&r](uint8_t id, const FinishReport& f) { return r.m.ReportFinish(id, f, r.now, r.out); };
    const FinishReport done = r.Report(ResultKind::Finished, 90000);

    // Who may report, for which race, and what.
    r.m.NoteLap(ana, 3);
    r.m.NoteLap(wim, 3);
    CHECK(!report(wim, done)); // a watcher
    CHECK(!report(99, done));  // nobody
    FinishReport other = done;
    other.raceIndex++;
    CHECK(!report(ana, other)); // another race
    CHECK(!report(ana, r.Report(ResultKind::Unfinished, 0)));
    CHECK(!report(ana, r.Report(ResultKind::Finished, 0)));

    // The laps: the player's own poses must have named the last lap.
    r.m.NoteLap(ana, 2);
    CHECK(!report(ana, done));
    r.m.NoteLap(ana, 3);
    // The clock: no more than a second ahead of the host's own count of the race.
    CHECK(!report(ana, r.Report(ResultKind::Finished, 91001)));
    CHECK(Count(r.out, kMsgResults) == 0 && r.m.Results().entries.empty() && r.S().phaseTotalMs == kRaceCapMs);
    CHECK(report(ana, r.Report(ResultKind::Finished, 91000)));
    CHECK(!report(ana, done) && !r.Retire(ana)); // one result a race
    RaceResults t = r.Table();
    CHECK(!t.final && t.raceIndex == r.S().raceIndex && t.entries.size() == 3);
    CHECK(Row(t, 0, ana, ResultKind::Finished, 91000, 100));
    CHECK(Row(t, 1, kHostId, ResultKind::Unfinished, 0, 93) && Row(t, 2, bo, ResultKind::Unfinished, 0, 87));
    CHECK(r.P() == Phase::Racing && Points(r, ana) == 0); // points come with the final table

    // A time far behind the host's count is moved up to a second behind it.
    r.At(go + 100000);
    r.m.NoteLap(bo, 4); // over the line: the lap after the last
    CHECK(report(bo, r.Report(ResultKind::Finished, 50000)));
    t = r.Table();
    CHECK(!t.final && Row(t, 1, bo, ResultKind::Finished, 99000, 93));

    // A retirement needs no lap, and the last result ends the race.
    r.m.NoteLap(kHostId, 1);
    CHECK(r.Retire(kHostId));
    t = r.Table();
    CHECK(r.P() == Phase::Results && t.final && t.entries.size() == 3);
    CHECK(Row(t, 0, ana, ResultKind::Finished, 91000, 100) && Row(t, 1, bo, ResultKind::Finished, 99000, 93) &&
          Row(t, 2, kHostId, ResultKind::Retired, 0, 0));
    CHECK(!r.Retire(kHostId)); // the race is over
    r.Verify();

    // The first second of a race: nothing can be a second behind yet.
    Rig e;
    const uint8_t eve = e.Add("Eve");
    e.Begin();
    e.ToRacing();
    e.After(500);
    CHECK(!e.Finish(eve, 1501));
    CHECK(e.Finish(eve, 1));
    CHECK(Row(e.Table(), 0, eve, ResultKind::Finished, 1, 100));
    e.Verify();
}

void TestRaceEnd() {
    std::printf("the three ways a race ends\n");
    // Everyone is done: the results come at once.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 90000);
        CHECK(r.Finish(ana, 90000));
        r.At(r.go + 95000);
        CHECK(r.Finish(bo, 95000) && r.P() == Phase::Racing);
        r.At(r.go + 96000);
        CHECK(r.Retire(kHostId));
        CHECK(r.P() == Phase::Results && r.S().phaseTotalMs == kResultsMs && r.S().phaseLeftMs == kResultsMs);
        CHECK(r.Table().final && r.m.LastOrder() == (std::vector<uint8_t>{ ana, bo, kHostId }));
        CHECK(Points(r, ana) == 100 && Points(r, bo) == 93 && Points(r, kHostId) == 0);
        r.Verify();
    }
    // The winner plus sixty seconds.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 10000);
        CHECK(r.Retire(bo)); // a retirement starts no clock
        CHECK(r.S().phaseTotalMs == kRaceCapMs && r.S().phaseLeftMs == kRaceCapMs - 10000);
        r.At(r.go + 90000);
        CHECK(r.Finish(ana, 90000));
        CHECK(r.S().phaseTotalMs == 90000 + kAfterWinnerMs && r.S().phaseLeftMs == kAfterWinnerMs);
        // The players are told the shorter time.
        RoomState told;
        const Outgoing* room = r.Last(kMsgRoom);
        CHECK(room != nullptr && DecodeRoomState(room->payload.data(), room->payload.size(), told));
        CHECK(told.phase == Phase::Racing && told.phaseTotalMs == 90000 + kAfterWinnerMs &&
              told.phaseLeftMs == kAfterWinnerMs);
        r.At(r.go + 90000 + kAfterWinnerMs - 1);
        CHECK(r.P() == Phase::Racing && r.S().phaseLeftMs == 1 && !r.Table().final);
        r.At(r.go + 90000 + kAfterWinnerMs);
        CHECK(r.P() == Phase::Results);
        const RaceResults t = r.Table();
        CHECK(t.final && t.entries.size() == 3 && Row(t, 0, ana, ResultKind::Finished, 90000, 100) &&
              Row(t, 1, kHostId, ResultKind::Unfinished, 0, 93) && Row(t, 2, bo, ResultKind::Retired, 0, 0));
        CHECK(Points(r, ana) == 100 && Points(r, kHostId) == 93 && Points(r, bo) == 0);
        r.Verify();
    }
    // The second over the line moves nothing.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 90000);
        CHECK(r.Finish(ana, 90000));
        r.At(r.go + 100000);
        CHECK(r.Finish(bo, 100000) && r.S().phaseTotalMs == 90000 + kAfterWinnerMs);
        r.At(r.go + 90000 + kAfterWinnerMs - 1);
        CHECK(r.P() == Phase::Racing);
        r.At(r.go + 90000 + kAfterWinnerMs);
        CHECK(r.P() == Phase::Results && Points(r, kHostId) == 87);
    }
    // A winner in the last minute: the cap stays where it was.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        r.Begin();
        r.ToRacing();
        r.At(r.go + kRaceCapMs - kAfterWinnerMs + 5000);
        CHECK(r.Finish(ana, kRaceCapMs - kAfterWinnerMs + 5000) && r.S().phaseTotalMs == kRaceCapMs &&
              r.S().phaseLeftMs == kAfterWinnerMs - 5000);
        r.At(r.go + kRaceCapMs - 1);
        CHECK(r.P() == Phase::Racing);
        r.At(r.go + kRaceCapMs);
        CHECK(r.P() == Phase::Results);
    }
    // The room may wait less for the rest.
    {
        RoomConfig quick = Config(77);
        quick.afterWinnerMs = 5000;
        Rig r(quick);
        const uint8_t ana = r.Add("Ana");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 30000);
        CHECK(r.Finish(ana, 30000) && r.S().phaseTotalMs == 35000);
        r.At(r.go + 34999);
        CHECK(r.P() == Phase::Racing);
        r.At(r.go + 35000);
        CHECK(r.P() == Phase::Results);
    }
    // The 5:00 cap with nobody over the line: everyone is placed, by id while the game said nothing.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        ExpectTimeout(r, Phase::Racing, kRaceCapMs, Phase::Results);
        const RaceResults t = r.Table();
        CHECK(t.final && t.entries.size() == 3 && Count(r.out, kMsgResults) == 1);
        CHECK(Row(t, 0, kHostId, ResultKind::Unfinished, 0, 100) && Row(t, 1, ana, ResultKind::Unfinished, 0, 93) &&
              Row(t, 2, bo, ResultKind::Unfinished, 0, 87));
        CHECK(Points(r, kHostId) == 100 && Points(r, ana) == 93 && Points(r, bo) == 87);
        r.Verify();
    }
    // The same with the host game's order: strangers in it are passed over, and whoever it leaves out
    // comes after.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.m.SetStanding({ ana, bo, kHostId }); // not racing yet: not kept
        r.ToRacing();
        r.m.SetStanding({ bo, 99, ana });
        r.After(kRaceCapMs);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && Row(t, 0, bo, ResultKind::Unfinished, 0, 100) &&
              Row(t, 1, ana, ResultKind::Unfinished, 0, 93) && Row(t, 2, kHostId, ResultKind::Unfinished, 0, 87));
        CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ bo, ana, kHostId }));
        r.Verify();
    }
    // An order handed in before GO is of no race: it places nobody.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.LockAll();
        r.GridAll();
        CHECK(r.P() == Phase::Countdown);
        r.m.SetStanding({ bo, ana, kHostId });
        r.After(kCountdownMs);
        r.After(kRaceCapMs);
        CHECK(r.P() == Phase::Results && r.m.LastOrder() == (std::vector<uint8_t>{ kHostId, ana, bo }));
    }
    // The host's game leaves the race. Nothing ends: everyone is told once that the CPU machines are out,
    // and those still driving at the cap go by the lap of their latest pose, then by the last order the
    // host's game gave.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t cy = r.Add("Cy");
        CHECK(r.m.Start(r.now, r.out));
        if (r.P() == Phase::Voting) {
            r.SkipVote(); // four racers
        }
        r.LockAll();
        r.GridAll();
        CHECK(r.P() == Phase::Countdown && !r.m.HostLeftRace(r.now, r.out) && !r.S().cpusOut);
        r.After(kCountdownMs);
        CHECK(r.P() == Phase::Racing && !r.S().cpusOut);
        r.go = r.now;
        r.m.SetStanding({ cy, bo, ana, kHostId });
        r.m.NoteLap(ana, 2);
        r.m.NoteLap(bo, 1);
        r.m.NoteLap(cy, 1);
        r.At(r.go + 30000);
        const size_t rooms = Count(r.out, kMsgRoom);
        const size_t tables = Count(r.out, kMsgResults);
        CHECK(r.m.HostLeftRace(r.now, r.out));
        CHECK(Count(r.out, kMsgRoom) == rooms + 1 && Count(r.out, kMsgResults) == tables);
        RoomState told;
        const Outgoing* sent = r.Last(kMsgRoom);
        CHECK(sent != nullptr && sent->to == kEveryone &&
              DecodeRoomState(sent->payload.data(), sent->payload.size(), told));
        CHECK(told.cpusOut && told.phase == Phase::Racing && told.phaseTotalMs == kRaceCapMs &&
              told.phaseLeftMs == kRaceCapMs - 30000);
        CHECK(r.S().cpusOut && r.P() == Phase::Racing);
        CHECK(!r.m.HostLeftRace(r.now, r.out) && Count(r.out, kMsgRoom) == rooms + 1); // told once
        CHECK(r.Retire(kHostId)); // the host's own machine is out like anyone's who leaves
        CHECK(r.P() == Phase::Racing && !r.Table().final);
        r.At(r.go + kRaceCapMs);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && t.entries.size() == 4);
        CHECK(Row(t, 0, ana, ResultKind::Unfinished, 0, 100) && Row(t, 1, cy, ResultKind::Unfinished, 0, 93) &&
              Row(t, 2, bo, ResultKind::Unfinished, 0, 87) && Row(t, 3, kHostId, ResultKind::Retired, 0, 0));
        r.Verify();
        // The next race has its CPU machines again.
        r.After(kResultsMs);
        CHECK(r.P() != Phase::Results && !r.S().cpusOut);
        sent = r.Last(kMsgRoom);
        CHECK(sent != nullptr && DecodeRoomState(sent->payload.data(), sent->payload.size(), told) && !told.cpusOut);
    }
    // While the host's game is in the race its order stands, whatever the laps say: a lap is the coarser
    // of the two.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.m.SetStanding({ bo, kHostId, ana });
        r.m.NoteLap(ana, 2);
        r.m.NoteLap(bo, 1);
        r.After(kRaceCapMs);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && Row(t, 0, bo, ResultKind::Unfinished, 0, 100) &&
              Row(t, 1, kHostId, ResultKind::Unfinished, 0, 93) && Row(t, 2, ana, ResultKind::Unfinished, 0, 87));
        r.Verify();
    }
    // A finisher keeps the first place when the host's game has left, and a racer the old order never named
    // still goes by lap.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.m.SetStanding({ kHostId });
        r.At(r.go + 60000);
        CHECK(r.Finish(kHostId, 60000));
        CHECK(r.m.HostLeftRace(r.now, r.out));
        r.m.NoteLap(bo, 2);
        r.m.NoteLap(ana, 1);
        r.After(kAfterWinnerMs);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && Row(t, 0, kHostId, ResultKind::Finished, 60000, 100) &&
              Row(t, 1, bo, ResultKind::Unfinished, 0, 93) && Row(t, 2, ana, ResultKind::Unfinished, 0, 87));
        r.Verify();
    }
}

void TestResultsOrder() {
    std::printf("the order of the table\n");
    // Finishers by time, not by the order their reports came in; the same time, by that order.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t cy = r.Add("Cy");
        CHECK(r.m.Start(r.now, r.out));
        r.SkipVote();
        r.ToRacing();
        r.At(r.go + 90500);
        CHECK(r.Finish(ana, 91000));
        r.At(r.go + 90900);
        CHECK(r.Finish(bo, 90200));
        RaceResults t = r.Table();
        CHECK(!t.final && t.entries.size() == 4 && Row(t, 0, bo, ResultKind::Finished, 90200, 100) &&
              Row(t, 1, ana, ResultKind::Finished, 91000, 93));
        r.At(r.go + 91000);
        CHECK(r.Finish(cy, 91000));
        t = r.Table();
        CHECK(!t.final && Row(t, 1, ana, ResultKind::Finished, 91000, 93) &&
              Row(t, 2, cy, ResultKind::Finished, 91000, 87) && Row(t, 3, kHostId, ResultKind::Unfinished, 0, 81));
        CHECK(r.Retire(kHostId));
        t = r.Table();
        CHECK(t.final && Row(t, 3, kHostId, ResultKind::Retired, 0, 0));
        CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ bo, ana, cy, kHostId }));
        r.Verify();
    }
    // A retired player ranks below everyone who did not retire (D13), and the one who lasted longer
    // above the one who went out first.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t cy = r.Add("Cy");
        CHECK(r.m.Start(r.now, r.out));
        r.SkipVote();
        r.ToRacing();
        r.At(r.go + 10000);
        CHECK(r.Retire(bo));
        RaceResults t = r.Table();
        CHECK(!t.final && t.entries.size() == 4 && Row(t, 3, bo, ResultKind::Retired, 0, 0) &&
              Row(t, 0, kHostId, ResultKind::Unfinished, 0, 100));
        r.At(r.go + 20000);
        CHECK(r.Retire(cy));
        r.At(r.go + 90000);
        CHECK(r.Finish(ana, 90000));
        r.After(kAfterWinnerMs);
        t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && t.entries.size() == 4);
        CHECK(Row(t, 0, ana, ResultKind::Finished, 90000, 100) && Row(t, 1, kHostId, ResultKind::Unfinished, 0, 93) &&
              Row(t, 2, cy, ResultKind::Retired, 0, 0) && Row(t, 3, bo, ResultKind::Retired, 0, 0));
        CHECK(r.m.LastOrder() == (std::vector<uint8_t>{ ana, kHostId, cy, bo }));
        r.Verify();
    }
    // Only those on the grid are in it: not a watcher, and not a racer whose track was another one.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t wim = r.Add("Wim", Seat::Watch);
        r.Begin();
        r.LockAll();
        CHECK(r.GridReady(kHostId) && r.GridReady(ana) && r.GridReady(bo, 9));
        r.After(kCountdownMs);
        CHECK(r.P() == Phase::Racing && !r.Who(bo)->inRace);
        CHECK(!r.Finish(bo, 1000) && !r.Retire(bo) && !r.Retire(wim));
        r.At(r.now + 50000);
        CHECK(r.Finish(ana, 50000) && r.Retire(kHostId));
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && t.entries.size() == 2 && Points(r, bo) == 0 &&
              Points(r, wim) == 0);
        r.Verify();
    }
}

void TestRaceLeaveAndJoin() {
    std::printf("leaving and joining while a race runs\n");
    // The one the race waited for leaves: it is over.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 40000);
        CHECK(r.Finish(ana, 40000) && r.Retire(kHostId) && r.P() == Phase::Racing);
        r.m.Leave(bo, r.now, r.out);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Results && t.final && t.entries.size() == 2 &&
              Row(t, 0, ana, ResultKind::Finished, 40000, 100) && Row(t, 1, kHostId, ResultKind::Retired, 0, 0));
        r.Verify();
    }
    // A racer whose result is in leaves while others drive: the table goes out again without them. The
    // sixty seconds their finish started keep running.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 40000);
        CHECK(r.Finish(ana, 40000) && Count(r.out, kMsgResults) == 1);
        r.m.Leave(ana, r.now, r.out);
        const RaceResults t = r.Table();
        CHECK(r.P() == Phase::Racing && Count(r.out, kMsgResults) == 2 && !t.final && t.entries.size() == 2);
        CHECK(Row(t, 0, kHostId, ResultKind::Unfinished, 0, 100) && Row(t, 1, bo, ResultKind::Unfinished, 0, 93));
        CHECK(r.S().phaseTotalMs == 40000 + kAfterWinnerMs);
        r.Verify();
    }
    // Before any result nobody has a table, so none is sent: not for a racer who leaves, not for a
    // watcher who does, and not to someone who joins.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t wim = r.Add("Wim", Seat::Watch);
        r.Begin();
        r.ToRacing();
        r.m.Leave(bo, r.now, r.out);
        r.m.Leave(wim, r.now, r.out);
        const uint8_t dee = r.Add("Dee", Seat::Watch);
        CHECK(r.P() == Phase::Racing && Count(r.out, kMsgResults) == 0);

        // Once there is one, whoever joins is given it: during the race and after it.
        r.At(r.go + 40000);
        CHECK(r.Finish(ana, 40000) && Count(r.out, kMsgResults) == 1);
        const uint8_t eli = r.Add("Eli", Seat::Watch);
        const Outgoing* sent = r.Last(kMsgResults);
        RaceResults got;
        CHECK(Count(r.out, kMsgResults) == 2 && sent != nullptr && sent->to == eli &&
              DecodeResults(sent->payload.data(), sent->payload.size(), got) && got == r.m.Results() && !got.final);
        CHECK(r.Retire(kHostId) && r.P() == Phase::Results);
        const uint8_t fay = r.Add("Fay");
        sent = r.Last(kMsgResults);
        CHECK(sent != nullptr && sent->to == fay &&
              DecodeResults(sent->payload.data(), sent->payload.size(), got) && got == r.m.Results() && got.final);
        CHECK(got.entries.size() == 2 && Points(r, fay) == 0 && Points(r, dee) == 0);
        r.Verify();
    }
}

// A record Machine Create could have made (gdx_net_lobby_tests.cpp has the bounds).
MachineRecord RecordOf(uint8_t fill) {
    MachineRecord record{};
    for (size_t i = 0; i < record.size(); i++) {
        record[i] = static_cast<uint8_t>(fill ^ i);
    }
    record[0x00] = 1;
    record[0x01] = 2;
    record[0x02] = 0;
    record[0x03] = static_cast<uint8_t>(fill % 7);
    record[0x04] = 3;
    record[0x05] = 6;
    record[0x06] = 8;
    record[0x08] = 5;
    record[0x15 + 8] = 0;
    return record;
}

const GridEntry* EntryOf(const Grid& grid, uint8_t id) {
    for (const GridEntry& e : grid.humans) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

void TestMachineRecord() {
    std::printf("machine record (O4.5)\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    r.Begin();

    // The host's own lock never passes a decoder, so the room checks the record itself.
    LockRequest bad = LockOf(1, 0, 50);
    bad.hasRecord = true;
    bad.record = RecordOf(1);
    bad.record[0x00] = 5;
    CHECK(!r.m.Lock(kHostId, bad, r.now, r.out));
    CHECK(!r.Who(kHostId)->locked && r.S().locked == 0);

    LockRequest mine = LockOf(5, 2, 60);
    mine.hasRecord = true;
    mine.record = RecordOf(4);
    CHECK(r.m.Lock(ana, mine, r.now, r.out));
    CHECK(r.m.Lock(bo, LockOf(9, 0, 50), r.now, r.out));
    CHECK(r.m.Lock(kHostId, LockOf(1, 0, 50), r.now, r.out));
    CHECK(r.P() == Phase::GridSync);

    // The record travels in the grid with its racer, and only with them.
    const Grid first = r.m.LastGrid();
    const GridEntry* a = EntryOf(first, ana);
    const GridEntry* b = EntryOf(first, bo);
    const GridEntry* h = EntryOf(first, kHostId);
    CHECK(a != nullptr && a->hasRecord && a->record == mine.record && a->machine == 5);
    CHECK(b != nullptr && !b->hasRecord && b->record == MachineRecord{});
    CHECK(h != nullptr && !h->hasRecord);
    Grid wire;
    const Outgoing* sent = r.Last(kMsgGrid);
    CHECK(sent != nullptr && DecodeGrid(sent->payload.data(), sent->payload.size(), wire) && wire == first);

    // The next race starts with no record: a racer brings theirs again on its lock-in screen. One who lets
    // the screen time out keeps the machine of their last lock, and races its stock version.
    r.GridAll();
    r.After(kCountdownMs);
    r.CapRace({ ana, bo, kHostId });
    r.NextRace();
    CHECK(r.m.Lock(bo, LockOf(9, 0, 50), r.now, r.out));
    CHECK(r.m.Lock(kHostId, LockOf(1, 0, 50), r.now, r.out));
    r.After(kMachineMs);
    CHECK(r.P() == Phase::GridSync);
    const Grid second = r.m.LastGrid();
    CHECK(second.raceIndex != first.raceIndex && second.humans.size() == 3);
    const GridEntry* late = EntryOf(second, ana);
    CHECK(late != nullptr && late->machine == 5);
    for (const GridEntry& e : second.humans) {
        CHECK(!e.hasRecord && e.record == MachineRecord{});
    }
    r.Verify();
}

void TestSeriesPoints() {
    std::printf("series points\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    r.Begin();
    r.ToRacing();
    r.At(r.go + 80000);
    CHECK(r.Finish(ana, 80000));
    r.At(r.go + 81000);
    CHECK(r.Finish(bo, 81000) && r.Retire(kHostId));
    CHECK(Points(r, ana) == 100 && Points(r, bo) == 93 && Points(r, kHostId) == 0);
    // Every player is told: the roster carries the points.
    std::vector<PlayerInfo> told;
    const Outgoing* roster = r.Last(kMsgRoster);
    CHECK(roster != nullptr && DecodeRoster(roster->payload.data(), roster->payload.size(), told) &&
          told == r.m.Roster());

    // The next race adds to them, and its table is that race's alone.
    r.NextRace();
    CHECK(r.m.Results().raceIndex + 1 == r.S().raceIndex); // the last table is still the last race's
    r.ToRacing();
    r.At(r.go + 70000);
    r.m.SetStanding({ bo, ana, kHostId }); // of this race alone: the next one starts without an order
    // The laps of the last race are not laps of this one.
    CHECK(!r.m.ReportFinish(bo, r.Report(ResultKind::Finished, 70000), r.now, r.out));
    CHECK(r.Finish(bo, 70000));
    r.At(r.go + 75000);
    CHECK(r.Finish(kHostId, 75000) && r.Retire(ana));
    const RaceResults second = r.Table();
    CHECK(second.final && second.raceIndex == r.S().raceIndex && Row(second, 0, bo, ResultKind::Finished, 70000, 100));
    CHECK(Points(r, ana) == 100 && Points(r, bo) == 193 && Points(r, kHostId) == 93);

    // Someone who joins mid-series starts at nothing and scores from the next race.
    const uint8_t cy = r.Add("Cy");
    r.NextRace();
    r.ToRacing();
    r.At(r.go + 60000);
    CHECK(r.Finish(cy, 60000));
    r.After(kAfterWinnerMs);
    CHECK(r.P() == Phase::Results);
    CHECK(Points(r, cy) == 100 && Points(r, kHostId) == 93 + 93 && Points(r, ana) == 100 + 87 &&
          Points(r, bo) == 193 + 81);

    // Back in the lobby the points of the series stay up; the next series starts from nothing.
    CHECK(r.m.EndSeries(r.now, r.out));
    CHECK(r.P() == Phase::Lobby && Points(r, bo) == 274 && Points(r, cy) == 100);
    CHECK(r.m.Start(r.now, r.out));
    for (const PlayerInfo& p : r.m.Roster()) {
        CHECK(p.points == 0);
    }
    r.Verify();
}

} // namespace

// A ballot or pick for a track of the player's own: the id is the content hash, the title for display.
TrackChoice CustomChoice(uint8_t seed) {
    TrackChoice c;
    c.track = kTrackCustom;
    for (size_t i = 0; i < c.custom.id.size(); i++) {
        c.custom.id[i] = static_cast<uint8_t>(seed + i);
    }
    c.custom.title = "Own track";
    return c;
}

// Three racers on LAST PLACE PICKS through one series race, then last place (Ana) is the picker. Custom
// tracks are on.
struct CustomRig : Rig {
    uint8_t ana = kSystemId;
    uint8_t bo = kSystemId;
    uint8_t raced = kTrackNone;

    CustomRig() : CustomRig(Config(77)) {
    }

    explicit CustomRig(const RoomConfig& config) : Rig(config) {
        ana = Add("Ana");
        bo = Add("Bo");
        CHECK(m.SetLastPlacePicks(true, now, out));
        CHECK(m.Start(now, out));
        raced = S().track;
        Race({ bo, kHostId, ana });
        After(kResultsMs);
        CHECK(P() == Phase::Pick && S().picker == ana);
        CHECK(m.SetCustomTracks(true, now, out));
    }

    // Ana picks her own track and every racer locks in.
    void PickAndLock() {
        CHECK(m.Pick(ana, CustomChoice(1), now, out));
        CHECK(P() == Phase::Machine);
        LockAll();
    }
};

void TestCustomTrack() {
    std::printf("custom track\n");

    // The switch is off: a pick may not name a custom track, and the byte form never can.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        CHECK(r.m.SetLastPlacePicks(true, r.now, r.out));
        r.Begin();
        r.Race({ bo, kHostId, ana });
        r.After(kResultsMs);
        CHECK(r.P() == Phase::Pick && r.S().picker == ana);
        CHECK(!r.m.Pick(ana, CustomChoice(1), r.now, r.out));
        CHECK(r.m.SetCustomTracks(true, r.now, r.out));
        CHECK(!r.m.Pick(ana, kTrackCustom, r.now, r.out)); // a custom pick needs its id and title
        TrackChoice untitled = CustomChoice(1);
        untitled.custom.title.clear();
        CHECK(!r.m.Pick(ana, untitled, r.now, r.out));
        CHECK(r.P() == Phase::Pick);
    }

    // A custom pick: the room names it, its owner holds it and nobody else does.
    {
        CustomRig r;
        CHECK(r.m.Pick(r.ana, CustomChoice(1), r.now, r.out));
        CHECK(r.P() == Phase::Machine);
        CHECK(r.S().track == kTrackCustom && r.S().custom == CustomChoice(1).custom);
        CHECK(r.m.CurrentTrack() == CustomChoice(1));
        CHECK(r.m.TrackOwner() == r.ana);
        CHECK(r.m.HoldsTrack(r.ana) && !r.m.HoldsTrack(kHostId) && !r.m.HoldsTrack(r.bo));
        CHECK(r.m.LastPicked().track == kTrackCustom && r.m.LastPicked().by == r.ana);
        r.Verify();
    }

    // Every racer locked while the host and Bo lack it: ASSETS waits its full length, then Bo watches.
    {
        CustomRig r;
        r.PickAndLock();
        CHECK(r.P() == Phase::Assets);
        CHECK(Count(r.out, kMsgGrid) == 1); // only the first race's
        CHECK(!r.m.SetRules(r.m.Rules())); // the rules froze with the locks
        CHECK(r.m.TrackHeld(kHostId, r.now, r.out));
        CHECK(r.P() == Phase::Assets); // Bo still lacks it
        const uint32_t start = r.now;
        CHECK(r.S().phaseTotalMs == kAssetsMs);
        r.At(start + kAssetsMs - 1);
        CHECK(r.P() == Phase::Assets);
        r.At(start + kAssetsMs);
        CHECK(r.P() == Phase::GridSync);
        CHECK(!r.Who(r.bo)->inRace && !r.Who(r.bo)->locked);
        CHECK(r.Who(r.ana)->inRace && r.Who(kHostId)->inRace);
        CHECK(r.m.LastGrid().track == CustomChoice(1));
        CHECK(Ids(r.m.LastGrid()).size() == 2);
        CHECK(!r.m.TrackHeld(r.bo, r.now, r.out)); // too late: the grid is out
        r.Verify();
    }

    // Everyone reports it before the deadline: the grid goes out at once.
    {
        CustomRig r;
        r.PickAndLock();
        CHECK(r.P() == Phase::Assets);
        CHECK(r.m.TrackHeld(r.bo, r.now, r.out));
        CHECK(r.P() == Phase::Assets);
        CHECK(r.m.TrackHeld(kHostId, r.now, r.out));
        CHECK(r.P() == Phase::GridSync);
        CHECK(r.m.LastGrid().track == CustomChoice(1));
        CHECK(r.OnGrid().size() == 3);
        CHECK(!r.m.TrackFailed(r.now, r.out)); // the grid is out: too late to fall back
        CHECK(r.S().track == kTrackCustom);
    }

    // The deadline passes and the host never got it: the room races a stock track, everyone on the grid.
    {
        CustomRig r;
        r.PickAndLock();
        CHECK(r.m.TrackHeld(r.bo, r.now, r.out));
        r.After(kAssetsMs);
        CHECK(r.P() == Phase::GridSync);
        CHECK(r.S().track < kStockTracks && r.S().track != r.raced);
        CHECK(r.m.LastGrid().track.track == r.S().track && r.m.LastGrid().track.custom == CustomTrack{});
        CHECK(r.m.TrackOwner() == kSystemId && !r.m.HoldsTrack(r.ana));
        CHECK(r.OnGrid().size() == 3);
        for (uint8_t id : r.OnGrid()) {
            CHECK(r.Who(id)->locked); // the locks stand through the fallback
        }
        r.Verify();
    }

    // The owner leaves during ASSETS before the host holds it: fallback with a fresh grid of those left.
    {
        CustomRig r;
        r.PickAndLock();
        r.m.Leave(r.ana, r.now, r.out);
        CHECK(r.P() == Phase::GridSync);
        CHECK(r.S().track < kStockTracks && r.S().track != r.raced);
        CHECK(Ids(r.m.LastGrid()).size() == 2 && r.m.LastGrid().track.track < kStockTracks);
    }

    // The owner leaves once the host holds it: the transfer goes on from the host, and Bo is the one left.
    {
        CustomRig r;
        r.PickAndLock();
        CHECK(r.m.TrackHeld(kHostId, r.now, r.out));
        r.m.Leave(r.ana, r.now, r.out);
        CHECK(r.P() == Phase::Assets && r.S().track == kTrackCustom);
        CHECK(r.m.TrackHeld(r.bo, r.now, r.out));
        CHECK(r.P() == Phase::GridSync && r.m.LastGrid().track == CustomChoice(1));
    }

    // The owner leaves on the lock-in screen while the host lacks it: the room draws at once and stays there.
    {
        CustomRig r;
        CHECK(r.m.Pick(r.ana, CustomChoice(1), r.now, r.out));
        r.m.Leave(r.ana, r.now, r.out);
        CHECK(r.P() == Phase::Machine && r.S().track < kStockTracks && r.S().track != r.raced);
        r.LockAll();
        CHECK(r.P() == Phase::GridSync && r.m.LastGrid().track.track == r.S().track);
    }

    // The host's flow cannot get it: TrackFailed falls back on the lock-in screen, and later locks skip ASSETS.
    {
        CustomRig r;
        CHECK(!r.m.TrackFailed(r.now, r.out)); // nothing to fail on a pick screen
        CHECK(r.m.Pick(r.ana, CustomChoice(1), r.now, r.out));
        CHECK(r.m.TrackFailed(r.now, r.out));
        CHECK(r.P() == Phase::Machine && r.S().track < kStockTracks && r.S().track != r.raced);
        CHECK(!r.m.TrackFailed(r.now, r.out)); // a stock track cannot fail
        CHECK(!r.m.TrackHeld(r.ana, r.now, r.out));
        r.LockAll();
        CHECK(r.P() == Phase::GridSync);
    }

    // TrackFailed during ASSETS sends the grid with the stock track at once.
    {
        CustomRig r;
        r.PickAndLock();
        CHECK(r.P() == Phase::Assets);
        CHECK(r.m.TrackFailed(r.now, r.out));
        CHECK(r.P() == Phase::GridSync && r.m.LastGrid().track.track < kStockTracks);
        CHECK(r.OnGrid().size() == 3);
    }

    // The fallback never draws the track just raced, whatever the room.
    for (uint32_t room = 1; room <= 200; room++) {
        CustomRig r(Config(room));
        CHECK(r.m.Pick(r.ana, CustomChoice(uint8_t(room)), r.now, r.out));
        CHECK(r.m.TrackFailed(r.now, r.out));
        CHECK(r.S().track < kStockTracks && r.S().track != r.raced);
    }

    // A custom ballot: the winner's ballot names the track and its owner.
    {
        Rig r;
        r.Add("Ana");
        r.Add("Bo");
        r.Add("Cy");
        CHECK(r.m.Start(r.now, r.out));
        CHECK(r.P() == Phase::Voting);
        CHECK(!r.m.Vote(1, CustomChoice(5), r.now, r.out)); // the switch is off
        CHECK(r.m.SetCustomTracks(true, r.now, r.out));
        CHECK(r.m.Vote(1, CustomChoice(5), r.now, r.out));
        CHECK(r.m.Vote(2, CustomChoice(6), r.now, r.out));
        CHECK(r.m.Vote(3, CustomChoice(7), r.now, r.out));
        CHECK(r.m.Vote(kHostId, CustomChoice(8), r.now, r.out));
        CHECK(r.P() == Phase::Lottery);
        const LotteryResult& result = r.m.Lottery();
        CHECK(result.track == kTrackCustom && result.winner < result.ballots.size());
        const uint8_t owner = result.ballots[result.winner].id;
        CHECK(r.m.TrackOwner() == owner && r.m.HoldsTrack(owner));
        CHECK(r.m.CurrentTrack().custom == result.ballots[result.winner].custom);
        CHECK(r.m.SkipLottery(r.now, r.out));
        CHECK(r.P() == Phase::Machine && r.S().track == kTrackCustom);
        r.Verify();
    }
}

void TestSuccessorState() {
    std::printf("successor and CPU settings in room\n");
    Rig r;
    // Nobody can take over until a member has an address the host can give.
    CHECK(r.m.Successor() == kSystemId && r.S().successor == kSystemId);
    const uint8_t ana = r.Add("Ana");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(r.m.Successor() == kSystemId);
    r.m.SetAddress(kHostId, MappedV4(1), 1); // the host is never its own successor
    r.m.SetAddress(9, MappedV4(1), 1);       // nobody sits there
    r.m.SetAddress(wim, MappedV4(0x0A000003), 4000);
    r.out.clear();
    r.After(1);
    // The watcher is the only member with an address, so it is next (it races from the next grid, D28.3).
    CHECK(r.m.Successor() == wim);
    RoomState told;
    const Outgoing* room = r.Last(kMsgRoom);
    CHECK(room != nullptr && DecodeRoomState(room->payload.data(), room->payload.size(), told));
    CHECK(told.successor == wim && told.successorIp == MappedV4(0x0A000003) && told.successorPort == 4000);
    // A racer comes before any watcher.
    r.m.SetAddress(ana, MappedV4(0x0A000002), 4001);
    r.out.clear();
    r.After(1);
    room = r.Last(kMsgRoom);
    CHECK(r.m.Successor() == ana && room != nullptr &&
          DecodeRoomState(room->payload.data(), room->payload.size(), told) && told.successor == ana &&
          told.successorPort == 4001);
    // Join order, not id order: Ana leaves, and the newcomer who gets her id joined after Bo.
    const uint8_t bo = r.Add("Bo");
    r.m.SetAddress(bo, MappedV4(0x0A000004), 4002);
    r.m.Leave(ana, r.now, r.out);
    const uint8_t cy = r.Add("Cy");
    CHECK(cy == ana && cy < bo);
    r.m.SetAddress(cy, MappedV4(0x0A000005), 4003);
    r.out.clear();
    r.After(1);
    room = r.Last(kMsgRoom);
    CHECK(r.m.Successor() == bo && room != nullptr &&
          DecodeRoomState(room->payload.data(), room->payload.size(), told) && told.successor == bo &&
          told.successorIp == MappedV4(0x0A000004) && told.successorPort == 4002);
    // Nothing changed, so nothing is sent.
    r.out.clear();
    r.After(1);
    CHECK(Count(r.out, kMsgRoom) == 0);
    // The address of a seat goes with the player: a newcomer on a freed id has none until it is set.
    r.m.Leave(cy, r.now, r.out);
    const uint8_t dee = r.Add("Dee");
    CHECK(dee == cy);
    r.m.Leave(bo, r.now, r.out);
    r.out.clear();
    r.After(1);
    CHECK(r.m.Successor() == wim);
    // Everyone with an address leaves: nobody can take over, and the room says so.
    r.out.clear();
    r.m.Leave(wim, r.now, r.out);
    r.After(1);
    room = r.Last(kMsgRoom);
    CHECK(r.m.Successor() == kSystemId && room != nullptr &&
          DecodeRoomState(room->payload.data(), room->payload.size(), told) && told.successor == kSystemId &&
          IsZeroIp(told.successorIp) && told.successorPort == 0);
    r.Verify();

    // The CPU settings travel in room (decision 24).
    Rig c;
    CHECK(c.m.Cpu() == CpuOptions() && c.S().cpuFill && c.S().aiAggression == kAiAggressionDefault &&
          !c.S().cpuTrackHazards);
    CpuOptions cpu;
    cpu.fill = false;
    cpu.aiAggression = 300;
    cpu.trackHazards = true;
    CpuOptions bad = cpu;
    bad.aiAggression = static_cast<uint16_t>(kAiAggressionMin - 1);
    CHECK(!c.m.SetCpuOptions(bad, c.now, c.out));
    bad.aiAggression = static_cast<uint16_t>(kAiAggressionMax + 1);
    CHECK(!c.m.SetCpuOptions(bad, c.now, c.out));
    CHECK(c.m.Cpu() == CpuOptions());
    c.out.clear();
    CHECK(c.m.SetCpuOptions(cpu, c.now, c.out));
    room = c.Last(kMsgRoom);
    CHECK(room != nullptr && DecodeRoomState(room->payload.data(), room->payload.size(), told) && !told.cpuFill &&
          told.aiAggression == 300 && told.cpuTrackHazards);
    c.out.clear();
    CHECK(c.m.SetCpuOptions(cpu, c.now, c.out) && Count(c.out, kMsgRoom) == 0);
    // So do the rules, before any grid.
    RaceRules rules;
    rules.laps = 9;
    rules.lavaNoKill = true;
    CHECK(c.m.SetRules(rules));
    c.out.clear();
    c.After(1);
    room = c.Last(kMsgRoom);
    CHECK(room != nullptr && DecodeRoomState(room->payload.data(), room->payload.size(), told) &&
          told.rules == rules);
    c.out.clear();
    CHECK(c.m.SetRules(rules));
    c.After(1);
    CHECK(Count(c.out, kMsgRoom) == 0);
    // Without CPU fill the grid has the humans alone.
    c.Add("Ana");
    c.Begin();
    c.LockAll();
    CHECK(c.m.LastGrid().cpus == 0 && c.m.LastGrid().humans.size() == 2);
    // Once the grid is out, the CPU settings stay as they are.
    CHECK(!c.m.SetCpuOptions(CpuOptions(), c.now, c.out) && c.m.Cpu() == cpu);
    c.Verify();

    // The room's starting value comes from its config.
    RoomConfig off = Config(5);
    off.cpuFill = false;
    Rig d(off);
    CHECK(!d.m.Cpu().fill && !d.S().cpuFill);
}

RoomMirror MirrorOf(const Rig& r) {
    RoomMirror mirror;
    mirror.roster = r.m.Roster();
    mirror.room = r.S();
    mirror.results = r.m.Results();
    mirror.haveResults = !r.m.Results().entries.empty();
    return mirror;
}

const PlayerInfo* NamedIn(const std::vector<PlayerInfo>& roster, const std::string& name) {
    for (const PlayerInfo& p : roster) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

void TestPromote() {
    std::printf("a member's mirror promoted to host (D28.3)\n");
    // The remap: the successor becomes 0, the old host is gone, everyone else keeps their id.
    CHECK(HandoverId(3, 3) == kHostId && HandoverId(kHostId, 3) == kSystemId && HandoverId(2, 3) == 2);

    // After a final table: the results page runs out, then last place picks, under the same room id.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        const uint8_t wim = r.Add("Wim", Seat::Watch);
        r.m.SetAddress(ana, MappedV4(0x0A000001), 4001);
        CHECK(r.m.SetLastPlacePicks(true, r.now, r.out)); // the new host keeps it: last place picks below
        r.Begin();
        r.ToRacing();
        r.At(r.go + 80000);
        CHECK(r.Finish(ana, 80000));
        r.At(r.go + 81000);
        CHECK(r.Finish(bo, 81000) && r.Retire(kHostId));
        CHECK(r.P() == Phase::Results && Points(r, ana) == 100 && Points(r, bo) == 93);
        CHECK(r.m.Successor() == ana);
        r.After(2000);
        const RoomMirror mirror = MirrorOf(r);

        CHECK(!RoomMachine::Promote(mirror, kHostId, r.now).has_value());
        CHECK(!RoomMachine::Promote(mirror, kSystemId, r.now).has_value());
        CHECK(!RoomMachine::Promote(mirror, 9, r.now).has_value());
        std::optional<RoomMachine> promoted = RoomMachine::Promote(mirror, ana, r.now);
        CHECK(promoted.has_value());
        if (!promoted.has_value()) {
            return;
        }
        Rig q;
        q.m = std::move(*promoted);
        q.now = r.now;
        const std::vector<PlayerInfo>& roster = q.m.Roster();
        CHECK(roster.size() == 3 && NamedIn(roster, "Host") == nullptr);
        CHECK(q.Who(kHostId) != nullptr && q.Who(kHostId)->name == "Ana" && q.Who(kHostId)->host &&
              q.Who(kHostId)->points == 100 && !q.Who(kHostId)->watcher);
        CHECK(q.Who(ana) == nullptr);
        CHECK(q.Who(bo) != nullptr && q.Who(bo)->name == "Bo" && q.Who(bo)->points == 93 && !q.Who(bo)->host);
        CHECK(q.Who(wim) != nullptr && q.Who(wim)->watcher);
        const RoomState s = q.S();
        CHECK(s.roomId == 77 && s.raceIndex == mirror.room.raceIndex && s.phase == Phase::Results);
        CHECK(s.phaseLeftMs == kResultsMs - 2000);
        // The table moves with the ids: the first row is now player 0's, the old host's row is gone.
        CHECK(q.m.Results().final && q.m.Results().entries.size() == 2);
        CHECK(q.m.Results().entries.size() == 2 && q.m.Results().entries[0].id == kHostId &&
              q.m.Results().entries[1].id == bo);
        // Nobody has an address in the new room until they come back.
        CHECK(q.m.Successor() == kSystemId);
        q.m.SetAddress(bo, MappedV4(0x0A000002), 4002);
        CHECK(q.m.Successor() == bo);
        q.After(kResultsMs - 2000);
        CHECK(q.P() == Phase::Pick && q.S().picker == bo && q.S().raceIndex == mirror.room.raceIndex + 1);
        q.Verify();
    }

    // A crash mid-race: that race scores nothing, the next one is chosen at once under a new race index.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        const uint8_t bo = r.Add("Bo");
        r.Begin();
        r.ToRacing();
        r.At(r.go + 30000);
        const RoomMirror mirror = MirrorOf(r);
        CHECK(mirror.room.phase == Phase::Racing);
        std::optional<RoomMachine> promoted = RoomMachine::Promote(mirror, bo, r.now);
        CHECK(promoted.has_value());
        if (!promoted.has_value()) {
            return;
        }
        Rig q;
        q.m = std::move(*promoted);
        q.now = r.now;
        // The room votes, as it did before: the next race's vote opens at once.
        CHECK(q.P() == Phase::Voting && q.S().raceIndex == mirror.room.raceIndex + 1 && !q.S().lastPlacePicks);
        q.SkipVote();
        CHECK(q.Who(kHostId)->name == "Bo" && q.Who(ana) != nullptr && q.Who(bo) == nullptr);
        CHECK(q.Who(kHostId)->points == 0 && q.Who(ana)->points == 0);
        // The new host is on the grid of the next race, with Ana.
        CHECK(q.OnGrid() == std::vector<uint8_t>({ kHostId, ana }));
        q.Verify();
    }

    // Mid-vote: the vote starts over with the same race index; the ballots cast before are gone.
    {
        Rig r;
        const uint8_t a = r.Add("A");
        r.Add("B");
        const uint8_t c = r.Add("C");
        r.Add("D"); // four racers stay once the old host is gone, so there is a vote
        CHECK(r.m.Start(r.now, r.out));
        CHECK(r.P() == Phase::Voting);
        CHECK(r.m.Vote(a, 1, r.now, r.out));
        std::optional<RoomMachine> promoted = RoomMachine::Promote(MirrorOf(r), c, r.now + 5000);
        CHECK(promoted.has_value() && promoted->GetPhase() == Phase::Voting && promoted->Votes().empty());
        CHECK(promoted.has_value() && promoted->State(r.now + 5000).raceIndex == r.S().raceIndex &&
              promoted->State(r.now + 5000).phaseLeftMs == kVoteMs);
    }

    // A watcher successor: the host always races, so it races from the next grid.
    {
        Rig r;
        const uint8_t wim = r.Add("Wim", Seat::Watch);
        const uint8_t val = r.Add("Val", Seat::Watch);
        std::optional<RoomMachine> promoted = RoomMachine::Promote(MirrorOf(r), wim, r.now);
        CHECK(promoted.has_value());
        if (!promoted.has_value()) {
            return;
        }
        Rig q;
        q.m = std::move(*promoted);
        q.now = r.now;
        CHECK(q.P() == Phase::Lobby && q.m.Racers() == 1 && q.m.Watchers() == 1);
        CHECK(!q.Who(kHostId)->watcher && q.Who(val)->watcher);
        q.Begin();
        CHECK(q.P() == Phase::Machine && q.OnGrid() == std::vector<uint8_t>({ kHostId }));
        q.LockAll();
        CHECK(q.P() == Phase::GridSync);
        CHECK(q.m.LastGrid().humans.size() == 1 && q.m.LastGrid().humans[0].id == kHostId);
        q.GridAll();
        CHECK(q.P() == Phase::Countdown);
        q.Verify();
    }

    // Every setting the host had carries over; ready marks do not.
    {
        Rig r;
        const uint8_t ana = r.Add("Ana");
        CHECK(r.m.SetRacerCap(8, r.now, r.out) && r.m.SetCustomTracks(true, r.now, r.out));
        CHECK(r.m.SetLastPlacePicks(true, r.now, r.out));
        CpuOptions cpu;
        cpu.fill = false;
        cpu.aiAggression = 250;
        cpu.trackHazards = true;
        CHECK(r.m.SetCpuOptions(cpu, r.now, r.out));
        RaceRules rules;
        rules.laps = 7;
        rules.lavaNoKill = true;
        rules.boostFrames = 90;
        CHECK(r.m.SetRules(rules));
        CHECK(r.m.SetReady(ana, true, r.now, r.out));
        std::optional<RoomMachine> promoted = RoomMachine::Promote(MirrorOf(r), ana, r.now);
        CHECK(promoted.has_value());
        if (!promoted.has_value()) {
            return;
        }
        const RoomState was = r.S();
        const RoomState after = promoted->State(r.now);
        CHECK(after.racerCap == 8 && after.customTracks && after.rules == rules && after.pool == was.pool);
        CHECK(after.lastPlacePicks);
        CHECK(promoted->Cpu() == cpu && !after.cpuFill && after.aiAggression == 250 && after.cpuTrackHazards);
        CHECK(after.phase == Phase::Lobby && after.roomId == was.roomId);
        CHECK(!promoted->Roster()[0].ready);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash keeps the lines before it
    TestDraws();
    TestSeats();
    TestLobby();
    TestTimeouts();
    TestVote();
    TestCommittedLottery();
    TestRoomCode();
    TestPick();
    TestMachine();
    TestRaceNext();
    TestGrid();
    TestGridOrder();
    TestTrackCheck();
    TestRules();
    TestEmotes();
    TestSeries();
    TestFinishCheck();
    TestRaceEnd();
    TestResultsOrder();
    TestRaceLeaveAndJoin();
    TestSeriesPoints();
    TestMachineRecord();
    TestCustomTrack();
    TestSuccessorState();
    TestPromote();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
