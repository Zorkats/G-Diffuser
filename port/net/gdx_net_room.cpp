// port/net/gdx_net_room.cpp: the room state machine (gdx_net_room.h).

#include "gdx_net_room.h"

#include <algorithm>

#include "gdx_net_lottery.h"
#include "gdx_net_succession.h"

namespace gdx::net::lobby {

namespace {

uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs; // unsigned: correct across the 49-day wrap
}

// The murmur3 finalizer: every input bit reaches every output bit.
uint32_t Mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// FNV-1a, one byte at a time.
void Feed(uint32_t& h, uint8_t byte) {
    h ^= byte;
    h *= 16777619u;
}

} // namespace

uint32_t RoomHash(uint32_t roomId, uint16_t raceIndex, const std::vector<Ballot>& ballots) {
    uint32_t h = 2166136261u;
    for (int shift = 0; shift < 32; shift += 8) {
        Feed(h, static_cast<uint8_t>(roomId >> shift));
    }
    Feed(h, static_cast<uint8_t>(raceIndex & 0xFF));
    Feed(h, static_cast<uint8_t>(raceIndex >> 8));
    for (const Ballot& b : ballots) {
        Feed(h, b.id);
        Feed(h, b.track);
    }
    return Mix(h);
}

uint8_t DrawTrack(uint32_t hash, uint32_t pool, uint8_t avoid) {
    std::array<uint8_t, kStockTracks> tracks{};
    size_t count = 0;
    for (uint8_t t = 0; t < kStockTracks; t++) {
        if (((pool >> t) & 1u) != 0 && t != avoid) {
            tracks[count++] = t;
        }
    }
    if (count == 0) {
        // The pool holds only the track to avoid, or nothing at all.
        return (avoid < kStockTracks && ((pool >> avoid) & 1u) != 0) ? avoid : static_cast<uint8_t>(0);
    }
    return tracks[hash % count];
}

uint8_t DrawnTrack(uint32_t roomId, uint16_t raceIndex, uint32_t pool, uint8_t lastTrack) {
    return DrawTrack(RoomHash(roomId, raceIndex, std::vector<Ballot>()), pool, lastTrack);
}

namespace {

// The lottery rule on a given seed word. The ballots are sorted by player id.
LotteryResult LotteryOn(uint32_t hash, std::vector<Ballot> ballots, uint32_t pool, uint8_t lastTrack) {
    LotteryResult result;
    if (ballots.empty()) {
        result.track = DrawTrack(hash, pool, lastTrack);
        return result;
    }
    result.winner = static_cast<uint8_t>(hash % ballots.size());
    result.track = ballots[result.winner].track;
    result.custom = ballots[result.winner].custom; // empty unless the ballot named a custom track
    if (result.track == kTrackRandom) {
        // A second hash, so the track does not follow from which ballot won.
        result.track = DrawTrack(Mix(hash ^ 0x9E3779B9u), pool, lastTrack);
    }
    result.ballots = std::move(ballots);
    return result;
}

void SortBallots(std::vector<Ballot>& ballots) {
    std::sort(ballots.begin(), ballots.end(), [](const Ballot& a, const Ballot& b) { return a.id < b.id; });
}

} // namespace

LotteryResult RunLottery(uint32_t roomId, uint16_t raceIndex, std::vector<Ballot> ballots, uint32_t pool,
                         uint8_t lastTrack) {
    SortBallots(ballots);
    const uint32_t hash = RoomHash(roomId, raceIndex, ballots);
    return LotteryOn(hash, std::move(ballots), pool, lastTrack);
}

LotteryResult RunCommittedLottery(const Bytes32& salt, uint32_t roomId, uint16_t raceIndex, std::vector<Ballot> ballots,
                                  uint32_t pool, uint8_t lastTrack) {
    SortBallots(ballots);
    const uint32_t hash = lottery::SeedHash(lottery::DrawSeed(salt, roomId, raceIndex, ballots));
    LotteryResult result = LotteryOn(hash, std::move(ballots), pool, lastTrack);
    result.salt = salt;
    return result;
}

bool CheckLottery(const RoomState& room, const LotteryResult& lottery) {
    if (IsZero32(room.commitment)) {
        return room.type != RoomType::Open && IsZero32(lottery.salt) &&
               RunLottery(room.roomId, room.raceIndex, lottery.ballots, room.pool, room.lastTrack) == lottery;
    }
    return lottery::Verify(room.commitment, room.roomId, room.raceIndex, lottery.salt) &&
           RunCommittedLottery(lottery.salt, room.roomId, room.raceIndex, lottery.ballots, room.pool,
                               room.lastTrack) == lottery;
}

bool EmoteAllowed(std::deque<uint32_t>& times, uint32_t nowMs) {
    while (!times.empty() && Elapsed(nowMs, times.front()) >= kEmoteWindowMs) {
        times.pop_front();
    }
    if (times.size() >= kEmoteBurst) {
        return false;
    }
    times.push_back(nowMs);
    return true;
}

uint8_t HandoverId(uint8_t id, uint8_t successor) {
    if (id == successor) {
        return kHostId;
    }
    return id == kHostId ? kSystemId : id;
}

RoomMachine::RoomMachine(const RoomConfig& config, const PlayerInfo& host) : config_(config) {
    if (!PoolOk(config_.pool)) {
        config_.pool = kPoolAll;
    }
    cpu_.fill = config_.cpuFill;
    PlayerInfo self = host;
    self.id = kHostId;
    self.host = true;
    self.ready = false;
    self.watcher = false;
    self.raceNext = false;
    self.inRace = false;
    self.locked = false;
    self.pingMs = 0;
    roster_.push_back(self);
}

std::optional<RoomMachine> RoomMachine::Promote(const RoomMirror& mirror, uint8_t successor, uint32_t nowMs,
                                                const Bytes32& lotteryKey) {
    const RoomState& room = mirror.room;
    if (successor == kHostId || successor >= kMaxPlayers) {
        return std::nullopt;
    }
    const PlayerInfo* self = nullptr;
    for (const PlayerInfo& p : mirror.roster) {
        if (p.id == successor) {
            self = &p;
        }
    }
    if (self == nullptr) {
        return std::nullopt;
    }
    RoomConfig config;
    config.roomId = room.roomId;
    config.pool = room.pool;
    config.cpuFill = room.cpuFill;
    config.type = room.type;
    config.lotteryKey = lotteryKey;
    config.code = room.code; // the successor takes the code over (O5.10)
    // The constructor seats the successor as player 0 in a race seat and keeps its points: the host always
    // races, so a watcher successor races from the next grid.
    RoomMachine m(config, *self);
    for (const PlayerInfo& p : mirror.roster) {
        const uint8_t id = HandoverId(p.id, successor);
        if (id == kSystemId || id == kHostId || id >= kMaxPlayers || m.Find(id) != nullptr) {
            continue;
        }
        PlayerInfo member = p;
        member.id = id;
        member.host = false;
        member.ready = false;
        member.inRace = false;
        member.locked = false;
        m.seats_[id] = SeatData();
        m.roster_.push_back(member);
    }
    std::sort(m.roster_.begin(), m.roster_.end(),
              [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
    for (const PlayerInfo& p : m.roster_) {
        m.seats_[p.id].joinSerial = ++m.joinSerial_;
    }
    // The old host raced, so the racers do not outnumber the seats it had.
    const size_t racers = m.Racers();
    m.racerCap_ = static_cast<uint8_t>(std::min<size_t>(kMaxRacers, std::max<size_t>(room.racerCap, racers)));
    if (m.racerCap_ < kDefaultRacers) {
        m.racerCap_ = static_cast<uint8_t>(kDefaultRacers);
    }
    m.customTracks_ = room.customTracks;
    m.cpu_.fill = room.cpuFill;
    m.cpu_.aiAggression = room.aiAggression < kAiAggressionMin   ? kAiAggressionMin
                          : room.aiAggression > kAiAggressionMax ? kAiAggressionMax
                                                                 : room.aiAggression;
    m.cpu_.trackHazards = room.cpuTrackHazards;
    if (RulesOk(room.rules)) {
        m.rules_ = room.rules;
    }
    m.raceIndex_ = room.raceIndex;
    m.lastTrack_ = room.lastTrack;

    // The series so far: its points are in the roster. Its last table names last place, who picks next.
    bool series = false;
    for (const PlayerInfo& p : m.roster_) {
        series = series || p.points != 0;
    }
    const bool finalTable = mirror.haveResults && mirror.results.final;
    if (finalTable) {
        m.results_.raceIndex = mirror.results.raceIndex;
        m.results_.final = true;
        for (const ResultEntry& e : mirror.results.entries) {
            const uint8_t id = HandoverId(e.id, successor);
            if (id == kSystemId || m.Find(id) == nullptr) {
                continue;
            }
            ResultEntry entry = e;
            entry.id = id;
            m.results_.entries.push_back(entry);
            if (series) {
                m.lastOrder_.push_back(id);
            }
        }
    }
    m.seriesRaces_ = series ? 1u : 0u;

    std::vector<Outgoing> unsent; // nobody is linked yet: each member gets the room when it comes back
    if (room.phase == Phase::Lobby) {
        m.SetPhase(Phase::Lobby, 0, nowMs);
    } else if (room.phase == Phase::Results && finalTable && mirror.results.raceIndex == room.raceIndex) {
        m.track_ = room.track;
        const uint32_t left = std::min(room.phaseLeftMs, m.config_.resultsMs);
        m.SetPhase(Phase::Results, left != 0 ? left : 1u, nowMs);
    } else {
        const bool gridOut = room.phase == Phase::Assets || room.phase == Phase::GridSync ||
                             room.phase == Phase::Countdown || room.phase == Phase::Racing ||
                             room.phase == Phase::Results;
        if (gridOut) {
            m.raceIndex_ = static_cast<uint16_t>(room.raceIndex + 1);
        }
        m.BeginTrackChoice(nowMs, unsent);
    }
    m.rosterDirty_ = true;
    m.roomDirty_ = true;
    return m;
}

PlayerInfo* RoomMachine::Find(uint8_t id) {
    for (PlayerInfo& p : roster_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const PlayerInfo* RoomMachine::Find(uint8_t id) const {
    for (const PlayerInfo& p : roster_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

uint8_t RoomMachine::FreeId() const {
    for (uint8_t id = 1; id < kMaxPlayers; id++) {
        if (Find(id) == nullptr) {
            return id;
        }
    }
    return kSystemId; // unreachable: Join checks the seat counts first
}

size_t RoomMachine::Racers() const {
    size_t n = 0;
    for (const PlayerInfo& p : roster_) {
        n += p.watcher ? 0 : 1;
    }
    return n;
}

size_t RoomMachine::Watchers() const {
    return roster_.size() - Racers();
}

size_t RoomMachine::OnGrid() const {
    size_t n = 0;
    for (const PlayerInfo& p : roster_) {
        n += p.inRace ? 1 : 0;
    }
    return n;
}

uint8_t RoomMachine::Engine(uint8_t id) const {
    return id < kMaxPlayers ? seats_[id].engine : static_cast<uint8_t>(50);
}

uint16_t RoomMachine::LoadMs(uint8_t id) const {
    return id < kMaxPlayers ? seats_[id].loadMs : static_cast<uint16_t>(0);
}

bool RoomMachine::AllVoted() const {
    for (const PlayerInfo& p : roster_) {
        if (p.watcher) {
            continue;
        }
        bool voted = false;
        for (const Ballot& b : votes_) {
            voted = voted || b.id == p.id;
        }
        if (!voted) {
            return false;
        }
    }
    return true;
}

bool RoomMachine::AllLocked() const {
    for (const PlayerInfo& p : roster_) {
        if (p.inRace && !p.locked) {
            return false;
        }
    }
    return true;
}

bool RoomMachine::AllHeld() const {
    if (track_ != kTrackCustom) {
        return true;
    }
    for (const PlayerInfo& p : roster_) {
        if ((p.id == kHostId || p.inRace) && !seats_[p.id].holdsTrack) {
            return false;
        }
    }
    return true;
}

bool RoomMachine::AllGridReady() const {
    for (const PlayerInfo& p : roster_) {
        if (p.inRace && !seats_[p.id].gridReady) {
            return false;
        }
    }
    return true;
}

bool RoomMachine::HasReport(uint8_t id) const {
    for (const Report& r : reports_) {
        if (r.id == id) {
            return true;
        }
    }
    return false;
}

bool RoomMachine::AllReported() const {
    size_t racing = 0;
    for (const PlayerInfo& p : roster_) {
        if (!p.inRace) {
            continue;
        }
        if (!HasReport(p.id)) {
            return false;
        }
        racing++;
    }
    return racing > 0;
}

// The table: finishers by time, then those still driving in the host game's order, then the retired with
// the one who lasted longest first. A place is worth its points whether the racer finished or was still
// driving; retiring is worth none. Once the host's game has left the race its order is an old one, so
// those still driving go by the lap of their latest pose first and by that order within a lap.
RaceResults RoomMachine::BuildResults(bool final) const {
    RaceResults results;
    results.raceIndex = raceIndex_;
    results.final = final;
    const auto add = [&results](uint8_t id, ResultKind kind, uint32_t timeMs) {
        ResultEntry e;
        e.id = id;
        e.kind = kind;
        e.timeMs = timeMs;
        e.points = kind == ResultKind::Retired ? static_cast<uint8_t>(0) : PlacePoints(results.entries.size() + 1);
        results.entries.push_back(e);
    };

    std::vector<Report> sorted = reports_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Report& a, const Report& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.kind == ResultKind::Finished) {
            return a.timeMs != b.timeMs ? a.timeMs < b.timeMs : a.serial < b.serial;
        }
        return a.serial > b.serial;
    });
    for (const Report& r : sorted) {
        if (r.kind == ResultKind::Finished) {
            add(r.id, r.kind, r.timeMs);
        }
    }
    std::vector<uint8_t> driving;
    const auto drives = [this, &driving](uint8_t id) {
        const PlayerInfo* p = Find(id);
        return p != nullptr && p->inRace && !HasReport(id) &&
               std::find(driving.begin(), driving.end(), id) == driving.end();
    };
    for (uint8_t id : standing_) {
        if (drives(id)) {
            driving.push_back(id);
        }
    }
    for (const PlayerInfo& p : roster_) {
        if (drives(p.id)) {
            driving.push_back(p.id);
        }
    }
    if (cpusOut_) {
        std::stable_sort(driving.begin(), driving.end(),
                         [this](uint8_t a, uint8_t b) { return seats_[a].lap > seats_[b].lap; });
    }
    for (uint8_t id : driving) {
        add(id, ResultKind::Unfinished, 0);
    }
    for (const Report& r : sorted) {
        if (r.kind == ResultKind::Retired) {
            add(r.id, r.kind, 0);
        }
    }
    return results;
}

uint8_t RoomMachine::LastPlace() const {
    for (auto it = lastOrder_.rbegin(); it != lastOrder_.rend(); ++it) {
        const PlayerInfo* p = Find(*it);
        if (p != nullptr && !p->watcher) {
            return p->id;
        }
    }
    return kSystemId;
}

RoomState RoomMachine::State(uint32_t nowMs) const {
    RoomState s;
    s.roomId = config_.roomId;
    s.raceIndex = raceIndex_;
    s.phase = phase_;
    s.phaseTotalMs = phaseMs_;
    const uint32_t spent = Elapsed(nowMs, phaseStartMs_);
    s.phaseLeftMs = spent < phaseMs_ ? phaseMs_ - spent : 0;
    s.racerCap = racerCap_;
    s.customTracks = customTracks_;
    s.pool = config_.pool;
    s.track = track_;
    if (track_ == kTrackCustom) {
        s.custom = custom_;
    }
    s.lastTrack = lastTrack_;
    s.picker = picker_;
    s.racers = static_cast<uint8_t>(OnGrid());
    s.cpusOut = cpusOut_;
    for (const PlayerInfo& p : roster_) {
        s.locked = static_cast<uint8_t>(s.locked + (p.locked ? 1 : 0));
    }
    s.cpuFill = cpu_.fill;
    s.aiAggression = cpu_.aiAggression;
    s.cpuTrackHazards = cpu_.trackHazards;
    s.rules = rules_;
    s.successor = Successor();
    if (s.successor != kSystemId) {
        s.successorIp = seats_[s.successor].ip;
        s.successorPort = seats_[s.successor].port;
    }
    s.type = config_.type;
    s.commitment = commitment_;
    s.code = config_.code;
    return s;
}

uint8_t RoomMachine::Successor() const {
    std::vector<const PlayerInfo*> order;
    for (const PlayerInfo& p : roster_) {
        if (p.id != kHostId && p.id < kMaxPlayers) {
            order.push_back(&p);
        }
    }
    std::sort(order.begin(), order.end(), [this](const PlayerInfo* a, const PlayerInfo* b) {
        return seats_[a->id].joinSerial < seats_[b->id].joinSerial;
    });
    std::vector<succession::Member> members;
    members.reserve(order.size());
    for (const PlayerInfo* p : order) {
        succession::Member m;
        m.id = p->id;
        m.racer = !p->watcher;
        // A member the host cannot give an address for cannot be reached by the others.
        m.present = seats_[p->id].port != 0;
        m.directReachable = seats_[p->id].reachable;
        members.push_back(m);
    }
    const std::optional<uint8_t> next = succession::Successor(members, config_.type != RoomType::Lan);
    return next.has_value() ? *next : kSystemId;
}

void RoomMachine::SetReachable(uint8_t id, bool reachable) {
    if (id == kHostId || Find(id) == nullptr || seats_[id].reachable == reachable) {
        return;
    }
    seats_[id].reachable = reachable;
    roomDirty_ = true; // the successor may change, and `room` names it
}

void RoomMachine::SetAddress(uint8_t id, const IpBytes& ip, uint16_t port) {
    if (id == kHostId || Find(id) == nullptr) {
        return;
    }
    SeatData& seat = seats_[id];
    if (seat.ip != ip || seat.port != port) {
        seat.ip = ip;
        seat.port = port;
        roomDirty_ = true;
    }
}

bool RoomMachine::SetCpuOptions(const CpuOptions& cpu, uint32_t nowMs, std::vector<Outgoing>& out) {
    const bool gridSent = phase_ == Phase::Assets || phase_ == Phase::GridSync || phase_ == Phase::Countdown ||
                          phase_ == Phase::Racing;
    if (gridSent || cpu.aiAggression < kAiAggressionMin || cpu.aiAggression > kAiAggressionMax) {
        return false;
    }
    if (!(cpu == cpu_)) {
        cpu_ = cpu;
        roomDirty_ = true;
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::SetPhase(Phase phase, uint32_t lengthMs, uint32_t nowMs) {
    phase_ = phase;
    phaseStartMs_ = nowMs;
    phaseMs_ = lengthMs;
    roomDirty_ = true;
}

void RoomMachine::ClearRaceFlags() {
    for (PlayerInfo& p : roster_) {
        if (p.inRace || p.locked) {
            rosterDirty_ = true;
        }
        p.inRace = false;
        p.locked = false;
        seats_[p.id].gridReady = false;
        seats_[p.id].loadMs = 0;
        seats_[p.id].lap = 0;
        seats_[p.id].holdsTrack = false;
    }
    reports_.clear();
    standing_.clear();
    if (cpusOut_) {
        cpusOut_ = false;
        roomDirty_ = true;
    }
}

void RoomMachine::Flush(uint32_t nowMs, std::vector<Outgoing>& out) {
    // The host recomputes the successor on every change and announces it (decision 19).
    const uint8_t successor = Successor();
    if (successor != announcedSuccessor_) {
        announcedSuccessor_ = successor;
        roomDirty_ = true;
    }
    if (rosterDirty_) {
        rosterDirty_ = false;
        out.push_back({ kEveryone, kMsgRoster, EncodeRoster(roster_) });
    }
    if (roomDirty_) {
        roomDirty_ = false;
        out.push_back({ kEveryone, kMsgRoom, EncodeRoomState(State(nowMs)) });
    }
}

bool RoomMachine::Join(const JoinRequest& join, uint32_t nowMs, uint8_t& id, RejectReason& why,
                       std::vector<Outgoing>& out) {
    if (!NameOk(join.name) || !ColourOk(join.colour) || !MachineOk(join.machine)) {
        why = RejectReason::BadProfile;
        return false;
    }
    const bool raceFull = Racers() >= racerCap_;
    const bool watchFull = Watchers() >= kMaxWatchers;
    if (join.seat == Seat::Race && raceFull) {
        why = watchFull ? RejectReason::Full : RejectReason::RaceSeatsFull;
        return false;
    }
    if (join.seat == Seat::Watch && watchFull) {
        why = raceFull ? RejectReason::Full : RejectReason::WatchSeatsFull;
        return false;
    }
    PlayerInfo player;
    player.id = FreeId();
    player.watcher = join.seat == Seat::Watch;
    player.colour = join.colour;
    player.machine = join.machine;
    player.name = join.name;
    seats_[player.id] = SeatData();
    seats_[player.id].joinSerial = ++joinSerial_;
    seats_[player.id].reachable = join.reachable;
    roster_.push_back(player);
    std::sort(roster_.begin(), roster_.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
    id = player.id;
    CatchUp(id, nowMs, out);
    return true;
}

void RoomMachine::Rejoined(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out) {
    if (id == kHostId || Find(id) == nullptr) {
        return;
    }
    CatchUp(id, nowMs, out);
}

void RoomMachine::CatchUp(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out) {
    rosterDirty_ = true;
    roomDirty_ = true; // the simplest way to give the player the room; everyone else gets a fresh timer
    Flush(nowMs, out);
    if (phase_ == Phase::Voting) {
        out.push_back({ id, kMsgVotes, EncodeVotes(votes_) });
    } else if (phase_ == Phase::Lottery) {
        out.push_back({ id, kMsgLottery, EncodeLottery(lottery_) });
    } else if ((phase_ == Phase::Racing || phase_ == Phase::Results) && results_.raceIndex == raceIndex_ &&
               (results_.final || !results_.entries.empty())) {
        out.push_back({ id, kMsgResults, EncodeResults(results_) });
    }
}

void RoomMachine::Leave(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* leaver = Find(id);
    if (id == kHostId || leaver == nullptr) {
        return;
    }
    const bool wasRacing = leaver->inRace;
    // The owner leaves before the host has its track: nobody else can have the bytes (decision 11).
    const bool ownerGone = id == trackOwner_ && track_ == kTrackCustom && !seats_[kHostId].holdsTrack &&
                           (phase_ == Phase::Lottery || phase_ == Phase::Machine || phase_ == Phase::Assets);
    roster_.erase(std::remove_if(roster_.begin(), roster_.end(), [id](const PlayerInfo& p) { return p.id == id; }),
                  roster_.end());
    seats_[id] = SeatData();
    lastOrder_.erase(std::remove(lastOrder_.begin(), lastOrder_.end(), id), lastOrder_.end());
    standing_.erase(std::remove(standing_.begin(), standing_.end(), id), standing_.end());
    reports_.erase(std::remove_if(reports_.begin(), reports_.end(), [id](const Report& r) { return r.id == id; }),
                   reports_.end());
    rosterDirty_ = true;
    const size_t ballots = votes_.size();
    votes_.erase(std::remove_if(votes_.begin(), votes_.end(), [id](const Ballot& b) { return b.id == id; }),
                 votes_.end());

    if (ownerGone && phase_ != Phase::Assets) {
        FallBack(nowMs, out); // the grid MACHINE sends when it closes names the stock track
    }
    switch (phase_) {
    case Phase::Voting:
        if (votes_.size() != ballots) {
            out.push_back({ kEveryone, kMsgVotes, EncodeVotes(votes_) });
        }
        if (AllVoted()) {
            CloseVote(nowMs, out);
        }
        break;
    case Phase::Pick:
        if (id == picker_) {
            ChooseDrawn(kSystemId, nowMs, out);
        }
        break;
    case Phase::Machine:
        roomDirty_ = true; // the counter's two numbers
        if (AllLocked()) {
            LeaveMachine(nowMs, out);
        }
        break;
    case Phase::Assets:
        if (ownerGone) {
            FallBack(nowMs, out);
        } else if (AllHeld()) {
            CloseAssets(nowMs, out);
        }
        break;
    case Phase::GridSync:
        if (AllGridReady()) {
            CloseGrid(nowMs, out);
        }
        break;
    case Phase::Racing:
        if (!wasRacing) {
            break;
        }
        if (AllReported()) {
            FinishRace(nowMs, out); // the one result the race waited for will not come
        } else if (!results_.entries.empty() && results_.raceIndex == raceIndex_) {
            results_ = BuildResults(false); // the table already sent names the one who left
            out.push_back({ kEveryone, kMsgResults, EncodeResults(results_) });
        }
        break;
    default:
        break;
    }
    Flush(nowMs, out);
}

bool RoomMachine::SetReady(uint8_t id, bool ready, uint32_t nowMs, std::vector<Outgoing>& out) {
    PlayerInfo* p = Find(id);
    if (p == nullptr || p->watcher || phase_ != Phase::Lobby) {
        return false;
    }
    if (p->ready != ready) {
        p->ready = ready;
        rosterDirty_ = true;
    }
    bool allReady = Racers() >= 2;
    for (const PlayerInfo& q : roster_) {
        allReady = allReady && (q.watcher || q.ready);
    }
    if (allReady) {
        return Start(nowMs, out);
    }
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::Start(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phase_ != Phase::Lobby) {
        return false;
    }
    for (PlayerInfo& p : roster_) {
        if (p.ready || p.points != 0) {
            p.ready = false;
            p.points = 0; // a new series starts from nothing
            rosterDirty_ = true;
        }
    }
    seriesRaces_ = 0;
    lastOrder_.clear();
    BeginTrackChoice(nowMs, out);
    Flush(nowMs, out);
    return true;
}

void RoomMachine::BeginTrackChoice(uint32_t nowMs, std::vector<Outgoing>& out) {
    ClearRaceFlags();
    votes_.clear();
    track_ = kTrackNone;
    custom_ = CustomTrack();
    trackOwner_ = kSystemId;
    picker_ = kSystemId;
    salt_ = Bytes32{};
    commitment_ = Bytes32{};
    if (Racers() >= kVoteMinRacers) {
        // An Open room commits to this race's lottery before the first ballot (decision 39).
        if (config_.type == RoomType::Open && !IsZero32(config_.lotteryKey)) {
            salt_ = lottery::RaceSalt(config_.lotteryKey, config_.roomId, raceIndex_);
            commitment_ = lottery::Commit(config_.roomId, raceIndex_, salt_);
        }
        SetPhase(Phase::Voting, config_.voteMs, nowMs);
        out.push_back({ kEveryone, kMsgVotes, EncodeVotes(votes_) });
        return;
    }
    // Under four racers last place picks. A series' first race has no last place, so the host draws.
    const uint8_t picker = seriesRaces_ > 0 ? LastPlace() : kSystemId;
    if (picker == kSystemId) {
        ChooseDrawn(kSystemId, nowMs, out);
        return;
    }
    picker_ = picker;
    SetPhase(Phase::Pick, config_.pickMs, nowMs);
}

void RoomMachine::ChooseDrawn(uint8_t by, uint32_t nowMs, std::vector<Outgoing>& out) {
    picked_ = Picked();
    picked_.track = DrawnTrack(config_.roomId, raceIndex_, config_.pool, lastTrack_);
    picked_.drawn = true;
    picked_.by = by;
    track_ = picked_.track;
    out.push_back({ kEveryone, kMsgPicked, EncodePicked(picked_) });
    EnterMachine(nowMs);
}

bool RoomMachine::ChoiceOk(const TrackChoice& choice) const {
    if (choice.track == kTrackCustom) {
        return customTracks_ && TrackTitleOk(choice.custom.title);
    }
    return TrackInPool(choice.track, config_.pool);
}

void RoomMachine::KnowTrack(const CustomTrack& custom, uint8_t owner) {
    custom_ = custom;
    trackOwner_ = owner;
    seats_[owner].holdsTrack = true; // the owner named a track of its own
}

bool RoomMachine::Vote(uint8_t id, uint8_t track, uint32_t nowMs, std::vector<Outgoing>& out) {
    TrackChoice choice;
    choice.track = track;
    return track != kTrackCustom && Vote(id, choice, nowMs, out);
}

bool RoomMachine::Vote(uint8_t id, const TrackChoice& choice, uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* p = Find(id);
    if (p == nullptr || p->watcher || phase_ != Phase::Voting || !ChoiceOk(choice)) {
        return false;
    }
    for (const Ballot& b : votes_) {
        if (b.id == id) {
            return false;
        }
    }
    Ballot ballot;
    ballot.id = id;
    ballot.track = choice.track;
    if (choice.track == kTrackCustom) {
        ballot.custom = choice.custom;
    }
    votes_.push_back(ballot);
    std::sort(votes_.begin(), votes_.end(), [](const Ballot& a, const Ballot& b) { return a.id < b.id; });
    out.push_back({ kEveryone, kMsgVotes, EncodeVotes(votes_) });
    if (AllVoted()) {
        CloseVote(nowMs, out);
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::CloseVote(uint32_t nowMs, std::vector<Outgoing>& out) {
    lottery_ = IsZero32(commitment_)
                   ? RunLottery(config_.roomId, raceIndex_, votes_, config_.pool, lastTrack_)
                   : RunCommittedLottery(salt_, config_.roomId, raceIndex_, votes_, config_.pool, lastTrack_);
    track_ = lottery_.track;
    if (track_ == kTrackCustom) {
        KnowTrack(lottery_.custom, lottery_.ballots[lottery_.winner].id);
    }
    out.push_back({ kEveryone, kMsgLottery, EncodeLottery(lottery_) });
    SetPhase(Phase::Lottery, config_.lotteryMs, nowMs);
}

bool RoomMachine::SkipLottery(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phase_ != Phase::Lottery) {
        return false;
    }
    EnterMachine(nowMs);
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::Pick(uint8_t id, uint8_t track, uint32_t nowMs, std::vector<Outgoing>& out) {
    TrackChoice choice;
    choice.track = track;
    return track != kTrackCustom && Pick(id, choice, nowMs, out);
}

bool RoomMachine::Pick(uint8_t id, const TrackChoice& choice, uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phase_ != Phase::Pick || id != picker_ || Find(id) == nullptr || !ChoiceOk(choice)) {
        return false;
    }
    if (choice.track == kTrackRandom) {
        ChooseDrawn(id, nowMs, out);
    } else {
        picked_ = Picked();
        picked_.track = choice.track;
        picked_.drawn = false;
        picked_.by = id;
        track_ = choice.track;
        if (track_ == kTrackCustom) {
            picked_.custom = choice.custom;
            KnowTrack(choice.custom, id);
        }
        out.push_back({ kEveryone, kMsgPicked, EncodePicked(picked_) });
        EnterMachine(nowMs);
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::EnterMachine(uint32_t nowMs) {
    // Watchers who asked to race take the free race seats, in the order they asked.
    std::vector<PlayerInfo*> asking;
    for (PlayerInfo& p : roster_) {
        if (p.watcher && p.raceNext) {
            asking.push_back(&p);
        }
    }
    std::sort(asking.begin(), asking.end(), [this](const PlayerInfo* a, const PlayerInfo* b) {
        return seats_[a->id].nextSerial < seats_[b->id].nextSerial;
    });
    for (PlayerInfo* p : asking) {
        if (Racers() >= racerCap_) {
            break;
        }
        p->watcher = false;
        p->raceNext = false;
        seats_[p->id].nextSerial = 0;
    }
    picker_ = kSystemId;
    for (PlayerInfo& p : roster_) {
        p.inRace = !p.watcher;
        p.locked = false;
        seats_[p.id].gridReady = false;
        // A record travels for one race: a racer who does not lock in this one races a stock machine.
        // holdsTrack is kept: a track may have moved while the lottery was shown.
        seats_[p.id].hasRecord = false;
        seats_[p.id].record = MachineRecord{};
    }
    rosterDirty_ = true;
    SetPhase(Phase::Machine, config_.machineMs, nowMs);
}

bool RoomMachine::Lock(uint8_t id, const LockRequest& lock, uint32_t nowMs, std::vector<Outgoing>& out) {
    PlayerInfo* p = Find(id);
    if (p == nullptr || phase_ != Phase::Machine || !p->inRace || p->locked || lock.machine >= kRosterMachines ||
        lock.skin >= kMachineSkins || lock.engine > kEngineMax || (lock.hasRecord && !MachineRecordOk(lock.record))) {
        return false;
    }
    p->machine = lock.machine;
    p->skin = lock.skin;
    p->locked = true;
    seats_[id].engine = lock.engine;
    seats_[id].hasRecord = lock.hasRecord;
    seats_[id].record = lock.hasRecord ? lock.record : MachineRecord{};
    rosterDirty_ = true;
    roomDirty_ = true; // the counter
    if (AllLocked()) {
        LeaveMachine(nowMs, out);
    }
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::SetRules(const RaceRules& rules) {
    // ASSETS may send a fresh grid at any moment, so the rules are fixed once MACHINE ends.
    const bool gridSent = phase_ == Phase::Assets || phase_ == Phase::GridSync || phase_ == Phase::Countdown ||
                          phase_ == Phase::Racing;
    if (gridSent || !RulesOk(rules)) {
        return false;
    }
    if (!(rules == rules_)) {
        rules_ = rules;
        roomDirty_ = true;
    }
    return true;
}

void RoomMachine::LeaveMachine(uint32_t nowMs, std::vector<Outgoing>& out) {
    // A racer who never locked keeps the machine of its last lock, or its profile machine. A profile with
    // no machine gets the first one, so every racer on the grid has a machine.
    for (PlayerInfo& p : roster_) {
        if (p.inRace && p.machine == kNoMachine) {
            p.machine = 0;
            rosterDirty_ = true;
        }
    }
    if (!AllHeld()) {
        SetPhase(Phase::Assets, config_.assetsMs, nowMs);
        return;
    }
    SendGrid(nowMs, out);
}

// The wait for the custom track is over: everyone holds it, or the time ran out. Without the host's copy
// there is no track at all, so the host draws a stock one. A racer without it watches this race.
void RoomMachine::CloseAssets(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (!seats_[kHostId].holdsTrack) {
        FallBack(nowMs, out);
        return;
    }
    for (PlayerInfo& p : roster_) {
        if (p.inRace && !seats_[p.id].holdsTrack) {
            p.inRace = false;
            p.locked = false;
            rosterDirty_ = true;
        }
    }
    SendGrid(nowMs, out);
}

// Decision 11: a stock track from the pool, never the one just raced, and a second hash so it does not
// follow from the draw a pick would have made. In ASSETS a fresh grid goes out at once; in MACHINE the grid
// sent when the screen closes names it. The machines locked so far stand.
void RoomMachine::FallBack(uint32_t nowMs, std::vector<Outgoing>& out) {
    track_ = DrawTrack(Mix(RoomHash(config_.roomId, raceIndex_, std::vector<Ballot>()) ^ 0x46414C4Cu), config_.pool,
                       lastTrack_);
    custom_ = CustomTrack();
    trackOwner_ = kSystemId;
    for (PlayerInfo& p : roster_) {
        seats_[p.id].holdsTrack = false;
    }
    roomDirty_ = true;
    if (phase_ == Phase::Assets) {
        SendGrid(nowMs, out);
    }
}

bool RoomMachine::TrackHeld(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* p = Find(id);
    if (p == nullptr || track_ != kTrackCustom || (phase_ != Phase::Machine && phase_ != Phase::Assets)) {
        return false;
    }
    seats_[id].holdsTrack = true;
    if (phase_ == Phase::Assets && AllHeld()) {
        CloseAssets(nowMs, out);
    }
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::TrackFailed(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (track_ != kTrackCustom ||
        (phase_ != Phase::Lottery && phase_ != Phase::Machine && phase_ != Phase::Assets)) {
        return false;
    }
    FallBack(nowMs, out);
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::HoldsTrack(uint8_t id) const {
    return id < kMaxPlayers && track_ == kTrackCustom && seats_[id].holdsTrack;
}

TrackChoice RoomMachine::CurrentTrack() const {
    TrackChoice choice;
    choice.track = track_;
    if (track_ == kTrackCustom) {
        choice.custom = custom_;
    }
    return choice;
}

void RoomMachine::SendGrid(uint32_t nowMs, std::vector<Outgoing>& out) {
    // The humans take the back rows, as the player does in a stock race, and the last race's winner
    // starts last of all. A racer with no result yet starts ahead of those who have one.
    grid_ = Grid();
    grid_.raceIndex = raceIndex_;
    grid_.seed = Mix(RoomHash(config_.roomId, raceIndex_, std::vector<Ballot>()) ^ 0x47524944u);
    grid_.rules = rules_;
    grid_.track = CurrentTrack();
    const auto add = [this](const PlayerInfo& p) {
        GridEntry e;
        e.id = p.id;
        e.machine = p.machine;
        e.skin = p.skin;
        e.hasRecord = seats_[p.id].hasRecord;
        e.record = seats_[p.id].record;
        grid_.humans.push_back(e);
    };
    for (const PlayerInfo& p : roster_) {
        if (p.inRace && std::find(lastOrder_.begin(), lastOrder_.end(), p.id) == lastOrder_.end()) {
            add(p);
        }
    }
    for (auto it = lastOrder_.rbegin(); it != lastOrder_.rend(); ++it) {
        const PlayerInfo* p = Find(*it);
        if (p != nullptr && p->inRace) {
            add(*p);
        }
    }
    grid_.cpus = cpu_.fill ? static_cast<uint8_t>(kGridSlots - grid_.humans.size()) : static_cast<uint8_t>(0);
    if (!grid_.humans.empty()) {
        out.push_back({ kEveryone, kMsgGrid, EncodeGrid(grid_) });
    }
    SetPhase(Phase::GridSync, config_.gridMs, nowMs);
}

bool RoomMachine::GridReady(uint8_t id, const ReadyToRace& ready, uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* p = Find(id);
    if (p == nullptr || phase_ != Phase::GridSync || !p->inRace || seats_[id].gridReady ||
        ready.raceIndex != raceIndex_) {
        return false;
    }
    seats_[id].gridReady = true;
    seats_[id].digest = ready.digest;
    seats_[id].loadMs = ready.loadMs;
    if (AllGridReady()) {
        CloseGrid(nowMs, out);
    }
    Flush(nowMs, out);
    return true;
}

// The wait for the tracks is over, because everyone answered or because the time ran out. Whoever is not
// ready, or loaded a different track than the host did, watches this race.
void RoomMachine::CloseGrid(uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* host = Find(kHostId);
    const bool hostReady = host != nullptr && host->inRace && seats_[kHostId].gridReady;
    for (PlayerInfo& p : roster_) {
        if (!p.inRace) {
            continue;
        }
        // Without the host copy there is no track to hold the others to, and nobody to run the CPUs.
        if (!hostReady || !seats_[p.id].gridReady || seats_[p.id].digest != seats_[kHostId].digest) {
            p.inRace = false;
            p.locked = false;
            rosterDirty_ = true;
        }
    }
    EnterCountdown(nowMs, out);
}

void RoomMachine::EnterCountdown(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (OnGrid() == 0) {
        FinishRace(nowMs, out); // nobody loaded the track: no race
        return;
    }
    SetPhase(Phase::Countdown, config_.countdownMs, nowMs);
    StartSignal start;
    start.raceIndex = raceIndex_;
    start.startAtMs = nowMs + config_.startLeadMs;
    out.push_back({ kEveryone, kMsgStart, EncodeStart(start) });
}

void RoomMachine::NoteLap(uint8_t id, int16_t lap) {
    if (id < kMaxPlayers) {
        seats_[id].lap = lap;
    }
}

void RoomMachine::SetStanding(const std::vector<uint8_t>& order) {
    if (phase_ == Phase::Racing) {
        standing_ = order;
    }
}

bool RoomMachine::HostLeftRace(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phase_ != Phase::Racing || cpusOut_) {
        return false;
    }
    cpusOut_ = true;
    roomDirty_ = true;
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::ReportFinish(uint8_t id, const FinishReport& report, uint32_t nowMs, std::vector<Outgoing>& out) {
    const PlayerInfo* p = Find(id);
    if (p == nullptr || phase_ != Phase::Racing || !p->inRace || report.raceIndex != raceIndex_ || HasReport(id)) {
        return false;
    }
    // Racing starts at GO on the host clock, so this is the host's own count of the race.
    const uint32_t raced = Elapsed(nowMs, phaseStartMs_);
    Report taken;
    taken.id = id;
    taken.kind = report.kind;
    if (report.kind == ResultKind::Finished) {
        if (report.timeMs == 0 || seats_[id].lap < static_cast<int16_t>(grid_.rules.laps) ||
            report.timeMs > raced + kFinishAheadMs) {
            return false;
        }
        const uint32_t earliest = raced > kFinishBehindMs ? raced - kFinishBehindMs : 1;
        taken.timeMs = std::min(std::max(report.timeMs, earliest), kRaceTimeMaxMs);
    } else if (report.kind != ResultKind::Retired) {
        return false;
    }
    bool first = taken.kind == ResultKind::Finished;
    for (const Report& r : reports_) {
        first = first && r.kind != ResultKind::Finished;
    }
    taken.serial = ++reportSerial_;
    reports_.push_back(taken);

    if (AllReported()) {
        FinishRace(nowMs, out);
    } else {
        if (first && raced + config_.afterWinnerMs < phaseMs_) {
            phaseMs_ = raced + config_.afterWinnerMs; // the others have this long, and the cap still holds
            roomDirty_ = true;
        }
        results_ = BuildResults(false);
        out.push_back({ kEveryone, kMsgResults, EncodeResults(results_) });
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::FinishRace(uint32_t nowMs, std::vector<Outgoing>& out) {
    results_ = BuildResults(true);
    lastOrder_.clear();
    for (const ResultEntry& e : results_.entries) {
        lastOrder_.push_back(e.id);
        PlayerInfo* p = Find(e.id);
        if (p != nullptr && e.points != 0) {
            const uint32_t sum = static_cast<uint32_t>(p->points) + e.points;
            p->points = static_cast<uint16_t>(sum > 0xFFFF ? 0xFFFF : sum);
            rosterDirty_ = true;
        }
    }
    out.push_back({ kEveryone, kMsgResults, EncodeResults(results_) });
    seriesRaces_++;
    SetPhase(Phase::Results, config_.resultsMs, nowMs);
}

void RoomMachine::LeaveResults() {
    lastTrack_ = track_ != kTrackNone ? track_ : lastTrack_;
    raceIndex_++;
}

bool RoomMachine::EndSeries(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phase_ != Phase::Results) {
        return false;
    }
    LeaveResults();
    ClearRaceFlags();
    track_ = kTrackNone;
    picker_ = kSystemId;
    SetPhase(Phase::Lobby, 0, nowMs);
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::SetRacerCap(uint8_t cap, uint32_t nowMs, std::vector<Outgoing>& out) {
    if (cap < kDefaultRacers || cap > kMaxRacers || cap < Racers()) {
        return false;
    }
    if (cap != racerCap_) {
        racerCap_ = cap;
        roomDirty_ = true;
    }
    Flush(nowMs, out);
    return true;
}

bool RoomMachine::SetCustomTracks(bool on, uint32_t nowMs, std::vector<Outgoing>& out) {
    if (on != customTracks_) {
        customTracks_ = on;
        roomDirty_ = true;
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::SetPing(uint8_t id, uint16_t pingMs) {
    PlayerInfo* p = Find(id);
    if (p != nullptr && p->pingMs != pingMs) {
        p->pingMs = pingMs;
        rosterDirty_ = true;
    }
}

bool RoomMachine::Emote(uint8_t id, uint8_t emote, uint32_t nowMs, std::vector<Outgoing>& out) {
    if (Find(id) == nullptr || emote >= kEmoteCount) {
        return false;
    }
    if (!EmoteAllowed(seats_[id].emoteTimes, nowMs)) {
        return false;
    }
    EmoteLine line;
    line.from = id;
    line.emote = emote;
    out.push_back({ kEveryone, kMsgEmoteLine, EncodeEmoteLine(line) });
    return true;
}

bool RoomMachine::SeatNext(uint8_t id, bool raceNext, uint32_t nowMs, std::vector<Outgoing>& out) {
    PlayerInfo* p = Find(id);
    if (p == nullptr || !p->watcher) {
        return false;
    }
    if (p->raceNext != raceNext) {
        p->raceNext = raceNext;
        seats_[id].nextSerial = raceNext ? ++nextSerial_ : 0;
        rosterDirty_ = true;
    }
    Flush(nowMs, out);
    return true;
}

void RoomMachine::OnTimeout(uint32_t nowMs, std::vector<Outgoing>& out) {
    switch (phase_) {
    case Phase::Lobby:
        break;
    case Phase::Voting:
        CloseVote(nowMs, out); // a missing ballot is an abstention
        break;
    case Phase::Lottery:
        EnterMachine(nowMs);
        break;
    case Phase::Pick:
        ChooseDrawn(kSystemId, nowMs, out);
        break;
    case Phase::Machine:
        LeaveMachine(nowMs, out);
        break;
    case Phase::GridSync:
        CloseGrid(nowMs, out);
        break;
    case Phase::Assets:
        CloseAssets(nowMs, out);
        break;
    case Phase::Countdown:
        SetPhase(Phase::Racing, config_.raceCapMs, nowMs);
        break;
    case Phase::Racing:
        FinishRace(nowMs, out); // the cap, or the time the others had after the first finish
        break;
    case Phase::Results:
        LeaveResults();
        BeginTrackChoice(nowMs, out);
        break;
    }
}

void RoomMachine::Update(uint32_t nowMs, std::vector<Outgoing>& out) {
    if (phaseMs_ != 0 && Elapsed(nowMs, phaseStartMs_) >= phaseMs_) {
        OnTimeout(nowMs, out);
    }
    Flush(nowMs, out);
}

} // namespace gdx::net::lobby
