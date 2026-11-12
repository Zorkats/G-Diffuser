// port/net/gdx_net_room.h: the room state machine of milestone O3 (design document section 3.4).
//
// Pure: no socket, no clock, no game header, no thread. The host owns one RoomMachine. Every call takes the
// current time in milliseconds and appends the messages to send to `out`; the caller (LobbyHost) moves them
// over the channel. That keeps every state, seat rule and timeout testable on a fake clock.
//
// The states, in order: LOBBY, then VOTING and LOTTERY with four or more racers or PICK with fewer, then
// MACHINE (the lock-in screen), ASSETS when a racer still lacks a custom track, GRID_SYNC, COUNTDOWN, RACING
// and RESULTS, and back to the next vote or pick. Every wait has a timeout, and a player who misses one
// watches the race instead of stalling it.
//
// Custom tracks (O4.6). A ballot or a pick may name one of the voter's own tracks while the host's switch is
// on; that voter is the track's owner. The caller moves the bytes (gdx_net_transfer.h) from the moment the
// track is known and tells the room who holds them: the owner from the start, the host once its copy of the
// owner's upload verified, a guest once it said Have. When MACHINE ends with a racer or the host still
// without the track, ASSETS waits up to assetsMs for them (decision 9). At its end a racer without the track
// watches this race. If the host never gets the track, because the owner's upload failed, did not verify or
// the owner left, the host draws a stock track instead and the locked machines stand (decision 11).
//
// How a race ends (D21). Each player's own game counts its laps and times its race, and tells the host
// when it finished or retired. The host takes a finish only from a racer whose own poses reached the last
// lap, and only with a time its own count of the race allows. The race is over when every racer has a
// result, kAfterWinnerMs after the first finish, or at the cap, whichever comes first. The table lists the
// finishers by time, then those still driving in the host game's order, then the retired; the place in it
// is worth PlacePoints, and the points add up over the races of a series.
//
// Nobody can end a race for the others. The host player may leave the race like anyone (HostLeftRace): the
// CPU machines, which only the host's game drives, are then out of that race, and those still driving at
// its end are placed by the lap of their latest pose first.
//
// Seats: race seats up to the room's cap, and kMaxWatchers watch-only seats on top. A watcher may ask to
// race next and takes a free race seat when the next lock-in screen opens, first come first served.
//
// The track draws are functions of public values (the room id, the race index, the ballots), so a client
// can check the host's lottery with RunLottery. They are not secret: a player who votes last could compute
// which ballot wins. An Open room therefore commits to each lottery (decision 39, gdx_net_lottery.h): the
// commitment travels in `room` while the vote is open, the lottery is drawn from the salt, and `lottery`
// reveals the salt so every copy checks both with CheckLottery.

#ifndef GDX_NET_ROOM_H
#define GDX_NET_ROOM_H

#include "gdx_net_lobby.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace gdx::net::lobby {

// Timer lengths. They travel in RoomState, so a client never hard-codes one.
constexpr uint32_t kVoteMs = 30000;    // closes early when every racer has voted (D15)
constexpr uint32_t kLotteryMs = 3000;  // the host may skip it
constexpr uint32_t kPickMs = 15000;    // then the host draws a track
constexpr uint32_t kMachineMs = 60000; // the lock-in screen (D24)
// After the lock-in screen, the wait for racers who still lack a custom track (decision 9).
constexpr uint32_t kAssetsMs = 20000;
constexpr uint32_t kGridMs = 5000;     // a racer who has not loaded the track by then watches
// The start message leaves this long before intro tick 460, so it reaches every copy first.
constexpr uint32_t kStartLeadMs = 1000;
// The race intro: the notice above plus the 420 ticks from intro tick 460 to GO, at 60 ticks a second.
constexpr uint32_t kCountdownMs = kStartLeadMs + 7000;
constexpr uint32_t kRaceCapMs = 300000; // 5:00
constexpr uint32_t kResultsMs = 12000;  // never waits for a button
// Once somebody has finished, the others have this long.
constexpr uint32_t kAfterWinnerMs = 60000;
// The host counts the race from GO on its own clock. A finish time further ahead of that count than this is
// refused: the race has not lasted that long. One further behind it is moved up to this far behind, so a
// game that runs slow, or a report held back, gains nothing.
constexpr uint32_t kFinishAheadMs = 1000;
constexpr uint32_t kFinishBehindMs = 1000;

// Under this many racers nobody votes: last place picks (D13).
constexpr size_t kVoteMinRacers = 4;
// Preset messages one player may send per window.
constexpr size_t kEmoteBurst = 5;
constexpr uint32_t kEmoteWindowMs = 5000;
// Outgoing::to for a message every player gets.
constexpr uint8_t kEveryone = 0xFE;

struct RoomConfig {
    uint32_t roomId = 1; // a fresh random value per room
    uint32_t pool = kPoolAll;
    uint32_t voteMs = kVoteMs;
    uint32_t lotteryMs = kLotteryMs;
    uint32_t pickMs = kPickMs;
    uint32_t machineMs = kMachineMs;
    uint32_t assetsMs = kAssetsMs;
    uint32_t gridMs = kGridMs;
    uint32_t startLeadMs = kStartLeadMs;
    uint32_t countdownMs = kCountdownMs;
    bool cpuFill = true; // CPUs take every grid slot the humans leave free (D2)
    uint32_t raceCapMs = kRaceCapMs;
    uint32_t resultsMs = kResultsMs;
    uint32_t afterWinnerMs = kAfterWinnerMs;
    RoomType type = RoomType::Lan; // O6.1
    // The secret each race's lottery salt comes from (gdx_net_lottery.h, RaceSalt). An Open room with a key
    // commits to its lotteries; all zero, or any other room type, draws from public values as before.
    Bytes32 lotteryKey{};
    // The directory code once the room is listed (O5.10), or kNoRoomCode. SetCode changes it later.
    uint32_t code = kNoRoomCode;
};

// The host's CPU settings (G-NET decision 24). Only the host runs CPUs, but they travel in `room` so a
// successor runs them the same way.
struct CpuOptions {
    bool fill = true;                             // CPUs take every grid slot the humans leave free (D2)
    uint16_t aiAggression = kAiAggressionDefault; // kAiAggressionMin to kAiAggressionMax
    bool trackHazards = false;

    bool operator==(const CpuOptions& o) const {
        return fill == o.fill && aiAggression == o.aiAggression && trackHazards == o.trackHazards;
    }
};

// What a member's copy holds of the room, from the host's messages: enough for its successor to run it
// (D28.3). The room is as of the moment the mirror was taken, its phase timer run down.
struct RoomMirror {
    std::vector<PlayerInfo> roster;
    RoomState room;
    RaceResults results; // the last table the host sent; its race index says which race
    bool haveResults = false;
};

// A player's id once `successor` hosts (decision 20): the successor becomes kHostId, the old host is gone
// (kSystemId), and everyone else keeps theirs. Every copy remaps with it, so one roster entry moves.
uint8_t HandoverId(uint8_t id, uint8_t successor);

// One message for the caller to send: to a player id, or to kEveryone.
struct Outgoing {
    uint8_t to = kEveryone;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

// hash(roomId, raceIndex, ballots): the seed of every draw. The ballots must be sorted by player id.
uint32_t RoomHash(uint32_t roomId, uint16_t raceIndex, const std::vector<Ballot>& ballots);

// The pool's track that hash selects, never `avoid` while the pool holds another one. 0 for an empty pool.
uint8_t DrawTrack(uint32_t hash, uint32_t pool, uint8_t avoid);

// The track the host draws when nobody chooses: a series' first race under four racers, a pick that timed
// out, a Random pick. Never the track just raced while the pool holds another one.
uint8_t DrawnTrack(uint32_t roomId, uint16_t raceIndex, uint32_t pool, uint8_t lastTrack);

// The lottery: one of the ballots cast wins, so a track's odds are its share of the votes. A Random ballot
// that wins, and a vote nobody cast a ballot in, draw from the pool as DrawnTrack does. The ballots are
// sorted by player id first, so the host and every client reach the same result.
// The committed lottery of an Open room (decision 39): the same rule, with the draw seed of the salt in place
// of RoomHash, and the salt in the result.
LotteryResult RunCommittedLottery(const Bytes32& salt, uint32_t roomId, uint16_t raceIndex, std::vector<Ballot> ballots,
                                  uint32_t pool, uint8_t lastTrack);
// A client's check of the host's lottery against the room it was drawn in: with a commitment, the salt must
// open it and the committed lottery must give this result; without one, the salt is zero, the room is not
// Open, and RunLottery gives this result.
bool CheckLottery(const RoomState& room, const LotteryResult& lottery);
LotteryResult RunLottery(uint32_t roomId, uint16_t raceIndex, std::vector<Ballot> ballots, uint32_t pool,
                         uint8_t lastTrack);

// The preset message rate limit. true, with nowMs recorded in times, while the sender is under kEmoteBurst
// messages in the last kEmoteWindowMs. The host enforces it; a client runs the same check to tell its player.
bool EmoteAllowed(std::deque<uint32_t>& times, uint32_t nowMs);

class RoomMachine {
public:
    // host: the host's name, colour and profile machine. It becomes player 0 in a race seat.
    RoomMachine(const RoomConfig& config, const PlayerInfo& host);

    // The successor's room (D28.3), from its own mirror. The successor becomes player 0 in a race seat, with
    // its points; the old host is gone; everyone else keeps their id, seat and points, and is still to come
    // back (the caller admits them). The room id, the race index, the pool and every setting carry over.
    //
    // Where the room goes on: the lobby stays the lobby, and a results page shown after a final table runs out
    // its timer. Anything else was a race being chosen, set up or run, which nobody wins: the next track
    // choice starts at once. A race whose grid could have been sent is skipped by its index, so a late
    // report for it is refused. Join order does not travel, so the successor after this one follows id order.
    //
    // nullopt when the successor is the old host or not in the roster.
    // lotteryKey: the successor's own fresh key, since the old host's never leaves it (RoomConfig).
    static std::optional<RoomMachine> Promote(const RoomMirror& mirror, uint8_t successor, uint32_t nowMs,
                                              const Bytes32& lotteryKey = Bytes32{});

    // Seats a player. Race seats and watch seats fill separately; a full kind is refused with the reason in
    // why. On success id is the new player's id, every player gets the roster and the room, and the joiner
    // also gets the ballots of a vote in progress or the lottery being shown. A racer who joins once the
    // lock-in screen has opened watches that race.
    bool Join(const JoinRequest& join, uint32_t nowMs, uint8_t& id, RejectReason& why, std::vector<Outgoing>& out);
    // Frees the seat. The host cannot leave: closing the room is the caller's job.
    void Leave(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out);
    // A member a promoted host kept a seat for is back on a link (D28.4). Everyone gets the roster and the
    // room, and the member also gets what a joiner gets. Ignored for the host and for an id not seated.
    void Rejoined(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out);

    // From a player, the host included. Each returns false, and changes nothing, when the call is not allowed
    // in the current state or for that seat.

    // The lobby's ready mark. When two or more racers are in and every one is ready, the series starts.
    bool SetReady(uint8_t id, bool ready, uint32_t nowMs, std::vector<Outgoing>& out);
    // One ballot per racer, final. The vote closes when every racer has cast one. A custom track is taken
    // only while the host's switch is on, and makes the voter its owner should the ballot win.
    bool Vote(uint8_t id, const TrackChoice& choice, uint32_t nowMs, std::vector<Outgoing>& out);
    bool Vote(uint8_t id, uint8_t track, uint32_t nowMs, std::vector<Outgoing>& out);
    // Only the picker, only in Phase::Pick. kTrackRandom asks the host to draw. A custom track as in Vote.
    bool Pick(uint8_t id, const TrackChoice& choice, uint32_t nowMs, std::vector<Outgoing>& out);
    bool Pick(uint8_t id, uint8_t track, uint32_t nowMs, std::vector<Outgoing>& out);
    // The lock-in screen: once per race, only by a racer on the grid, only in Phase::Machine.
    bool Lock(uint8_t id, const LockRequest& lock, uint32_t nowMs, std::vector<Outgoing>& out);
    // A preset message, relayed to everyone. false when the player is over the rate limit.
    bool Emote(uint8_t id, uint8_t emote, uint32_t nowMs, std::vector<Outgoing>& out);
    // A watcher asks for a race seat at the next lock-in screen, or takes the request back.
    bool SeatNext(uint8_t id, bool raceNext, uint32_t nowMs, std::vector<Outgoing>& out);
    // A racer on the grid has loaded the track. The host copy says so like everyone else, and its digest
    // is the one the others are held to: a racer whose track differs watches this race.
    bool GridReady(uint8_t id, const ReadyToRace& ready, uint32_t nowMs, std::vector<Outgoing>& out);
    // A racer says how its own race ended: once per race, only while it runs. Retiring is always taken. A
    // finish is refused when the racer's poses never reached the last lap, or when the time is further ahead
    // of the host's count of the race than kFinishAheadMs. Everyone gets the table as it stands, and the
    // final one when this was the last result missing.
    bool ReportFinish(uint8_t id, const FinishReport& report, uint32_t nowMs, std::vector<Outgoing>& out);

    // From the host's own copy of the race.

    // The lap the racer's latest pose named: what a finish is held to.
    void NoteLap(uint8_t id, int16_t lap);
    // The host game's order of the humans, first to last. It places those still driving when the race ends.
    void SetStanding(const std::vector<uint8_t>& order);
    // The host's game left the race that is running, so nothing drives the CPU machines or sends a standing
    // any more. Everyone is told once (RoomState::cpusOut). false outside a race, and the second time.
    bool HostLeftRace(uint32_t nowMs, std::vector<Outgoing>& out);

    // From the host only.

    // Starts a series from the lobby. One racer is enough.
    bool Start(uint32_t nowMs, std::vector<Outgoing>& out);
    bool SkipLottery(uint32_t nowMs, std::vector<Outgoing>& out);
    // From the results back to the lobby.
    bool EndSeries(uint32_t nowMs, std::vector<Outgoing>& out);
    // kDefaultRacers to kMaxRacers, and never below the racers already seated.
    bool SetRacerCap(uint8_t cap, uint32_t nowMs, std::vector<Outgoing>& out);
    // The host's switch for custom tracks (D8). Ballots and picks already made stay as they are.
    bool SetCustomTracks(bool on, uint32_t nowMs, std::vector<Outgoing>& out);
    // The rules of the next race. Refused once a grid has been sent, and for values outside their limits.
    // They travel in the next `room`, which Update sends.
    bool SetRules(const RaceRules& rules);
    // The CPU settings. Refused once a grid has been sent, and for an aggression outside its limits.
    bool SetCpuOptions(const CpuOptions& cpu, uint32_t nowMs, std::vector<Outgoing>& out);
    // The directory code, once the room is listed under it (O5.10). Refused in a LAN room and for a value that is
    // not a code. It travels in the next `room`.
    bool SetCode(uint32_t code) {
        if (config_.type == RoomType::Lan || (code != kNoRoomCode && code >= kRoomCodeValues)) {
            return false;
        }
        roomDirty_ = roomDirty_ || config_.code != code;
        config_.code = code;
        return true;
    }
    const CpuOptions& Cpu() const {
        return cpu_;
    }
    // Where the host reaches a member, as its link sees it. Only a member with an address can be the
    // successor (decision 18), so the caller sets it right after a join. Ignored for the host and for an
    // id that is not seated.
    void SetAddress(uint8_t id, const IpBytes& ip, uint16_t port);
    // Whether the member can take inbound connections (decision 51): from its Join, then from each Reach. Ignored
    // for the host and for an id that is not seated.
    void SetReachable(uint8_t id, bool reachable);
    // The next host (decision 19): the first racer in join order with an address, else the first such
    // watcher; kSystemId when nobody can take over. Outside a LAN room a reachable member goes first
    // (decision 51). Announced in `room` whenever it changes.
    uint8_t Successor() const;
    // The player's round trip time. It travels with the next roster, which Update sends.
    void SetPing(uint8_t id, uint16_t pingMs);

    // The custom track of the race being set up (O4.6). The caller learns from these what to move.

    // The player holds the custom track of this race: a guest said Have, or the host's copy of the owner's
    // upload verified. Only in MACHINE and ASSETS, only for a custom track. ASSETS ends as soon as the host
    // and every racer hold it.
    bool TrackHeld(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out);
    // The host cannot get the track: the owner's upload failed or did not verify, or the host named a track
    // it cannot load. The host draws a stock track (decision 11). false when no custom track is being set up,
    // or once the grid is out.
    bool TrackFailed(uint32_t nowMs, std::vector<Outgoing>& out);
    bool HoldsTrack(uint8_t id) const;

    // The track of the race being set up or run, with its identity when it is custom.
    TrackChoice CurrentTrack() const;
    // Who named the custom track; kSystemId for a stock track or none.
    uint8_t TrackOwner() const {
        return trackOwner_;
    }

    // Runs the phase timer. Call it once per tick.
    void Update(uint32_t nowMs, std::vector<Outgoing>& out);

    const std::vector<PlayerInfo>& Roster() const {
        return roster_;
    }

    RoomState State(uint32_t nowMs) const;

    Phase GetPhase() const {
        return phase_;
    }

    // The ballots of the vote in progress, or of the last one.
    const std::vector<Ballot>& Votes() const {
        return votes_;
    }

    const LotteryResult& Lottery() const {
        return lottery_;
    }

    const Picked& LastPicked() const {
        return picked_;
    }

    // The last race's order, first to last.
    const std::vector<uint8_t>& LastOrder() const {
        return lastOrder_;
    }

    // The table last sent: of the race being run, or of the last one.
    const RaceResults& Results() const {
        return results_;
    }

    // The grid of the race being loaded or run, as sent when the lock-in screen closed.
    const Grid& LastGrid() const {
        return grid_;
    }

    const RaceRules& Rules() const {
        return rules_;
    }

    // What the racer reported with its ready message; 0 before it.
    uint16_t LoadMs(uint8_t id) const;

    size_t Racers() const;
    size_t Watchers() const;
    // The engine setting of the player's last lock, or 50 before any.
    uint8_t Engine(uint8_t id) const;

private:
    struct SeatData {
        bool gridReady = false;
        std::array<uint8_t, kDigestBytes> digest{};
        uint16_t loadMs = 0;
        uint32_t nextSerial = 0; // the order "race next" was asked in; 0 when it was not
        uint8_t engine = 50;
        bool holdsTrack = false; // the custom track of the race being set up
        // The racer's own Create Machine slot, from its lock in this race only (decision 10).
        bool hasRecord = false;
        MachineRecord record{};
        int16_t lap = 0; // of the racer's latest pose in the race being run
        std::deque<uint32_t> emoteTimes;
        uint32_t joinSerial = 0; // the order players joined in (decision 19)
        IpBytes ip{};            // the member's address as the host's link sees it; port 0 when unknown
        uint16_t port = 0;
        bool reachable = false;  // decision 51
    };

    // How one racer's race ended, as the host took it.
    struct Report {
        uint8_t id = 0;
        ResultKind kind = ResultKind::Finished;
        uint32_t timeMs = 0;
        uint32_t serial = 0; // the order the reports were taken in
    };

    PlayerInfo* Find(uint8_t id);
    const PlayerInfo* Find(uint8_t id) const;
    uint8_t FreeId() const;
    size_t OnGrid() const;
    bool AllVoted() const;
    bool AllLocked() const;
    bool AllGridReady() const;
    bool AllHeld() const;
    bool HasReport(uint8_t id) const;
    bool AllReported() const;
    uint8_t LastPlace() const;
    RaceResults BuildResults(bool final) const;

    void SetPhase(Phase phase, uint32_t lengthMs, uint32_t nowMs);
    void ClearRaceFlags();
    void BeginTrackChoice(uint32_t nowMs, std::vector<Outgoing>& out);
    void ChooseDrawn(uint8_t by, uint32_t nowMs, std::vector<Outgoing>& out);
    void CloseVote(uint32_t nowMs, std::vector<Outgoing>& out);
    void EnterMachine(uint32_t nowMs);
    void LeaveMachine(uint32_t nowMs, std::vector<Outgoing>& out);
    void SendGrid(uint32_t nowMs, std::vector<Outgoing>& out);
    void CloseAssets(uint32_t nowMs, std::vector<Outgoing>& out);
    void FallBack(uint32_t nowMs, std::vector<Outgoing>& out);
    bool ChoiceOk(const TrackChoice& choice) const;
    void KnowTrack(const CustomTrack& custom, uint8_t owner);
    void CloseGrid(uint32_t nowMs, std::vector<Outgoing>& out);
    void EnterCountdown(uint32_t nowMs, std::vector<Outgoing>& out);
    void FinishRace(uint32_t nowMs, std::vector<Outgoing>& out);
    void LeaveResults();
    void OnTimeout(uint32_t nowMs, std::vector<Outgoing>& out);
    void Flush(uint32_t nowMs, std::vector<Outgoing>& out);
    // Sends the roster and the room to everyone, and to id the ballots, lottery or table of the phase.
    void CatchUp(uint8_t id, uint32_t nowMs, std::vector<Outgoing>& out);

    RoomConfig config_;
    std::vector<PlayerInfo> roster_; // sorted by id
    std::array<SeatData, kMaxPlayers> seats_;
    Phase phase_ = Phase::Lobby;
    uint32_t phaseStartMs_ = 0;
    uint32_t phaseMs_ = 0; // 0: the phase has no timer
    uint8_t racerCap_ = static_cast<uint8_t>(kDefaultRacers);
    bool customTracks_ = false;
    uint16_t raceIndex_ = 0;    // every race of this room, so two series never draw alike
    uint32_t seriesRaces_ = 0;  // races finished in this series
    uint32_t nextSerial_ = 0;
    uint32_t joinSerial_ = 0;
    CpuOptions cpu_;
    uint8_t announcedSuccessor_ = kSystemId; // the successor in the last `room` sent
    uint8_t track_ = kTrackNone;
    CustomTrack custom_;                // when track_ is kTrackCustom
    uint8_t trackOwner_ = kSystemId;    // who named the custom track
    uint8_t lastTrack_ = kTrackNone;
    uint8_t picker_ = kSystemId;
    std::vector<Ballot> votes_;
    LotteryResult lottery_;
    Bytes32 salt_{};       // this race's lottery salt, while the room is committed to it
    Bytes32 commitment_{}; // its commitment, sent in `room`; all zero when the room did not commit
    Picked picked_;
    std::vector<uint8_t> lastOrder_;
    std::vector<Report> reports_;   // of the race being run
    std::vector<uint8_t> standing_; // the host game's order of the humans
    bool cpusOut_ = false;          // the host's game left the race being run
    uint32_t reportSerial_ = 0;
    RaceResults results_;
    RaceRules rules_;
    Grid grid_;
    bool rosterDirty_ = false;
    bool roomDirty_ = false;
};

} // namespace gdx::net::lobby

#endif // GDX_NET_ROOM_H
