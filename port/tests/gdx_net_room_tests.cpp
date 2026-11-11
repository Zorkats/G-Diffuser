// port/tests/gdx_net_room_tests.cpp: the room state machine (port/net/gdx_net_room.h) on a fake clock.
//
// No network and no thread: each test calls the machine as the host session would and reads back its state
// and the messages it asked to send. Every timeout is checked one millisecond before it runs out and on it.

#include "../net/gdx_net_room.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

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

    CHECK(!r.m.SetRacerCap(7, r.now, r.out));
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
    CHECK(r.P() != Phase::Lobby);
    CHECK(!r.Who(kHostId)->ready && !r.Who(ana)->ready);
    CHECK(!r.m.SetReady(ana, true, r.now, r.out)); // no ready mark outside the lobby
    CHECK(!r.m.Start(r.now, r.out));
    r.Verify();

    // The host may start alone.
    Rig solo;
    CHECK(solo.m.Start(solo.now, solo.out));
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

    // Under four racers last place picks, and has kPickMs to do it.
    Rig p;
    p.Add("Ana");
    CHECK(p.m.Start(p.now, p.out));
    p.Race({ 0, 1 });
    p.After(kResultsMs);
    ExpectTimeout(p, Phase::Pick, kPickMs, Phase::Machine);
    p.Verify();

    // Nobody on the grid: there is no race to run.
    Rig g;
    g.Add("Ana");
    CHECK(g.m.Start(g.now, g.out));
    g.LockAll();
    ExpectTimeout(g, Phase::GridSync, kGridMs, Phase::Results);
    CHECK(g.m.LastOrder().empty() && g.OnGrid().empty());
    g.After(kResultsMs); // no last place to pick: the room draws
    CHECK(g.P() == Phase::Machine && g.m.LastPicked().drawn);
    g.Verify();
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
    CHECK(r.m.Start(r.now, r.out));
    // Three racers, first race of the series: nobody finished last yet, so the room draws.
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

    // Four racers vote; when one leaves, the three left pick.
    Rig v;
    v.Add("Ana");
    v.Add("Bo");
    v.Add("Cy");
    CHECK(v.m.Start(v.now, v.out));
    v.SkipVote();
    v.Race({ 3, 2, 1, 0 });
    v.m.Leave(2, v.now, v.out);
    v.After(kResultsMs);
    CHECK(v.P() == Phase::Pick && v.S().picker == kHostId);
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
    CHECK(r.P() == Phase::Machine);
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
    CHECK(r.P() == Phase::Voting); // four racers now
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
    t.SkipVote(); // four racers
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
    CHECK(t.P() == Phase::Machine && t.Who(wim)->watcher && !t.Who(wim)->inRace && t.S().racers == 2);
    t.Verify();
}

void TestGrid() {
    std::printf("grid\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t wim = r.Add("Wim", Seat::Watch);
    CHECK(r.m.Start(r.now, r.out));
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
    CHECK(l.m.Start(l.now, l.out));
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
    CHECK(r.m.Start(r.now, r.out));
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
    CHECK(same.m.Start(same.now, same.out));
    CHECK(other.m.Start(other.now, other.out));
    same.LockAll();
    other.LockAll();
    Rig again;
    CHECK(again.m.Start(again.now, again.out));
    again.LockAll();
    CHECK(same.m.LastGrid().seed == again.m.LastGrid().seed && same.m.LastGrid().seed != other.m.LastGrid().seed);
    CHECK(Ids(same.m.LastGrid()) == std::vector<uint8_t>{ kHostId } && same.m.LastGrid().cpus == kGridSlots - 1);

    // A room with the CPUs off sends none.
    RoomConfig bare = Config(77);
    bare.cpuFill = false;
    Rig n(bare);
    n.Add("Ana");
    CHECK(n.m.Start(n.now, n.out));
    n.LockAll();
    CHECK(n.m.LastGrid().cpus == 0 && n.m.LastGrid().humans.size() == 2);
    n.Verify();

    // The time runs out on the lock-in screen: the grid goes out with the racers who never locked too.
    Rig e;
    const uint8_t only = e.Add("Ana");
    CHECK(e.m.Start(e.now, e.out));
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
    CHECK(r.m.Start(r.now, r.out));
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
    CHECK(h.m.Start(h.now, h.out));
    h.LockAll();
    CHECK(h.GridReady(a));
    ExpectTimeout(h, Phase::GridSync, kGridMs, Phase::Results);
    CHECK(h.OnGrid().empty() && h.m.LastOrder().empty() && Count(h.out, kMsgStart) == 0);
    h.Verify();

    // Everyone but the host has another track: the host races alone.
    Rig o;
    const uint8_t x = o.Add("Ana");
    CHECK(o.m.Start(o.now, o.out));
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
    CHECK(r.m.Start(r.now, r.out));
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

    // A new series starts with a draw again, whoever finished last in the one before.
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
    CHECK(r.m.Start(r.now, r.out));
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
    CHECK(e.m.Start(e.now, e.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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
        CHECK(r.m.Start(r.now, r.out));
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

void TestSeriesPoints() {
    std::printf("series points\n");
    Rig r;
    const uint8_t ana = r.Add("Ana");
    const uint8_t bo = r.Add("Bo");
    CHECK(r.m.Start(r.now, r.out));
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

int main() {
    TestDraws();
    TestSeats();
    TestLobby();
    TestTimeouts();
    TestVote();
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
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
