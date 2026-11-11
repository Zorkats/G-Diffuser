// port/net/gdx_net_lobby.h: the LAN room wire format (version 2), LAN discovery datagrams and the chat filter.
//
// Pure: no socket, no clock, no game header. gdx_net_lobby_session.* moves these bytes over the reliable
// channel (gdx_net_channel.h) and gdx_net_room.* decides what the room does with them; this file only builds
// and checks them. Every decoder reads lengths before bytes, refuses anything short, long or out of range,
// and refuses trailing bytes, so a decoder either returns a whole valid message or false and leaves its
// output unchanged.
//
// Room messages ride the reliable channel as Message{type, payload}. All integers are little-endian.
// A string is u8 length + bytes. A track is a stock course index below kStockTracks.
//
//    1 Join         client to host  build, name, u32 colour (0x00RRGGBB), u8 machine, u8 Seat
//    2 Welcome      host to client  u8 player id
//    3 Reject       host to client  u8 RejectReason; the host drops the link right after
//    4 Roster       host to all     u8 count, count x { u8 id, u8 flags, u32 colour, u8 machine, u8 skin,
//                                   u16 ping ms, u16 series points, name }
//    5 Ready        client to host  u8 0 or 1
//    6, 7           retired: the typed Chat and ChatLine of version 1. Never reuse these numbers.
//    8 Room         host to all     RoomState, see EncodeRoomState
//    9 Vote         client to host  u8 track, or kTrackRandom
//   10 Votes        host to all     u8 count, count x { u8 id, u8 track }: the ballots cast so far
//   11 Lottery      host to all     the final ballots as in Votes, u8 winner (index into them, or kNoWinner),
//                                   u8 track
//   12 Pick         client to host  u8 track, or kTrackRandom
//   13 Picked       host to all     u8 track, u8 drawn (0 chosen, 1 drawn by the host), u8 by (id, or kSystemId)
//   14 Lock         client to host  u8 machine, u8 skin, u8 engine (0 to kEngineMax)
//   15 Emote        client to host  u8 preset message id
//   16 EmoteLine    host to all     u8 from id, u8 preset message id
//   17 SeatNext     client to host  u8 0 or 1: a watcher asks for a race seat at the next grid, or takes it back
//   18 ReadyToRace  client to host  u16 race index, 32 bytes course digest, u16 load time in ms
//   19 Grid         host to all     the race about to load, see EncodeGrid: u16 race index, u32 seed, u8 laps,
//                                   u8 rule flags, u32 magnet range, u32 magnet strength (float bits), u8 CPUs,
//                                   u8 count, count x { u8 id, u8 machine, u8 skin }, the humans from the
//                                   front of their rows to the back
//   20 Start        host to all     u16 race index, u32 host clock (ms) at which intro tick 460 runs
//   21 ClockPing    client to host  u32 client clock (ms). Unreliable: a late answer is a bad sample
//   22 ClockPong    host to client  u32 the client clock of the ping, u32 host clock (ms). Unreliable
//   23 State        both ways       u16 race index, u8 count (1 to kStatePoses), count x { u8 race slot,
//                                   u32 the owner's race tick, u32 host clock at that tick (ms), 3 x u32
//                                   position, 3 x u32 velocity, 3 x u32 forward, 3 x u32 up (float bits),
//                                   s16 lap, u32 racer state flags, u8 attack state }. Unreliable: a pose
//                                   that is late is worth less than the next one. A client sends its own
//                                   machine; the host sends its own, the CPUs', and every other player's
//   24 Finish       client to host  u16 race index, u8 ResultKind (Finished or Retired), u32 race time in ms
//                                   as the player's own game counted it (0 for Retired)
//   25 Results      host to all     u16 race index, u8 final (0 the race goes on, 1 it is over), u8 count,
//                                   count x { u8 id, u8 ResultKind, u32 time in ms, u8 points }, the humans
//                                   of the race from first to last
//
// Discovery datagrams travel outside the channel, on the same socket. They start with "GDXL", which the
// channel never sends (its packets start with "GD" and a type byte from 0 to 4), so the host tells them
// apart by the first four bytes.
//
//   Probe   "GDXL" u8 1, u16 kLobbyVersion, u32 nonce
//   Room    "GDXL" u8 2, u16 kLobbyVersion, u32 nonce (the probe's), u16 port, u8 racers, u8 racer cap,
//           u8 watchers, u8 Phase, build, host name
//
// Names follow the profile rule (gdx_profile_name_ok): 1 to kNameMax printable ASCII bytes, no space at
// either end. No typed text travels in a room: players talk with the preset messages (EmoteText).

#ifndef GDX_NET_LOBBY_H
#define GDX_NET_LOBBY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx::net::lobby {

constexpr uint16_t kDefaultPort = 24302;
constexpr uint16_t kLobbyVersion = 2;
// Race seats: 8 unless the host raises the cap, 12 at most (owner decision D12).
constexpr size_t kDefaultRacers = 8;
constexpr size_t kMaxRacers = 12;
// Watch-only seats, on top of the race seats (D14).
constexpr size_t kMaxWatchers = 2;
// Everyone in a room, the host included. Player ids are below this.
constexpr size_t kMaxPlayers = kMaxRacers + kMaxWatchers;
constexpr size_t kNameMax = 16;
constexpr size_t kBuildMax = 64;
constexpr size_t kChatMax = 120;
// The from id of a room notice ("Ana joined"), and "nobody" where a message names a player.
constexpr uint8_t kSystemId = 0xFF;
// The host is always player 0.
constexpr uint8_t kHostId = 0;

// The cartridge's 24 courses, Jack Cup first. The X Cup has no fixed course to vote for.
constexpr size_t kStockTracks = 24;
constexpr uint32_t kPoolAll = (1u << kStockTracks) - 1u;
constexpr uint8_t kTrackRandom = 0xFF; // a ballot or pick that asks for a draw
constexpr uint8_t kTrackNone = 0xFE;   // no track chosen yet
constexpr uint8_t kNoWinner = 0xFF;    // a lottery nobody voted in

constexpr size_t kRosterMachines = 30;
constexpr uint8_t kNoMachine = 0xFF;
constexpr size_t kMachineSkins = 4; // the colour schemes of one stock machine
constexpr uint8_t kEngineMax = 100; // the engine slider's position, as a percentage of its travel
constexpr size_t kEmoteCount = 8;

enum MessageType : uint8_t {
    kMsgJoin = 1,
    kMsgWelcome = 2,
    kMsgReject = 3,
    kMsgRoster = 4,
    kMsgReady = 5,
    // 6 and 7 carried typed chat in version 1.
    kMsgRoom = 8,
    kMsgVote = 9,
    kMsgVotes = 10,
    kMsgLottery = 11,
    kMsgPick = 12,
    kMsgPicked = 13,
    kMsgLock = 14,
    kMsgEmote = 15,
    kMsgEmoteLine = 16,
    kMsgSeatNext = 17,
    kMsgReadyToRace = 18,
    kMsgGrid = 19,
    kMsgStart = 20,
    kMsgClockPing = 21,
    kMsgClockPong = 22,
    kMsgState = 23,
    kMsgFinish = 24,
    kMsgResults = 25,
};

// Wire values. Never renumber them.
enum class RejectReason : uint8_t {
    Build = 1,          // a different G-Diffuser build
    Full = 2,           // no seat of either kind is free
    BadProfile = 3,     // the name or machine breaks the rules
    Kicked = 4,         // the host removed this player
    RaceSeatsFull = 5,  // asked to race; only watch seats are free
    WatchSeatsFull = 6, // asked to watch; only race seats are free
};

const char* RejectReasonText(RejectReason reason);

// Wire values.
enum class Seat : uint8_t {
    Race = 0,
    Watch = 1,
};

// Wire values. The room's state, in the order a race goes through them (design document section 3.4).
enum class Phase : uint8_t {
    Lobby = 0,     // waiting for the host to start
    Voting = 1,    // four or more racers: each picks a track
    Lottery = 2,   // the winning ballot is shown
    Pick = 3,      // under four racers: last place picks
    Machine = 4,   // the lock-in screen
    GridSync = 5,  // every racer loads the track
    Countdown = 6, // the race intro
    Racing = 7,
    Results = 8,
};
constexpr uint8_t kPhaseCount = 9;

const char* PhaseName(Phase phase);

struct PlayerInfo {
    uint8_t id = 0;
    bool host = false;
    bool ready = false;    // the lobby's ready mark
    bool watcher = false;  // holds a watch-only seat
    bool raceNext = false; // a watcher who asked for a race seat at the next grid
    bool inRace = false;   // on the grid of the race being set up or run
    bool locked = false;   // locked a machine in on the lock-in screen
    uint32_t colour = 0;
    uint8_t machine = kNoMachine;
    uint8_t skin = 0;
    uint16_t pingMs = 0;
    uint16_t points = 0; // of the series being run, or of the last one until the next starts
    std::string name;

    bool operator==(const PlayerInfo& o) const {
        return id == o.id && host == o.host && ready == o.ready && watcher == o.watcher && raceNext == o.raceNext &&
               inRace == o.inRace && locked == o.locked && colour == o.colour && machine == o.machine &&
               skin == o.skin && pingMs == o.pingMs && points == o.points && name == o.name;
    }
};

struct JoinRequest {
    std::string build;
    std::string name;
    uint32_t colour = 0;
    uint8_t machine = kNoMachine;
    Seat seat = Seat::Race;
};

// What the host tells everyone about the room. Sent whole on every change.
struct RoomState {
    uint32_t roomId = 0;        // random per room; part of the lottery hash
    uint16_t raceIndex = 0;     // counts the races of this room; part of the lottery hash
    Phase phase = Phase::Lobby;
    uint32_t phaseLeftMs = 0;   // time left in the phase when the message was sent; 0 with phaseTotalMs 0
    uint32_t phaseTotalMs = 0;  // the phase's whole timer; 0 when the phase has none
    uint8_t racerCap = static_cast<uint8_t>(kDefaultRacers);
    uint32_t pool = kPoolAll;   // bit n set: track n can be voted, picked or drawn
    uint8_t track = kTrackNone; // the track of the race being set up or run
    uint8_t lastTrack = kTrackNone;
    uint8_t picker = kSystemId; // who picks, in Phase::Pick
    uint8_t locked = 0;         // racers who locked a machine in
    uint8_t racers = 0;         // racers on the grid
    bool cpusOut = false;       // the host's game left this race: the CPU machines it drove are out of it

    bool operator==(const RoomState& o) const {
        return roomId == o.roomId && raceIndex == o.raceIndex && phase == o.phase && phaseLeftMs == o.phaseLeftMs &&
               phaseTotalMs == o.phaseTotalMs && racerCap == o.racerCap && pool == o.pool && track == o.track &&
               lastTrack == o.lastTrack && picker == o.picker && locked == o.locked && racers == o.racers &&
               cpusOut == o.cpusOut;
    }
};

struct Ballot {
    uint8_t id = 0;
    uint8_t track = kTrackRandom;

    bool operator==(const Ballot& o) const {
        return id == o.id && track == o.track;
    }
};

struct LotteryResult {
    std::vector<Ballot> ballots; // sorted by player id
    uint8_t winner = kNoWinner;  // index into ballots
    uint8_t track = kTrackNone;

    bool operator==(const LotteryResult& o) const {
        return ballots == o.ballots && winner == o.winner && track == o.track;
    }
};

struct Picked {
    uint8_t track = kTrackNone;
    bool drawn = false;     // the host drew it: a first race, a timeout, or a Random pick
    uint8_t by = kSystemId; // who picked, or kSystemId when nobody did

    bool operator==(const Picked& o) const {
        return track == o.track && drawn == o.drawn && by == o.by;
    }
};

struct LockRequest {
    uint8_t machine = 0;
    uint8_t skin = 0;
    uint8_t engine = 50;
};

struct EmoteLine {
    uint8_t from = 0;
    uint8_t emote = 0;
};

constexpr size_t kDigestBytes = 32;
// Every racer of a race, humans and CPUs: the grid of the game.
constexpr size_t kGridSlots = 30;
constexpr uint8_t kLapsDefault = 3;
constexpr uint8_t kLapsMax = 99;
// The limits of the Gameplay menu (racer.c clamps to the same ones).
constexpr float kMagnetRangeMin = 10.0f;
constexpr float kMagnetRangeMax = 200.0f;
constexpr float kMagnetStrengthMin = 0.25f;
constexpr float kMagnetStrengthMax = 3.0f;

// A racer tells the host its copy of the track is loaded, and which track that is.
struct ReadyToRace {
    uint16_t raceIndex = 0;
    std::array<uint8_t, kDigestBytes> digest{}; // of the control points of the course
    uint16_t loadMs = 0;                        // from leaving the lock-in screen to the loaded track

    bool operator==(const ReadyToRace& o) const {
        return raceIndex == o.raceIndex && digest == o.digest && loadMs == o.loadMs;
    }
};

// The host values of the options that change how a player drives their own machine. Every copy races on
// them and goes back to its own afterwards. The options that only steer CPUs stay with the host, which
// is the only copy that runs them.
struct RaceRules {
    uint8_t laps = kLapsDefault;
    bool ignoreTrackSettings = false;
    bool lavaNoKill = false;
    float magnetRange = 80.0f;
    float magnetStrength = 1.7f;

    bool operator==(const RaceRules& o) const {
        return laps == o.laps && ignoreTrackSettings == o.ignoreTrackSettings && lavaNoKill == o.lavaNoKill &&
               magnetRange == o.magnetRange && magnetStrength == o.magnetStrength;
    }
};

bool RulesOk(const RaceRules& rules);

struct GridEntry {
    uint8_t id = 0;
    uint8_t machine = 0;
    uint8_t skin = 0;

    bool operator==(const GridEntry& o) const {
        return id == o.id && machine == o.machine && skin == o.skin;
    }
};

// The race every copy is about to load: who is on the grid and in which order, how many CPUs fill it,
// the seed that deals the CPUs their machines, and the rules.
struct Grid {
    uint16_t raceIndex = 0;
    uint32_t seed = 0;
    RaceRules rules;
    uint8_t cpus = 0;
    std::vector<GridEntry> humans; // from the front of their rows to the back

    bool operator==(const Grid& o) const {
        return raceIndex == o.raceIndex && seed == o.seed && rules == o.rules && cpus == o.cpus && humans == o.humans;
    }
};

struct StartSignal {
    uint16_t raceIndex = 0;
    uint32_t startAtMs = 0; // on the host clock

    bool operator==(const StartSignal& o) const {
        return raceIndex == o.raceIndex && startAtMs == o.startAtMs;
    }
};

struct ClockPong {
    uint32_t clientMs = 0; // echoed from the ping
    uint32_t hostMs = 0;
};

// Poses in one State message. A field above this goes out as two messages, each whole on its own.
constexpr size_t kStatePoses = 18;
constexpr size_t kStateHeaderBytes = 3;
constexpr size_t kStatePoseBytes = 64;
// What a decoder lets through. A track is a few tens of thousands of units across, a machine at full boost
// moves well under a hundred units a tick, the two axes are unit vectors, and a race has at most kLapsMax
// laps; the room around each limit is for values in passing. The attack state is 0, 1 (side) or 2 (spin).
constexpr float kStatePosMax = 1000000.0f;
constexpr float kStateVelMax = 1000.0f;
constexpr float kStateAxisMax = 1.5f;
constexpr uint8_t kStateAttackMax = 2;
constexpr int16_t kStateLapMin = -1;
constexpr int16_t kStateLapMax = 120;

// One machine at one tick of its owner's race: the fields of GdxNetPose (gdx_net_race.h), with the race
// slot they belong to.
struct StatePose {
    uint8_t slot = 0;
    uint32_t tick = 0;
    uint32_t timeMs = 0; // the host clock when that tick ran, as its owner knew it
    std::array<float, 3> pos{};
    std::array<float, 3> vel{}; // units a tick
    std::array<float, 3> forward{};
    std::array<float, 3> up{};
    int16_t lap = 0;
    uint32_t flags = 0;
    uint8_t attack = 0;

    bool operator==(const StatePose& o) const {
        return slot == o.slot && tick == o.tick && timeMs == o.timeMs && pos == o.pos && vel == o.vel &&
               forward == o.forward && up == o.up && lap == o.lap && flags == o.flags && attack == o.attack;
    }
};

struct RaceState {
    uint16_t raceIndex = 0;
    std::vector<StatePose> poses;

    bool operator==(const RaceState& o) const {
        return raceIndex == o.raceIndex && poses == o.poses;
    }
};

// True when every number of the pose is one a machine can have: see the limits above.
bool StatePoseOk(const StatePose& pose);

// Wire values, and the order of a results table: finishers, then those still driving when the race
// ended, then those who retired.
enum class ResultKind : uint8_t {
    Finished = 0,
    Unfinished = 1,
    Retired = 2,
};

// No race lasts an hour: the room ends one after kRaceCapMs.
constexpr uint32_t kRaceTimeMaxMs = 3600000;
// What first place scores. The table is PlacePoints.
constexpr uint8_t kPointsMax = 100;

// The points of a place among the humans of a race, first place being 1: the game's own Grand Prix
// table (gRacePositionPoints in racer.c), so every copy scores alike. 0 for a place outside 1 to
// kMaxRacers.
uint8_t PlacePoints(size_t place);

// A player tells the host how its own race ended.
struct FinishReport {
    uint16_t raceIndex = 0;
    ResultKind kind = ResultKind::Finished; // Finished or Retired
    uint32_t timeMs = 0;                    // the game's own race time; 0 for Retired

    bool operator==(const FinishReport& o) const {
        return raceIndex == o.raceIndex && kind == o.kind && timeMs == o.timeMs;
    }
};

struct ResultEntry {
    uint8_t id = 0;
    ResultKind kind = ResultKind::Unfinished;
    uint32_t timeMs = 0; // the time the host settled on; 0 unless Finished
    uint8_t points = 0;  // what this race is worth to the player; 0 for Retired

    bool operator==(const ResultEntry& o) const {
        return id == o.id && kind == o.kind && timeMs == o.timeMs && points == o.points;
    }
};

// The humans of a race from first to last. While the race goes on it is where things stand; the final
// one is the result, and its points are the ones added to the series.
struct RaceResults {
    uint16_t raceIndex = 0;
    bool final = false;
    std::vector<ResultEntry> entries;

    bool operator==(const RaceResults& o) const {
        return raceIndex == o.raceIndex && final == o.final && entries == o.entries;
    }
};

// What a discovery reply says about a room.
struct RoomInfo {
    uint16_t port = 0;
    uint8_t racers = 0;
    uint8_t racerCap = 0;
    uint8_t watchers = 0;
    Phase phase = Phase::Lobby;
    std::string build;
    std::string hostName;

    bool RaceSeatFree() const {
        return racers < racerCap;
    }

    bool WatchSeatFree() const {
        return watchers < kMaxWatchers;
    }
};

// The profile name rule. Machine is a roster index below kRosterMachines, or kNoMachine.
bool NameOk(const std::string& name);
bool MachineOk(uint8_t machine);
bool ColourOk(uint32_t colour);
// A track a ballot may name: in the pool, or kTrackRandom.
bool TrackInPool(uint8_t track, uint32_t pool);
// A pool has at least one track and no bit above the stock courses.
bool PoolOk(uint32_t pool);

// The preset messages (D7). The id is the wire value; the words live here so every copy shows the same
// ones. "" for an id out of range.
const char* EmoteText(uint8_t emote);

// Keeps printable ASCII, turns tabs and line breaks into spaces, folds runs of spaces into one, trims both
// ends and cuts to kChatMax. Returns "" when nothing printable is left. No room message carries typed text
// since version 2; this and FilterChat are kept for the day typed chat returns.
std::string SanitizeChat(const std::string& raw);

// Masks blocked words with '*', ignoring case and common digit swaps (0 for o, 1 for i, 3 for e, 4 for a,
// 5 for s, 7 for t, @ for a, $ for s). A built-in list covers common English profanity and slurs. extra
// adds the player's own words, one per entry: a plain word matches at the start of a word, a word written
// with a leading '*' matches inside words as well ("*fuck" catches "motherfucker"), and a word with a
// trailing '|' matches only as a whole word ("ass|"). A match masks the whole word it sits in ("shitty").
// Entries that are empty or hold anything but letters, digits and those marks are ignored. Text is
// printable ASCII, as SanitizeChat leaves it.
std::string FilterChat(const std::string& text, const std::vector<std::string>& extra);

// Encoders. A value that breaks its rule is a caller bug: the encoder still returns bytes, and the decoder on
// the far side refuses them.
std::vector<uint8_t> EncodeJoin(const JoinRequest& join);
std::vector<uint8_t> EncodeWelcome(uint8_t id);
std::vector<uint8_t> EncodeReject(RejectReason reason);
std::vector<uint8_t> EncodeRoster(const std::vector<PlayerInfo>& roster);
std::vector<uint8_t> EncodeReady(bool ready);
std::vector<uint8_t> EncodeRoomState(const RoomState& room);
std::vector<uint8_t> EncodeVote(uint8_t track);
std::vector<uint8_t> EncodeVotes(const std::vector<Ballot>& ballots);
std::vector<uint8_t> EncodeLottery(const LotteryResult& lottery);
std::vector<uint8_t> EncodePick(uint8_t track);
std::vector<uint8_t> EncodePicked(const Picked& picked);
std::vector<uint8_t> EncodeLock(const LockRequest& lock);
std::vector<uint8_t> EncodeEmote(uint8_t emote);
std::vector<uint8_t> EncodeEmoteLine(const EmoteLine& line);
std::vector<uint8_t> EncodeSeatNext(bool raceNext);
std::vector<uint8_t> EncodeReadyToRace(const ReadyToRace& ready);
std::vector<uint8_t> EncodeGrid(const Grid& grid);
std::vector<uint8_t> EncodeStart(const StartSignal& start);
std::vector<uint8_t> EncodeClockPing(uint32_t clientMs);
std::vector<uint8_t> EncodeClockPong(const ClockPong& pong);
// Writes at most kStatePoses poses: a longer list is the caller's to split.
std::vector<uint8_t> EncodeState(const RaceState& state);
std::vector<uint8_t> EncodeFinish(const FinishReport& report);
std::vector<uint8_t> EncodeResults(const RaceResults& results);

bool DecodeJoin(const uint8_t* data, size_t len, JoinRequest& out);
bool DecodeWelcome(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeReject(const uint8_t* data, size_t len, RejectReason& out);
// Ids are unique and below kMaxPlayers, exactly one entry is the host and it is id 0 in a race seat, at most
// kMaxRacers race seats and kMaxWatchers watch seats. Only a watcher asks to race next, and only a racer is
// on the grid or locked in.
bool DecodeRoster(const uint8_t* data, size_t len, std::vector<PlayerInfo>& out);
bool DecodeReady(const uint8_t* data, size_t len, bool& out);
bool DecodeRoomState(const uint8_t* data, size_t len, RoomState& out);
// A stock track or kTrackRandom. Whether it is in the room's pool is the room's check.
bool DecodeVote(const uint8_t* data, size_t len, uint8_t& out);
// Ids are unique, ascending and below kMaxPlayers; at most kMaxRacers ballots.
bool DecodeVotes(const uint8_t* data, size_t len, std::vector<Ballot>& out);
bool DecodeLottery(const uint8_t* data, size_t len, LotteryResult& out);
bool DecodePick(const uint8_t* data, size_t len, uint8_t& out);
bool DecodePicked(const uint8_t* data, size_t len, Picked& out);
bool DecodeLock(const uint8_t* data, size_t len, LockRequest& out);
bool DecodeEmote(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeEmoteLine(const uint8_t* data, size_t len, EmoteLine& out);
bool DecodeSeatNext(const uint8_t* data, size_t len, bool& out);
bool DecodeReadyToRace(const uint8_t* data, size_t len, ReadyToRace& out);
// One to kMaxRacers humans with unique ids below kMaxPlayers, a roster machine and skin each, at most
// kGridSlots racers with the CPUs, and rules inside their limits.
bool DecodeGrid(const uint8_t* data, size_t len, Grid& out);
bool DecodeStart(const uint8_t* data, size_t len, StartSignal& out);
bool DecodeClockPing(const uint8_t* data, size_t len, uint32_t& out);
bool DecodeClockPong(const uint8_t* data, size_t len, ClockPong& out);
// One to kStatePoses poses, each of them StatePoseOk. Whose slot a pose may name is the session's check.
bool DecodeState(const uint8_t* data, size_t len, RaceState& out);
// Finished with a time from 1 to kRaceTimeMaxMs, or Retired with none. Whether the player may report at
// all, and whether the time can be true, is the room's check.
bool DecodeFinish(const uint8_t* data, size_t len, FinishReport& out);
// At most kMaxRacers entries with unique ids below kMaxPlayers, in table order: finishers by time, then
// the unfinished, then the retired. A time only with Finished, points up to kPointsMax and none for
// Retired.
bool DecodeResults(const uint8_t* data, size_t len, RaceResults& out);

// Discovery.
bool IsDiscovery(const uint8_t* data, size_t len);
std::vector<uint8_t> EncodeProbe(uint32_t nonce);
std::vector<uint8_t> EncodeRoom(uint32_t nonce, const RoomInfo& room);
bool DecodeProbe(const uint8_t* data, size_t len, uint32_t& nonce);
bool DecodeRoom(const uint8_t* data, size_t len, uint32_t& nonce, RoomInfo& out);

} // namespace gdx::net::lobby

#endif // GDX_NET_LOBBY_H
