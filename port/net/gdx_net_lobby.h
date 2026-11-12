// port/net/gdx_net_lobby.h: the LAN room wire format (version 4), LAN discovery datagrams and the chat filter.
//
// Pure: no socket, no clock, no game header. gdx_net_lobby_session.* moves these bytes over the reliable
// channel (gdx_net_channel.h) and gdx_net_room.* decides what the room does with them; this file only builds
// and checks them. Every decoder reads lengths before bytes, refuses anything short, long or out of range,
// and refuses trailing bytes, so a decoder either returns a whole valid message or false and leaves its
// output unchanged.
//
// Room messages ride the reliable channel as Message{type, payload}. All integers are little-endian.
// A string is u8 length + bytes.
//
// A track is a u8: a stock course index below kStockTracks, or kTrackCustom. kTrackCustom is followed at
// once by the custom track's identity, 32 bytes (the SHA-256 of its content bytes, G-NET decision 4), and
// its title, a string of 1 to kTrackTitleMax printable bytes. "track" below means that form. A stock track
// keeps the single byte of version 2.
//
// A machine record, where one may travel, is a u8 0 or 1 and, after a 1, the 32 bytes of a Create Machine
// slot (the decomp's CustomMachine, 0x20 bytes) as the owner's save holds them (decision 10).
//
//    1 Join         client to host  build, name, u32 colour (0x00RRGGBB), u8 machine, u8 Seat, hack name,
//                                   32 bytes hack archive hash: the ROM hack this copy latched at boot, or
//                                   an empty name and 32 zero bytes for the stock game (decision 47),
//                                   32 bytes the joiner's friend key, 64 bytes its join proof over the Nonce
//                                   (both all zero: a copy with no key, which a Friends room refuses), u8 1 when
//                                   the joiner's own gathering found it reachable from outside, else 0 (decision 51)
//    2 Welcome      host to client  u8 player id
//    3 Reject       host to client  u8 RejectReason; the host drops the link right after
//    4 Roster       host to all     u8 count, count x { u8 id, u8 flags, u32 colour, u8 machine, u8 skin,
//                                   u16 ping ms, u16 series points, name }
//    5 Ready        client to host  u8 0 or 1
//    6, 7           retired: the typed Chat and ChatLine of version 1. Never reuse these numbers.
//    8 Room         host to all     RoomState: u32 room id, u16 race index, u8 Phase, u32 phase ms left,
//                                   u32 phase total ms, u8 racer cap, u32 pool, track, u8 last track, u8 picker,
//                                   u8 locked, u8 racers, u8 CPUs out, u8 custom tracks, u8 CPU flags (1 CPU
//                                   fill, 2 CPU track hazards), u16 AI aggression, the rules as in Grid, u8
//                                   successor (or kSystemId), 16 bytes successor address (IPv4
//                                   mapped, all zero for nobody), u16 successor port, u8 RoomType,
//                                   32 bytes lottery commitment: all zero unless this Open room committed to
//                                   the lottery of this race (decision 39), u32 room code: kNoRoomCode, or a
//                                   value below 2^30 in a directory room (O5.2, O5.10)
//    9 Vote         client to host  track, or kTrackRandom
//   10 Votes        host to all     u8 count, count x { u8 id, track }: the ballots cast so far
//   11 Lottery      host to all     the final ballots as in Votes, u8 winner (index into them, or kNoWinner),
//                                   track, 32 bytes salt that opens the commitment (all zero without one)
//   12 Pick         client to host  track, or kTrackRandom
//   13 Picked       host to all     track, u8 drawn (0 chosen, 1 drawn by the host), u8 by (id, or kSystemId)
//   14 Lock         client to host  u8 machine, u8 skin, u8 engine (0 to kEngineMax), machine record
//   15 Emote        client to host  u8 preset message id
//   16 EmoteLine    host to all     u8 from id, u8 preset message id
//   17 SeatNext     client to host  u8 0 or 1: a watcher asks for a race seat at the next grid, or takes it back
//   18 ReadyToRace  client to host  u16 race index, 32 bytes course digest, u16 load time in ms
//   19 Grid         host to all     the race about to load, see EncodeGrid: u16 race index, u32 seed, u8 laps,
//                                   u8 rule flags, u32 magnet range, u32 magnet strength (float bits), u32
//                                   boost frames, track, u8 CPUs, u8 count, count x { u8 id, u8 machine,
//                                   u8 skin, machine record }, the humans from the front of their rows to
//                                   the back
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
//   26 TrackOffer   both ways       a custom track's transfer. gdx_net_transfer.h owns the five payloads and
//   27 TrackChunk   both ways       the rule of who may send them: the owner to the host, the host to every
//   28 TrackHave    both ways       guest
//   29 TrackNeed    both ways
//   30 TrackAbort   both ways
//   31 HandoverToken  host to one member  u64 the member's own token (decision 21), never zero
//   32 HandoverTokens host to successor   u8 count, count x { u8 id, u64 token }: every other member's
//                                         token, ids unique, ascending and below kMaxPlayers, then
//                                         32 bytes directory takeover secret (all zero outside a
//                                         directory room; decision 32), then the room's bans (decision 52):
//                                         u8 count (at most kBanMax) x 32 bytes banned friend key, u8 count
//                                         (at most kBanMax) x 16 bytes banned address prefix (an IPv4 address
//                                         whole, an IPv6 one by its /64)
//   33 Handover       host to all         u8 successor (or kSystemId: the room closes), 16 bytes address
//                                         as in Room, u16 port
//   34 Rejoin         client to successor u8 the id the member had, u64 its token
//   35 Nonce          host to a new link  u32 room id, 32 bytes the host's friend key, 32 bytes nonce: what the
//                                         joiner signs before it sends Join (O7.3, decision 42)
//   36 Keys           host to all         u8 count, count x { u8 id, 32 bytes friend key }: the key of each
//                                         member who has one, ids unique and ascending
//   37 Reach          client to host      u8 0 or 1: the member's gathering finished after it joined, or it
//                                         came back to a promoted host, and this is what it found (decision 51)
//
// Discovery datagrams travel outside the channel, on the same socket. They start with "GDXL", which the
// channel never sends (its packets start with "GD" and a type byte from 0 to 5), so the host tells them
// apart by the first four bytes.
//
//   Probe   "GDXL" u8 1, u16 kLobbyVersion, u32 nonce, zero bytes up to kProbeBytes. The padding makes the
//           probe as long as the longest Room reply, so a probe with a forged source cannot make a host send
//           more than it received (decision 34).
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
#include <utility>
#include <vector>

#include "gdx_net_address.h"
#include "gdx_net_channel.h"

namespace gdx::net::lobby {

constexpr uint16_t kDefaultPort = 24302;
// One number for the channel and the room (decision 26): see kProtocolVersion.
constexpr uint16_t kLobbyVersion = kProtocolVersion;
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
constexpr uint8_t kTrackCustom = 0xFD; // a custom track: its identity and title follow on the wire
constexpr uint8_t kNoWinner = 0xFF;    // a lottery nobody voted in

// A custom track's identity is gdx::net::track::TrackId; this file keeps its own copy of the length so it
// stays free of the snapshot code. The title limit is GDX_COURSE_TITLE_MAX (gdx_course_registry.h).
constexpr size_t kTrackIdBytes = 32;
constexpr size_t kTrackTitleMax = 20;

// 32 opaque bytes: a lottery commitment or salt, or a takeover secret. All zero means "none".
using Bytes32 = std::array<uint8_t, 32>;

inline bool IsZero32(const Bytes32& bytes) {
    for (const uint8_t b : bytes) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

// A track a copy may not have. Its owner is whoever named it (the ballot's player, the picker), and its
// bytes travel through the host (gdx_net_transfer.h).
struct CustomTrack {
    std::array<uint8_t, kTrackIdBytes> id{};
    std::string title;

    bool operator==(const CustomTrack& o) const {
        return id == o.id && title == o.title;
    }
    bool operator!=(const CustomTrack& o) const {
        return !(*this == o);
    }
};

// 1 to kTrackTitleMax printable ASCII bytes.
bool TrackTitleOk(const std::string& title);

// A ballot's, a pick's or a race's track: a stock index, kTrackRandom or kTrackNone where the message allows
// them, or kTrackCustom with custom filled in. custom stays empty for every other track.
struct TrackChoice {
    uint8_t track = kTrackNone;
    CustomTrack custom;

    bool operator==(const TrackChoice& o) const {
        return track == o.track && custom == o.custom;
    }
    bool operator!=(const TrackChoice& o) const {
        return !(*this == o);
    }
};

// A Create Machine slot, the decomp's CustomMachine (0x20 bytes), as raw bytes. The game reads it straight
// into a racer (racer.c, func_8008D33C), and its checksum proves nothing anyone could not compute, so every
// field the game reads is range-checked (MachineRecordOk) by the host and by every receiver (decision 10).
constexpr size_t kMachineRecordBytes = 0x20;
using MachineRecord = std::array<uint8_t, kMachineRecordBytes>;

// The ROM hack this copy latched at boot (gdx_hackmods.h): its archive's base name and the SHA-256 of the
// archive file. The stock game is an empty name and zero bytes. A room admits only copies whose hack matches
// the host's, stock included (decision 47).
constexpr size_t kHackNameMax = 64;
constexpr size_t kHackHashBytes = 32;

struct HackIdentity {
    std::string name;
    std::array<uint8_t, kHackHashBytes> hash{};

    bool operator==(const HackIdentity& o) const {
        return name == o.name && hash == o.hash;
    }

    bool operator!=(const HackIdentity& o) const {
        return !(*this == o);
    }
};

// The stock game (an empty name and zero bytes), or 1 to kHackNameMax bytes with no control character and
// any hash. The name is a file name, so UTF-8 passes.
bool HackOk(const HackIdentity& hack);

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
    kMsgTrackOffer = 26,
    kMsgTrackChunk = 27,
    kMsgTrackHave = 28,
    kMsgTrackNeed = 29,
    kMsgTrackAbort = 30,
    kMsgHandoverToken = 31,
    kMsgHandoverTokens = 32,
    kMsgHandover = 33,
    kMsgRejoin = 34,
    kMsgNonce = 35,
    kMsgKeys = 36,
    kMsgReach = 37,
};

// Wire values. Never renumber them.
enum class RejectReason : uint8_t {
    Build = 1,          // a different G-Diffuser build; no host sends it since O5.1 (decision 25)
    Full = 2,           // no seat of either kind is free
    BadProfile = 3,     // the name or machine breaks the rules
    Kicked = 4,         // the host removed this player
    RaceSeatsFull = 5,  // asked to race; only watch seats are free
    WatchSeatsFull = 6, // asked to watch; only race seats are free
    Hack = 7,           // a different ROM hack, or a hack against the stock game
    Rejoin = 8,         // a Rejoin the successor cannot match to a seat it holds (D28.4)
    Banned = 9,         // the host banned this friend key from the room (O7.3)
    NotFriend = 10,     // a Friends room, and the key is not on the host's friend list
    BadProof = 11,      // the join proof does not check under the key it came with
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
    Assets = 9, // after Machine: racers who lack the custom track get it (O4.6). Appended, so the others
                // keep their numbers
};
constexpr uint8_t kPhaseCount = 10;

const char* PhaseName(Phase phase);

// Wire values (O6.1). How players find a room, which sets who may join it and what the room must prove.
enum class RoomType : uint8_t {
    Lan = 0,     // the room list of this network, or JOIN BY ADDRESS
    Code = 1,    // a room code from the directory
    Open = 2,    // listed for strangers: commits to its lottery (decision 39)
    Friends = 3, // listed for the host's friends only (O7.3)
};
constexpr uint8_t kRoomTypeCount = 4;
// RoomState::code when the room has none: every LAN room, and a directory room before it is listed.
constexpr uint32_t kNoRoomCode = 0xFFFFFFFFu;
// Room code values are below this (gdx_net_codes.h kRoomCodeLimit).
constexpr uint32_t kRoomCodeValues = 1u << 30;

const char* RoomTypeName(RoomType type);

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
    HackIdentity hack;
    Bytes32 key{};                     // the joiner's friend key; all zero for none
    std::array<uint8_t, 64> proof{};   // friends::JoinProofSign over the host's Nonce; all zero with no key
    bool reachable = false;            // the joiner can take inbound connections (decision 51)
};

// What a host sends a new link first (O7.3): the joiner signs it into its Join.
struct JoinNonce {
    uint32_t roomId = 0;
    Bytes32 hostKey{}; // all zero: the host has no key
    Bytes32 nonce{};

    bool operator==(const JoinNonce& o) const {
        return roomId == o.roomId && hostKey == o.hostKey && nonce == o.nonce;
    }
};

// One member's friend key, in Keys.
struct MemberKey {
    uint8_t id = 0;
    Bytes32 key{};

    bool operator==(const MemberKey& o) const {
        return id == o.id && key == o.key;
    }
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
// gEnhancements.Tuning.BoostDuration: 0 is the stock boost, above it the frames a boost lasts. The cap is the
// one enhancements/tuning/BoostDuration.cpp clamps to.
constexpr uint32_t kBoostFramesMax = 100000;
// gEnhancements.Gameplay.AiAggression, a percentage: the menu's slider limits (gdx_menu_registry.cpp).
constexpr uint16_t kAiAggressionMin = 100;
constexpr uint16_t kAiAggressionMax = 400;
constexpr uint16_t kAiAggressionDefault = 100;

// The host values of the options that change how a player drives their own machine. Every copy races on
// them and goes back to its own afterwards. The options that only steer CPUs stay with the host, which
// is the only copy that runs them.
struct RaceRules {
    uint8_t laps = kLapsDefault;
    bool ignoreTrackSettings = false;
    bool lavaNoKill = false;
    float magnetRange = 80.0f;
    float magnetStrength = 1.7f;
    uint32_t boostFrames = 0; // 0 for the stock boost

    bool operator==(const RaceRules& o) const {
        return laps == o.laps && ignoreTrackSettings == o.ignoreTrackSettings && lavaNoKill == o.lavaNoKill &&
               magnetRange == o.magnetRange && magnetStrength == o.magnetStrength && boostFrames == o.boostFrames;
    }
};

bool RulesOk(const RaceRules& rules);

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
    CustomTrack custom;         // the track's identity and title, when track is kTrackCustom
    bool customTracks = false;  // the host's switch (D8): voters may name custom tracks
    // Version 4 (D28.1). Every setting a successor needs to run the room as the host did (decision 24).
    bool cpuFill = true;                          // CPUs take the grid slots the humans leave free (D2)
    uint16_t aiAggression = kAiAggressionDefault; // the host's gEnhancements.Gameplay.AiAggression
    bool cpuTrackHazards = false;                 // the host's gEnhancements.Gameplay.CpuTrackHazards
    RaceRules rules;                              // what the next grid will race on
    // The next host (decision 19) and its address as the host sees it (decision 18). kSystemId with a zero
    // address when nobody can take over.
    uint8_t successor = kSystemId;
    IpBytes successorIp{}; // IPv4 mapped (gdx_net_address.h); all zero with kSystemId
    uint16_t successorPort = 0;
    // Version 7 (O6.1, O6.4).
    RoomType type = RoomType::Lan;
    Bytes32 commitment{}; // lottery::Commit of this race's salt, all zero when the room did not commit
    // The directory code (gdx_net_codes.h value), so members can share it and a successor can take it over.
    uint32_t code = kNoRoomCode;

    bool operator==(const RoomState& o) const {
        return roomId == o.roomId && raceIndex == o.raceIndex && phase == o.phase && phaseLeftMs == o.phaseLeftMs &&
               phaseTotalMs == o.phaseTotalMs && racerCap == o.racerCap && pool == o.pool && track == o.track &&
               lastTrack == o.lastTrack && picker == o.picker && locked == o.locked && racers == o.racers &&
               cpusOut == o.cpusOut && custom == o.custom && customTracks == o.customTracks &&
               cpuFill == o.cpuFill && aiAggression == o.aiAggression && cpuTrackHazards == o.cpuTrackHazards &&
               rules == o.rules && successor == o.successor && successorIp == o.successorIp &&
               successorPort == o.successorPort && type == o.type && commitment == o.commitment && code == o.code;
    }
};

// The handover messages (D28, decisions 20 to 22).
// A member's own token: the new host admits that member back only with it.
struct HandoverToken {
    uint64_t token = 0;

    bool operator==(const HandoverToken& o) const {
        return token == o.token;
    }
};

// The bans a room holds (decision 52): they go to the successor with the token table, so a ban lasts as long as
// the room. At most kBanMax of each; past that the oldest goes.
constexpr size_t kBanMax = 32;
struct BanList {
    std::vector<Bytes32> keys;     // banned friend keys, none all zero
    std::vector<IpBytes> prefixes; // an IPv4 address whole, an IPv6 one by its /64; never loopback

    bool operator==(const BanList& o) const {
        return keys == o.keys && prefixes == o.prefixes;
    }
};

// Every member's token, for the successor alone, which must check them once it hosts.
struct HandoverTokenEntry {
    uint8_t id = 0;
    uint64_t token = 0;

    bool operator==(const HandoverTokenEntry& o) const {
        return id == o.id && token == o.token;
    }
};

// The host is leaving now: go to the successor at once instead of waiting out the silence.
struct Handover {
    uint8_t successor = kSystemId; // kSystemId: nobody takes over and the room closes
    IpBytes ip{};
    uint16_t port = 0;

    bool operator==(const Handover& o) const {
        return successor == o.successor && ip == o.ip && port == o.port;
    }
};

// A member comes back to the successor under the id it had.
struct Rejoin {
    uint8_t id = 0;
    uint64_t token = 0;

    bool operator==(const Rejoin& o) const {
        return id == o.id && token == o.token;
    }
};

struct Ballot {
    uint8_t id = 0;
    uint8_t track = kTrackRandom;
    CustomTrack custom; // when track is kTrackCustom: a track of this player's

    Ballot() = default;
    Ballot(uint8_t id_, uint8_t track_, CustomTrack custom_ = CustomTrack())
        : id(id_), track(track_), custom(std::move(custom_)) {
    }

    bool operator==(const Ballot& o) const {
        return id == o.id && track == o.track && custom == o.custom;
    }
};

struct LotteryResult {
    std::vector<Ballot> ballots; // sorted by player id
    uint8_t winner = kNoWinner;  // index into ballots
    uint8_t track = kTrackNone;
    CustomTrack custom; // when track is kTrackCustom: the winning ballot's
    Bytes32 salt{};     // opens the room's commitment; all zero when the room did not commit

    bool operator==(const LotteryResult& o) const {
        return ballots == o.ballots && winner == o.winner && track == o.track && custom == o.custom &&
               salt == o.salt;
    }
};

struct Picked {
    uint8_t track = kTrackNone;
    bool drawn = false;     // the host drew it: a first race, a timeout, or a Random pick
    uint8_t by = kSystemId; // who picked, or kSystemId when nobody did
    CustomTrack custom;     // when track is kTrackCustom: a track of the picker's

    Picked() = default;
    Picked(uint8_t track_, bool drawn_, uint8_t by_, CustomTrack custom_ = CustomTrack())
        : track(track_), drawn(drawn_), by(by_), custom(std::move(custom_)) {
    }

    bool operator==(const Picked& o) const {
        return track == o.track && drawn == o.drawn && by == o.by && custom == o.custom;
    }
};

struct LockRequest {
    uint8_t machine = 0;
    uint8_t skin = 0;
    uint8_t engine = 50;
    bool hasRecord = false; // the machine is the player's own Create Machine slot, sent for this race only
    MachineRecord record{};

    bool operator==(const LockRequest& o) const {
        return machine == o.machine && skin == o.skin && engine == o.engine && hasRecord == o.hasRecord &&
               record == o.record;
    }
};

struct EmoteLine {
    uint8_t from = 0;
    uint8_t emote = 0;
};


// A racer tells the host its copy of the track is loaded, and which track that is.
struct ReadyToRace {
    uint16_t raceIndex = 0;
    std::array<uint8_t, kDigestBytes> digest{}; // of the control points of the course
    uint16_t loadMs = 0;                        // from leaving the lock-in screen to the loaded track

    bool operator==(const ReadyToRace& o) const {
        return raceIndex == o.raceIndex && digest == o.digest && loadMs == o.loadMs;
    }
};

// True when every field of a Create Machine record the game reads is in its range: the three grades and
// their weighting, the three parts, the logo and decal, and a name that ends inside its 9 bytes. The
// decoders refuse a message whose record fails, and the host refuses a lock with one.
bool MachineRecordOk(const MachineRecord& record);

struct GridEntry {
    uint8_t id = 0;
    uint8_t machine = 0;
    uint8_t skin = 0;
    bool hasRecord = false; // the player locked in their own Create Machine slot: every copy races this one
    MachineRecord record{};

    bool operator==(const GridEntry& o) const {
        return id == o.id && machine == o.machine && skin == o.skin && hasRecord == o.hasRecord &&
               record == o.record;
    }
};

// The race every copy is about to load: the track, who is on the grid and in which order, how many CPUs
// fill it, the seed that deals the CPUs their machines, and the rules.
struct Grid {
    uint16_t raceIndex = 0;
    uint32_t seed = 0;
    RaceRules rules;
    TrackChoice track{ 0, {} }; // a stock index or kTrackCustom; never Random or None
    uint8_t cpus = 0;
    std::vector<GridEntry> humans; // from the front of their rows to the back

    bool operator==(const Grid& o) const {
        return raceIndex == o.raceIndex && seed == o.seed && rules == o.rules && track == o.track &&
               cpus == o.cpus && humans == o.humans;
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
std::vector<uint8_t> EncodeVote(const TrackChoice& choice);
std::vector<uint8_t> EncodeVotes(const std::vector<Ballot>& ballots);
std::vector<uint8_t> EncodeLottery(const LotteryResult& lottery);
std::vector<uint8_t> EncodePick(uint8_t track);
std::vector<uint8_t> EncodePick(const TrackChoice& choice);
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
std::vector<uint8_t> EncodeHandoverToken(const HandoverToken& token);
std::vector<uint8_t> EncodeHandoverTokens(const std::vector<HandoverTokenEntry>& tokens,
                                          const Bytes32& takeoverSecret = Bytes32{}, const BanList& bans = BanList{});
std::vector<uint8_t> EncodeHandover(const Handover& handover);
std::vector<uint8_t> EncodeRejoin(const Rejoin& rejoin);
std::vector<uint8_t> EncodeNonce(const JoinNonce& nonce);
std::vector<uint8_t> EncodeKeys(const std::vector<MemberKey>& keys);
std::vector<uint8_t> EncodeReach(bool reachable);

bool DecodeJoin(const uint8_t* data, size_t len, JoinRequest& out);
bool DecodeWelcome(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeReject(const uint8_t* data, size_t len, RejectReason& out);
// Ids are unique and below kMaxPlayers, exactly one entry is the host and it is id 0 in a race seat, at most
// kMaxRacers race seats and kMaxWatchers watch seats. Only a watcher asks to race next, and only a racer is
// on the grid or locked in.
bool DecodeRoster(const uint8_t* data, size_t len, std::vector<PlayerInfo>& out);
bool DecodeReady(const uint8_t* data, size_t len, bool& out);
// Fields in range, rules that pass RulesOk, AI aggression inside the menu's limits, and a successor that is
// a member other than the host with a port, or kSystemId with a zero address. A known room type, and a
// commitment only in an Open room.
bool DecodeRoomState(const uint8_t* data, size_t len, RoomState& out);
// A stock track or kTrackRandom. Whether it is in the room's pool is the room's check. The uint8_t form
// refuses a custom track. The TrackChoice form also takes kTrackCustom with a valid title; whether the
// room's switch allows it is again the room's check.
bool DecodeVote(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeVote(const uint8_t* data, size_t len, TrackChoice& out);
// Ids are unique, ascending and below kMaxPlayers; at most kMaxRacers ballots.
bool DecodeVotes(const uint8_t* data, size_t len, std::vector<Ballot>& out);
bool DecodeLottery(const uint8_t* data, size_t len, LotteryResult& out);
bool DecodePick(const uint8_t* data, size_t len, uint8_t& out);
bool DecodePick(const uint8_t* data, size_t len, TrackChoice& out);
bool DecodePicked(const uint8_t* data, size_t len, Picked& out);
bool DecodeLock(const uint8_t* data, size_t len, LockRequest& out);
bool DecodeEmote(const uint8_t* data, size_t len, uint8_t& out);
bool DecodeEmoteLine(const uint8_t* data, size_t len, EmoteLine& out);
bool DecodeSeatNext(const uint8_t* data, size_t len, bool& out);
bool DecodeReadyToRace(const uint8_t* data, size_t len, ReadyToRace& out);
// One to kMaxRacers humans with unique ids below kMaxPlayers, a roster machine and skin each, at most
// kGridSlots racers with the CPUs, rules inside their limits, and a stock or custom track.
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
// A token is never zero, so a zeroed field never matches one.
bool DecodeHandoverToken(const uint8_t* data, size_t len, HandoverToken& out);
// At most kMaxPlayers entries, ids unique, ascending and below kMaxPlayers, no zero token, then the takeover
// secret, then the bans: at most kBanMax keys, none all zero, and at most kBanMax prefixes, each one BanPrefix
// could have made. Every output is left alone on failure.
bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out,
                          Bytes32& takeoverSecret, BanList& bans);
bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out,
                          Bytes32& takeoverSecret);
bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out);
// A successor from 1 to kMaxPlayers - 1 with a port, or kSystemId with a zero address.
bool DecodeHandover(const uint8_t* data, size_t len, Handover& out);
// An id from 1 to kMaxPlayers - 1 (the host never rejoins its own room) and a token that is not zero.
bool DecodeRejoin(const uint8_t* data, size_t len, Rejoin& out);
bool DecodeNonce(const uint8_t* data, size_t len, JoinNonce& out);
// Refuses a zero key, an id at or above kMaxPlayers, and ids that are not strictly ascending.
bool DecodeKeys(const uint8_t* data, size_t len, std::vector<MemberKey>& out);
// A u8 0 or 1, nothing after it.
bool DecodeReach(const uint8_t* data, size_t len, bool& out);

// Discovery.
bool IsDiscovery(const uint8_t* data, size_t len);
// The probe's length: the longest Room reply, a build of kBuildMax bytes and a host name of kNameMax.
constexpr size_t kProbeBytes = 4 + 1 + 2 + 4 + 2 + 4 + 1 + kBuildMax + 1 + kNameMax;
std::vector<uint8_t> EncodeProbe(uint32_t nonce);
std::vector<uint8_t> EncodeRoom(uint32_t nonce, const RoomInfo& room);
bool DecodeProbe(const uint8_t* data, size_t len, uint32_t& nonce);
bool DecodeRoom(const uint8_t* data, size_t len, uint32_t& nonce, RoomInfo& out);

} // namespace gdx::net::lobby

#endif // GDX_NET_LOBBY_H
