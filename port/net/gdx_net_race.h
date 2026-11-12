// Copyright (c) Zorkats
//
// port/net/gdx_net_race.h: remote racers of an online race (milestone O3, slice S2) and the start of
// one (slice S3: the grid, the intro every copy runs on the host clock, the rules of the race).
//
// A remote machine is a "puppet": a real Racer slot whose pose is written from poses its owner sent,
// instead of being driven by a controller or by the CPU code. This file is the half that knows nothing
// about the game: the pose, the buffer that turns a stream of poses into one pose per local tick, and the
// small store the game bridge (port/gdx_net_race_bridge.c) and the network thread (port/gdx_lan_lobby.cpp)
// share. From slice S4 the poses travel: see "Poses between copies" below.
//
// Pure: no socket, no clock, no game header. Time is a tick count and a clock value the caller passes in.
//
// Where a puppet is shown (slice S6). Its owner sends one pose every kSendIntervalTicks, and a pose is a
// few ticks old when it arrives. Shown as it comes, the machine would be behind where its owner has it,
// by more than six times the distance at which two machines touch, and no two copies would see the same
// bump. So a puppet is shown where its owner has it now, which is a guess:
//   1. A pose carries the room's clock at its tick. The buffer keeps how fast the owner's game runs and
//      how far its tick count is ahead of ours, smoothed, and from that how many of the owner's ticks the
//      newest pose is old (Sample).
//   2. The pose is moved that many ticks along the road at the velocity it carries (gdx_net_race_advance).
//   3. What we pushed the machine ourselves, in bumps its owner's poses do not hold yet, is added (Shove).
//   4. The place shown moves to that guess a part of the way each tick, so a correction is not a jump
//      (Settle).

#ifndef GDX_NET_RACE_H
#define GDX_NET_RACE_H

// Plain C types on purpose: the game bridge is built against the decomp headers, which have no uint32_t.

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_RACE_MAX_RACERS 30

// One machine at one tick. Positions are game units, the two axes are unit vectors.
typedef struct GdxNetPose {
    unsigned int tick;   // the owner's race tick, not ours
    unsigned int timeMs; // the room's clock when that tick ran, the same clock on every copy; it wraps
    float pos[3];
    float vel[3]; // units a tick
    float forward[3];
    float up[3];
    int lap;
    unsigned int flags; // the owner's racer state flags
    int attack;         // the owner's attack state: 0 none, 1 side attack, 2 spin
} GdxNetPose;

// gdx_net_race_sample result bits.
#define GDX_NET_SAMPLE_NONE 0 // no pose was ever received
#define GDX_NET_SAMPLE_POSE 1 // `out` holds a pose
#define GDX_NET_SAMPLE_HELD 2 // the newest pose is older than the furthest a machine is guessed ahead
#define GDX_NET_SAMPLE_STEP 4 // the owner's tick count or ours skipped: the lead was set anew, not eased

// The road at a place: three unit vectors, forward along it, up from it and to its left, in that order.
// Returns 0 where there is no road to follow.
typedef int (*GdxNetRoadFrame)(void* context, const float pos[3], float frame[9]);

// Forgets every pose, puppet mark, the echo, the rules and the active and online flags. Called when a
// race starts.
void gdx_net_race_reset(void);

// True while remote racers are in play: an online race (plan decision P5) or the developer echo.
void gdx_net_race_set_active(int on);
int gdx_net_race_active(void);

// True while the race being run belongs to a room. The echo never sets it. Turning it off also gives
// the player their own rules back.
void gdx_net_race_set_online(int on);
int gdx_net_race_online(void);

// A puppet slot is not moved by the game. Cheap: the game asks once per racer per tick.
void gdx_net_race_set_puppet(int slot, int on);
int gdx_net_race_is_puppet(int slot);

// A pose that arrived for `slot`. One older than the pose held is dropped.
void gdx_net_race_push(int slot, const GdxNetPose* pose);
// The newest pose of `slot`, and in `ahead` how many of its owner's ticks have passed since: our tick is
// `localTick` and the room's clock says `nowMs`. Returns GDX_NET_SAMPLE_ bits.
int gdx_net_race_sample(int slot, unsigned int localTick, unsigned int nowMs, GdxNetPose* out, float* ahead);
// Moves a pose `ticks` ahead at its own velocity, a tick at a time. After each step the velocity and the
// two axes are turned as the road turned, so the machine keeps its speed along the road, its drift across
// it and its lane. Where `road` says there is none it goes straight. Touches no store.
void gdx_net_race_advance(GdxNetPose* pose, float ticks, GdxNetRoadFrame road, void* context);
// A machine this copy drives pushed `slot` at our tick `localTick`: `push` is what the game would add to
// that machine's velocity. Its owner will feel the same bump and say so in its poses, a few ticks from now.
void gdx_net_race_shove(int slot, unsigned int localTick, const float push[3]);
// What the pushes that the newest pose is too old to hold add to the guess: a distance and a velocity.
void gdx_net_race_shoved(int slot, unsigned int localTick, float offset[3], float velocity[3]);
// The place to show `slot` at, given the guess for this tick and the velocity it moves at. The place
// shown before is carried on by that velocity and brought a part of the way to the guess. Returns 1 when
// the machine was put at the guess at once: its first place, or one too far to have been driven to.
int gdx_net_race_settle(int slot, unsigned int localTick, const float target[3], const float velocity[3],
                        float out[3]);

// Our own machine's pose, once per tick. Every kSendIntervalTicks it leaves for the other players.
void gdx_net_race_local_pose(const GdxNetPose* pose, unsigned int localTick);

// ---- Poses between copies (slice S4) ----
//
// The game thread and the network thread never wait for each other: each side puts poses in a fixed-size
// box and the other takes them out, one short lock each way. A box that is full drops its oldest pose,
// because the newer one is worth more.
//
//   game thread     gdx_net_race_local_pose / _send_pose  ->  outgoing box  ->  gdx_net_race_take_outgoing
//   network thread  gdx_net_race_deliver                  ->  incoming box  ->  gdx_net_race_receive
//
// A pose travels with its race slot, which is the same on every copy. The copy that takes it in turns
// the race slot into its own game slot (gdx_net_race_local_slot) and stamps it with its own tick.

// A pose and the race slot it belongs to.
typedef struct GdxNetSlotPose {
    int slot;
    GdxNetPose pose;
} GdxNetSlotPose;

#define GDX_NET_RACE_OUT_CAPACITY 128 // four sends of a whole field
#define GDX_NET_RACE_IN_CAPACITY 256

// Game thread, when a room's race starts: which race it is and which race slot is ours. Poses are
// exchanged from here to the next gdx_net_race_reset.
void gdx_net_race_link(unsigned int raceIndex, int self);
// True on the ticks a pose leaves: every kSendIntervalTicks of a linked race.
int gdx_net_race_send_due(unsigned int tick);
// Game thread: the pose of a machine this copy drives besides its own, which on the host is every CPU.
// Kept on the ticks a pose leaves and dropped on the others.
void gdx_net_race_send_pose(int raceSlot, const GdxNetPose* pose);
// Network thread: takes the poses waiting to leave, oldest first, at most `capacity`. Returns how many.
int gdx_net_race_take_outgoing(unsigned int* raceIndex, GdxNetSlotPose* out, int capacity);
// Network thread: a pose arrived from the room. Dropped when it is not for the race being run.
void gdx_net_race_deliver(unsigned int raceIndex, int raceSlot, const GdxNetPose* pose);
// Game thread, once per tick: the poses that arrived go to their puppets.
void gdx_net_race_receive(void);

// What happened to the poses of the race being run, for the log line a race ends with.
typedef struct GdxNetRaceStats {
    unsigned int sent;     // poses put in the outgoing box
    unsigned int received; // poses given to a puppet
    unsigned int dropped;  // poses a full box pushed out, either way
    unsigned int held;     // samples whose newest pose was too old to guess ahead from
    unsigned int snaps;    // places shown that were a jump, the first one of each puppet included
    unsigned int steps;    // samples that set a puppet's lead anew: a game dropped a frame
} GdxNetRaceStats;

void gdx_net_race_stats(GdxNetRaceStats* out);

// Developer check with one copy of the game: our own poses come back to `slot` after `delayTicks`,
// as if another player had sent them. 0 turns it off.
void gdx_net_race_echo(int slot, unsigned int delayTicks);

// Where a puppet stands in the race. `lap` is what its owner reported and `lapDistance` is where we found
// it on the track; near the finish line one of the two can be a pose ahead of the other, which would
// move the puppet a whole lap for a tick. The result is the race distance nearest to `previous`, and
// `*lapOut` the lap that goes with it. After a jump (`snapped`) the owner's lap is taken as it is.
float gdx_net_race_progress(float lapDistance, int lap, float courseLength, float previous, int snapped,
                            int* lapOut);

// ---- The start of an online race (slice S3) ----

// The grid. Race slots are the same on every copy: the humans first, from the front of their rows to the
// back, then the CPUs. Each copy drives its own machine in the game's slot 0, so it swaps that slot with
// its own (gdx_net_race_local_slot).
typedef struct GdxNetGrid {
    int racers; // humans + CPUs; 0 when there is no grid
    int humans;
    unsigned char character[GDX_NET_RACE_MAX_RACERS]; // by race slot
    unsigned char skin[GDX_NET_RACE_MAX_RACERS];
} GdxNetGrid;

// Fills `out` from the humans' machines and skins, in grid order. The CPUs get the machines no human
// took, shuffled by `seed`, so every copy shows the same field. Counts out of range give an empty grid.
void gdx_net_race_build_grid(unsigned int seed, const unsigned char* machine, const unsigned char* skin, int humans,
                             int cpus, GdxNetGrid* out);
// The game slot of a race slot on the copy whose own race slot is `self`. Its own inverse.
int gdx_net_race_local_slot(int self, int raceSlot);
// Where a race slot starts, 1 at the front: the CPUs ahead, the humans behind them in grid order.
int gdx_net_race_grid_place(int humans, int racers, int raceSlot);

// The race intro, run on the host clock. The game's intro timer starts at GDX_NET_INTRO_START, loses one
// each tick, and the machines are released when it reaches GDX_NET_INTRO_GO. `elapsedMs` is the time
// since the room's start time on the host clock, negative before it or while it is not known.
#define GDX_NET_INTRO_START 460
#define GDX_NET_INTRO_GO 40
#define GDX_NET_INTRO_TICKS_PER_SECOND 60

// The value the timer should hold once the tick that is due now has run.
int gdx_net_race_intro_target(int elapsedMs);
// Whether the intro runs a tick now, given the timer before it. It waits at the start value for the
// start time, and waits a tick when it has run ahead of the host clock. From GO on it runs freely.
int gdx_net_race_intro_run(int timer, int elapsedMs);
// The timer after a tick ran and took one off: `timer`, or less when this copy is behind the host clock.
// It never passes over a tick the game does something on (the three beeps and GO).
int gdx_net_race_intro_catch_up(int timer, int elapsedMs);

// The rules of an online race: the host's values of the options that change how a machine drives.
typedef struct GdxNetRules {
    int laps;
    int ignoreTrackSettings;
    int lavaNoKill;
    float magnetRange;
    float magnetStrength;
    int boostFrames; // 0 is the stock boost length
} GdxNetRules;

// NULL gives the player their own values back.
void gdx_net_race_set_rules(const GdxNetRules* rules);
// Each takes the player's own value and returns the one to race on: the same, unless rules are set.
int gdx_net_race_rule_ignore_track_settings(int own);
int gdx_net_race_rule_lava_no_kill(int own);
float gdx_net_race_rule_magnet_range(float own);
float gdx_net_race_rule_magnet_strength(float own);
int gdx_net_race_rule_boost_frames(int own);

// The Create Machine records of an online race, by character (G-NET O4.5, decision 10). A record is the 32
// bytes of the game's CustomMachine. A character races a record only when every human on it brought the same
// record; every other character, the CPUs' included, races its stock machine on every copy. Kept apart from
// the rules: gdx_net_race_reset does not clear it, because the game rebuilds its machines from it once the
// field is known and again once the race is over. Game thread only.
#define GDX_NET_MACHINE_RECORD_BYTES 32
// hasRecord and records may be NULL: then no human brought a record. records holds humans rows of
// GDX_NET_MACHINE_RECORD_BYTES.
void gdx_net_race_set_machines(const unsigned char* machine, const unsigned char* hasRecord,
                               const unsigned char* records, int humans);
void gdx_net_race_clear_machines(void);
// 1 while a table is set: the game builds every machine from it and from the stock machines alone.
int gdx_net_race_machines_set(void);
// 1 and the record in out when character races one, else 0.
int gdx_net_race_machine_record(int character, unsigned char* out);

// SHA-256 over the bytes fed since the last begin: the course digest of the ready message.
void gdx_net_race_digest_begin(void);
void gdx_net_race_digest_feed(const void* bytes, unsigned int len);
void gdx_net_race_digest_end(unsigned char out[32]);

#ifdef __cplusplus
} // extern "C"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gdx::net::race {

constexpr uint32_t kSendIntervalTicks = 2; // one pose every second tick, 30 a second (P10)
constexpr float kTicksPerMs = 0.06f;       // 60 ticks a second
// How fast the owner's game runs, in ticks per ms of the room's clock ("the pace"): a game that cannot
// hold 60 frames a second runs slower than the clock, and a pose of it that is 80 ms old is fewer than
// five of its ticks old. It is measured over this long a stretch of poses, so that a ms of error in a
// stamp is a fraction of a percent, and each measure moves the pace kept by kPaceGain of the difference.
constexpr int32_t kPaceSpanMs = 500;
constexpr float kPaceGain = 0.5f;
constexpr float kPaceLeast = 0.25f * kTicksPerMs;
constexpr float kPaceMost = 1.5f * kTicksPerMs;
// How far the owner's tick count is ahead of ours ("the lead") is read off the clock stamps each tick and
// followed by a filter: an eighth of the difference a tick, and a much smaller part into a drift, so an
// owner whose game runs a little slower or faster than ours is followed with no lasting error. A clock
// stamp shakes by a few ms; taken as it comes, that would shake the machine by several units.
constexpr float kLeadGain = 1.0f / 8.0f;
constexpr float kLeadDriftGain = 1.0f / 256.0f; // with kLeadGain, a filter that settles without swinging
constexpr float kLeadDriftMost = 1.0f;     // ticks a tick: one game at twice the pace of the other
// A game that drops a frame loses a whole tick at once, and the lead with it. A difference this large is
// that, not a shaky stamp: the lead is set at once, and the place shown is eased to it (Settle).
constexpr float kLeadResyncTicks = 0.75f;
constexpr float kAheadMostTicks = 40.0f;   // a machine is guessed at most this far ahead of its newest pose
constexpr float kEaseKeep = 0.75f;         // the part of a correction still to go after a tick
constexpr float kEaseSnapUnits = 200.0f;   // further from the guess than this, the machine is put there
constexpr size_t kShoveCapacity = 48;      // ticks of pushes kept: more than kAheadMostTicks

// The numbers a test or a later slice may want to change. Defaults are the constants above.
struct BufferConfig {
    float paceGain = kPaceGain;
    float leadGain = kLeadGain;
    float leadDriftGain = kLeadDriftGain;
    float leadResyncTicks = kLeadResyncTicks;
    float aheadMostTicks = kAheadMostTicks;
    float easeKeep = kEaseKeep;
    float easeSnapUnits = kEaseSnapUnits;
};

// One remote machine: its newest pose, its owner's lead, our pushes and the place shown.
class PoseBuffer {
  public:
    PoseBuffer() = default;
    explicit PoseBuffer(const BufferConfig& config) : mConfig(config) {
    }

    void Clear();
    // Keeps the pose when it is newer than the one held. False when it was dropped.
    bool Push(const GdxNetPose& pose);
    int Sample(uint32_t localTick, uint32_t nowMs, GdxNetPose& out, float& ahead);
    void Shove(uint32_t localTick, const float push[3]);
    void Shoved(uint32_t localTick, float offset[3], float velocity[3]) const;
    bool Settle(uint32_t localTick, const float target[3], const float velocity[3], float out[3]);

    bool Has() const {
        return mHas;
    }
    // The owner's tick count minus ours, as last worked out by Sample.
    float Lead() const {
        return mLead;
    }
    // The owner's ticks in one ms of the room's clock.
    float Pace() const {
        return mPace;
    }
    size_t Shoves() const {
        return mShoveCount;
    }

  private:
    struct ShoveEntry {
        uint32_t tick; // ours
        float push[3];
    };

    BufferConfig mConfig;
    GdxNetPose mPose{};
    bool mHas = false;
    float mPace = kTicksPerMs;
    uint32_t mPaceTick = 0; // the pose the pace is measured from
    uint32_t mPaceMs = 0;
    bool mLeadSet = false;
    float mLead = 0.0f;
    float mLeadDrift = 0.0f; // how much the lead changes in one of our ticks
    uint32_t mLeadTick = 0;
    std::array<ShoveEntry, kShoveCapacity> mShoves{}; // oldest first, from mShoveHead
    size_t mShoveHead = 0;
    size_t mShoveCount = 0;
    bool mShownSet = false;
    uint32_t mShownTick = 0;
    float mShown[3] = { 0.0f, 0.0f, 0.0f };
};

// See gdx_net_race_advance.
void Advance(GdxNetPose& pose, float ticks, GdxNetRoadFrame road, void* context);

// Poses on their way: each comes out when its tick is due. First in, first out.
class DelayLine {
  public:
    static constexpr size_t kCapacity = 512;

    void Clear();
    // A full line drops its oldest pose.
    void Push(const GdxNetPose& pose, uint32_t dueTick);
    bool Pop(uint32_t nowTick, GdxNetPose& out);
    size_t Size() const {
        return mCount;
    }

  private:
    struct Entry {
        GdxNetPose pose;
        uint32_t due;
    };
    std::array<Entry, kCapacity> mEntries{};
    size_t mHead = 0;
    size_t mCount = 0;
};

} // namespace gdx::net::race

#endif // __cplusplus

#endif // GDX_NET_RACE_H
