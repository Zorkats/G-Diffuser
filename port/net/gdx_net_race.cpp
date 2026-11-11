// Copyright (c) Zorkats
//
// port/net/gdx_net_race.cpp: the pose buffer of an online race. See gdx_net_race.h for how a remote
// machine is shown where its owner has it now.

#include "gdx_net_race.h"

#include "../gdx_sha256.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace gdx::net::race {

namespace {

// Ticks are unsigned and may wrap; every comparison goes through a signed difference.
int32_t TicksFrom(uint32_t from, uint32_t to) {
    return static_cast<int32_t>(to - from);
}

// `v` as it lies in the frame `from`, put the same way in the frame `to`.
void Turn(const float from[9], const float to[9], float v[3]) {
    float part[3];

    for (int axis = 0; axis < 3; axis++) {
        part[axis] = v[0] * from[axis * 3] + v[1] * from[axis * 3 + 1] + v[2] * from[axis * 3 + 2];
    }
    for (int k = 0; k < 3; k++) {
        v[k] = part[0] * to[k] + part[1] * to[3 + k] + part[2] * to[6 + k];
    }
}

} // namespace

void PoseBuffer::Clear() {
    const BufferConfig config = mConfig;

    *this = PoseBuffer(config);
}

bool PoseBuffer::Push(const GdxNetPose& pose) {
    if (mHas && TicksFrom(mPose.tick, pose.tick) <= 0) {
        return false; // late, or a copy of the one held: the newest pose is the only one used
    }
    if (!mHas) {
        mPaceTick = pose.tick;
        mPaceMs = pose.timeMs;
    } else {
        const int32_t ms = static_cast<int32_t>(pose.timeMs - mPaceMs);

        if (ms < 0 || ms > 4 * kPaceSpanMs) {
            // A clock that went back, or a silence: nothing to measure across. The pace kept stands.
            mPaceTick = pose.tick;
            mPaceMs = pose.timeMs;
        } else if (ms >= kPaceSpanMs) {
            const float seen = static_cast<float>(TicksFrom(mPaceTick, pose.tick)) / static_cast<float>(ms);

            mPace += mConfig.paceGain * (std::clamp(seen, kPaceLeast, kPaceMost) - mPace);
            mPaceTick = pose.tick;
            mPaceMs = pose.timeMs;
        }
    }
    mPose = pose;
    mHas = true;
    return true;
}

int PoseBuffer::Sample(uint32_t localTick, uint32_t nowMs, GdxNetPose& out, float& ahead) {
    int result = GDX_NET_SAMPLE_POSE;

    if (!mHas) {
        return GDX_NET_SAMPLE_NONE;
    }
    // The owner's tick of the pose, counted from our tick, and how many of its ticks old the pose is by
    // the room's clock.
    const float poseTick = static_cast<float>(TicksFrom(localTick, mPose.tick));
    const float age = static_cast<float>(static_cast<int32_t>(nowMs - mPose.timeMs)) * mPace;
    const float seen = poseTick + age; // the lead, as this one stamp has it

    if (!mLeadSet) {
        mLead = seen;
        mLeadDrift = 0.0f;
        mLeadSet = true;
    } else {
        const int32_t elapsed = TicksFrom(mLeadTick, localTick);

        if (elapsed > 0) {
            mLead += mLeadDrift * static_cast<float>(elapsed);
            const float off = seen - mLead;

            if (std::fabs(off) > mConfig.leadResyncTicks) {
                mLead = seen; // the drift stands: one skipped tick says nothing about the pace
                result |= GDX_NET_SAMPLE_STEP;
            } else {
                mLead += mConfig.leadGain * off;
                mLeadDrift =
                    std::clamp(mLeadDrift + mConfig.leadDriftGain * off, -kLeadDriftMost, kLeadDriftMost);
            }
        }
    }
    mLeadTick = localTick;

    out = mPose;
    ahead = mLead - poseTick;
    if (ahead < 0.0f) {
        ahead = 0.0f; // the clocks say the pose is from the future: it is shown as it is
    } else if (ahead > mConfig.aheadMostTicks) {
        ahead = mConfig.aheadMostTicks;
        result |= GDX_NET_SAMPLE_HELD;
    }
    return result;
}

void PoseBuffer::Shove(uint32_t localTick, const float push[3]) {
    if (mShoveCount > 0) {
        ShoveEntry& last = mShoves[(mShoveHead + mShoveCount - 1) % kShoveCapacity];

        if (last.tick == localTick) {
            for (int k = 0; k < 3; k++) {
                last.push[k] += push[k]; // two machines pushed it in the same tick
            }
            return;
        }
    }
    if (mShoveCount == kShoveCapacity) {
        mShoveHead = (mShoveHead + 1) % kShoveCapacity;
        mShoveCount--;
    }
    ShoveEntry& entry = mShoves[(mShoveHead + mShoveCount) % kShoveCapacity];
    entry.tick = localTick;
    for (int k = 0; k < 3; k++) {
        entry.push[k] = push[k];
    }
    mShoveCount++;
}

void PoseBuffer::Shoved(uint32_t localTick, float offset[3], float velocity[3]) const {
    for (int k = 0; k < 3; k++) {
        offset[k] = velocity[k] = 0.0f;
    }
    if (!mHas || !mLeadSet) {
        return;
    }
    for (size_t i = 0; i < mShoveCount; i++) {
        const ShoveEntry& entry = mShoves[(mShoveHead + i) % kShoveCapacity];
        const int32_t since = TicksFrom(entry.tick, localTick);

        if (since < 0 || since >= static_cast<int32_t>(kShoveCapacity)) {
            continue;
        }
        // The push was at our tick entry.tick, which is the owner's tick entry.tick + lead. A pose from
        // that tick on is the owner's own account of the bump, and ours is no longer added.
        if (static_cast<float>(TicksFrom(entry.tick, mPose.tick)) >= mLead) {
            continue;
        }
        // The game adds a push to the velocity and moves the machine by it in the same tick.
        for (int k = 0; k < 3; k++) {
            velocity[k] += entry.push[k];
            offset[k] += entry.push[k] * static_cast<float>(since + 1);
        }
    }
}

bool PoseBuffer::Settle(uint32_t localTick, const float target[3], const float velocity[3], float out[3]) {
    bool put = !mShownSet;

    if (mShownSet) {
        const int32_t elapsed = std::clamp(TicksFrom(mShownTick, localTick), 1, 8);
        float off[3];

        for (int k = 0; k < 3; k++) {
            off[k] = mShown[k] + velocity[k] * static_cast<float>(elapsed) - target[k];
        }
        if (std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]) > mConfig.easeSnapUnits) {
            put = true;
        } else {
            const float keep = std::pow(mConfig.easeKeep, static_cast<float>(elapsed));

            for (int k = 0; k < 3; k++) {
                out[k] = target[k] + off[k] * keep;
            }
        }
    }
    if (put) {
        for (int k = 0; k < 3; k++) {
            out[k] = target[k];
        }
    }
    mShownSet = true;
    mShownTick = localTick;
    for (int k = 0; k < 3; k++) {
        mShown[k] = out[k];
    }
    return put;
}

void Advance(GdxNetPose& pose, float ticks, GdxNetRoadFrame road, void* context) {
    float here[9];
    float there[9];
    bool onRoad = false;

    if (!(ticks > 0.0f)) {
        return;
    }
    ticks = std::min(ticks, 4.0f * kAheadMostTicks); // a caller's mistake must not hang the game
    while (ticks > 0.0f) {
        const float step = std::min(ticks, 1.0f);
        float trial[3];

        if (!onRoad) {
            onRoad = road != nullptr && road(context, pose.pos, here) != 0;
        }
        for (int k = 0; k < 3; k++) {
            trial[k] = pose.pos[k] + pose.vel[k] * step;
        }
        if (onRoad && road(context, trial, there) != 0) {
            float turned[3] = { pose.vel[0], pose.vel[1], pose.vel[2] };

            // Half the step at the velocity it had and half at the one it ends with: a machine that goes
            // round a bend at a steady pace then stays in its lane, where whole steps would carry it out.
            Turn(here, there, turned);
            for (int k = 0; k < 3; k++) {
                pose.pos[k] += 0.5f * (pose.vel[k] + turned[k]) * step;
                pose.vel[k] = turned[k];
            }
            Turn(here, there, pose.forward);
            Turn(here, there, pose.up);
            for (int k = 0; k < 9; k++) {
                here[k] = there[k];
            }
        } else {
            for (int k = 0; k < 3; k++) {
                pose.pos[k] = trial[k];
            }
            onRoad = false;
        }
        ticks -= step;
    }
}

void DelayLine::Clear() {
    mHead = 0;
    mCount = 0;
}

void DelayLine::Push(const GdxNetPose& pose, uint32_t dueTick) {
    if (mCount == kCapacity) {
        mHead = (mHead + 1) % kCapacity;
        mCount--;
    }
    Entry& entry = mEntries[(mHead + mCount) % kCapacity];
    entry.pose = pose;
    entry.due = dueTick;
    mCount++;
}

bool DelayLine::Pop(uint32_t nowTick, GdxNetPose& out) {
    if (mCount == 0 || TicksFrom(mEntries[mHead].due, nowTick) < 0) {
        return false;
    }
    out = mEntries[mHead].pose;
    mHead = (mHead + 1) % kCapacity;
    mCount--;
    return true;
}

namespace {

// Poses waiting for the other thread, oldest first.
template <size_t N> struct PoseBox {
    std::array<GdxNetSlotPose, N> items{};
    size_t head = 0;
    size_t count = 0;

    void Clear() {
        head = count = 0;
    }

    // True when the box was full and its oldest pose made room.
    bool Push(int slot, const GdxNetPose& pose) {
        const bool full = count == N;

        if (full) {
            head = (head + 1) % N;
            count--;
        }
        GdxNetSlotPose& item = items[(head + count) % N];
        item.slot = slot;
        item.pose = pose;
        count++;
        return full;
    }

    bool Pop(GdxNetSlotPose& out) {
        if (count == 0) {
            return false;
        }
        out = items[head];
        head = (head + 1) % N;
        count--;
        return true;
    }
};

// Shared by the game thread and the network thread: every access takes the lock, and the questions the
// game asks for every racer every tick are single atomic reads.
struct Store {
    std::mutex lock;
    std::array<PoseBuffer, GDX_NET_RACE_MAX_RACERS> buffers;
    DelayLine echo;
    int echoSlot = -1;
    uint32_t echoDelay = 0;
    // The race the poses belong to, set by gdx_net_race_link.
    uint32_t raceIndex = 0;
    int self = 0;
    PoseBox<GDX_NET_RACE_OUT_CAPACITY> outgoing;
    PoseBox<GDX_NET_RACE_IN_CAPACITY> incoming;
    GdxNetRaceStats stats{};
};

Store& TheStore() {
    static Store store;
    return store;
}

std::atomic<uint32_t> sPuppets{ 0 };
std::atomic<int> sActive{ 0 };
std::atomic<int> sOnline{ 0 };
std::atomic<int> sLinked{ 0 };

// Only the game thread reads and writes these; the flag keeps the readers to one load.
std::atomic<int> sRulesSet{ 0 };
GdxNetRules sRules{};

gdx::hash::Sha256 sDigest{};

// The ticks the game does something on besides counting: the three beeps of the countdown.
constexpr int kIntroBeeps[] = { 220, 160, 100 };

// xorshift32: small, and the same on every compiler. Never 0.
uint32_t NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool SlotOk(int slot) {
    return slot >= 0 && slot < GDX_NET_RACE_MAX_RACERS;
}

// The caller holds the lock.
void QueueOutgoing(Store& store, int raceSlot, const GdxNetPose& pose) {
    if (store.outgoing.Push(raceSlot, pose)) {
        store.stats.dropped++;
    }
    store.stats.sent++;
}

} // namespace

} // namespace gdx::net::race

using namespace gdx::net::race;

void gdx_net_race_reset(void) {
    Store& store = TheStore();
    std::lock_guard<std::mutex> guard(store.lock);

    for (PoseBuffer& buffer : store.buffers) {
        buffer.Clear();
    }
    store.echo.Clear();
    store.echoSlot = -1;
    store.echoDelay = 0;
    store.raceIndex = 0;
    store.self = 0;
    store.outgoing.Clear();
    store.incoming.Clear();
    store.stats = GdxNetRaceStats{};
    sPuppets.store(0);
    sActive.store(0);
    sOnline.store(0);
    sLinked.store(0);
    sRulesSet.store(0);
}

void gdx_net_race_set_active(int on) {
    sActive.store(on ? 1 : 0);
}

int gdx_net_race_active(void) {
    return sActive.load();
}

void gdx_net_race_set_online(int on) {
    sOnline.store(on ? 1 : 0);
    if (!on) {
        sRulesSet.store(0);
    }
}

int gdx_net_race_online(void) {
    return sOnline.load(std::memory_order_relaxed);
}

void gdx_net_race_build_grid(unsigned int seed, const unsigned char* machine, const unsigned char* skin, int humans,
                             int cpus, GdxNetGrid* out) {
    unsigned char pool[GDX_NET_RACE_MAX_RACERS];
    bool taken[GDX_NET_RACE_MAX_RACERS] = {};
    int free = 0;
    uint32_t state = seed != 0 ? seed : 0x9E3779B9u;

    if (out == nullptr) {
        return;
    }
    *out = GdxNetGrid{};
    if (machine == nullptr || skin == nullptr || humans < 1 || cpus < 0 ||
        humans + cpus > GDX_NET_RACE_MAX_RACERS) {
        return;
    }
    for (int i = 0; i < humans; i++) {
        out->character[i] = machine[i] < GDX_NET_RACE_MAX_RACERS ? machine[i] : 0;
        out->skin[i] = skin[i];
        taken[out->character[i]] = true;
    }
    for (int c = 0; c < GDX_NET_RACE_MAX_RACERS; c++) {
        if (!taken[c]) {
            pool[free++] = static_cast<unsigned char>(c);
        }
    }
    // Fisher-Yates. Two humans on the same machine leave more free machines than CPUs, never fewer.
    for (int i = free - 1; i > 0; i--) {
        std::swap(pool[i], pool[NextRandom(state) % static_cast<uint32_t>(i + 1)]);
    }
    for (int i = 0; i < cpus; i++) {
        out->character[humans + i] = free > 0 ? pool[i % free] : 0;
        out->skin[humans + i] = 0;
    }
    out->humans = humans;
    out->racers = humans + cpus;
}

int gdx_net_race_local_slot(int self, int raceSlot) {
    if (self <= 0) {
        return raceSlot; // slot 0 is ours already, or we are not in this race
    }
    if (raceSlot == self) {
        return 0;
    }
    return raceSlot == 0 ? self : raceSlot;
}

int gdx_net_race_grid_place(int humans, int racers, int raceSlot) {
    const int cpus = racers - humans;

    return raceSlot < humans ? cpus + 1 + raceSlot : raceSlot - humans + 1;
}

int gdx_net_race_intro_target(int elapsedMs) {
    if (elapsedMs < 0) {
        return GDX_NET_INTRO_START;
    }
    // The tick with the start value runs at the start time itself, so one tick is gone at 0 ms.
    const long long ticks = (static_cast<long long>(elapsedMs) * GDX_NET_INTRO_TICKS_PER_SECOND) / 1000 + 1;
    const long long target = GDX_NET_INTRO_START - ticks;
    return target < GDX_NET_INTRO_GO ? GDX_NET_INTRO_GO : static_cast<int>(target);
}

int gdx_net_race_intro_run(int timer, int elapsedMs) {
    if (timer <= GDX_NET_INTRO_GO) {
        return 1;
    }
    if (elapsedMs < 0) {
        return 0;
    }
    // A beep is played by whatever draws the timer while it holds that value: showing one twice would
    // play it twice. A copy that is ahead there waits on the next tick instead.
    for (int beep : kIntroBeeps) {
        if (timer == beep) {
            return 1;
        }
    }
    return timer - 1 >= gdx_net_race_intro_target(elapsedMs) ? 1 : 0;
}

int gdx_net_race_intro_catch_up(int timer, int elapsedMs) {
    if (timer <= GDX_NET_INTRO_GO || elapsedMs < 0) {
        return timer;
    }
    int target = gdx_net_race_intro_target(elapsedMs);
    if (timer <= target) {
        return timer;
    }
    for (int beep : kIntroBeeps) {
        if (timer == beep) {
            return timer; // this tick is the beep: it is shown before anything is skipped
        }
        if (timer > beep && target < beep) {
            target = beep;
            break; // the beeps are listed from the first to the last
        }
    }
    return target;
}

void gdx_net_race_set_rules(const GdxNetRules* rules) {
    if (rules == nullptr) {
        sRulesSet.store(0);
        return;
    }
    sRules = *rules;
    sRulesSet.store(1);
}

int gdx_net_race_rule_ignore_track_settings(int own) {
    return sRulesSet.load(std::memory_order_relaxed) ? sRules.ignoreTrackSettings : own;
}

int gdx_net_race_rule_lava_no_kill(int own) {
    return sRulesSet.load(std::memory_order_relaxed) ? sRules.lavaNoKill : own;
}

float gdx_net_race_rule_magnet_range(float own) {
    return sRulesSet.load(std::memory_order_relaxed) ? sRules.magnetRange : own;
}

float gdx_net_race_rule_magnet_strength(float own) {
    return sRulesSet.load(std::memory_order_relaxed) ? sRules.magnetStrength : own;
}

void gdx_net_race_digest_begin(void) {
    gdx::hash::sha256_init(&sDigest);
}

void gdx_net_race_digest_feed(const void* bytes, unsigned int len) {
    if (bytes != nullptr && len > 0) {
        gdx::hash::sha256_update(&sDigest, bytes, len);
    }
}

void gdx_net_race_digest_end(unsigned char out[32]) {
    gdx::hash::sha256_final(&sDigest, out);
}

void gdx_net_race_set_puppet(int slot, int on) {
    if (!SlotOk(slot)) {
        return;
    }
    if (on) {
        sPuppets.fetch_or(1u << slot);
    } else {
        sPuppets.fetch_and(~(1u << slot));
    }
}

int gdx_net_race_is_puppet(int slot) {
    return SlotOk(slot) && ((sPuppets.load(std::memory_order_relaxed) >> slot) & 1u) != 0;
}

void gdx_net_race_push(int slot, const GdxNetPose* pose) {
    Store& store = TheStore();

    if (!SlotOk(slot) || pose == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    store.buffers[slot].Push(*pose);
}

int gdx_net_race_sample(int slot, unsigned int localTick, unsigned int nowMs, GdxNetPose* out, float* ahead) {
    Store& store = TheStore();

    if (!SlotOk(slot) || out == nullptr || ahead == nullptr) {
        return GDX_NET_SAMPLE_NONE;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    const int result = store.buffers[slot].Sample(localTick, nowMs, *out, *ahead);

    if (result & GDX_NET_SAMPLE_HELD) {
        store.stats.held++;
    }
    if (result & GDX_NET_SAMPLE_STEP) {
        store.stats.steps++;
    }
    return result;
}

void gdx_net_race_advance(GdxNetPose* pose, float ticks, GdxNetRoadFrame road, void* context) {
    if (pose != nullptr) {
        Advance(*pose, ticks, road, context);
    }
}

void gdx_net_race_shove(int slot, unsigned int localTick, const float push[3]) {
    Store& store = TheStore();

    if (!SlotOk(slot) || push == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    store.buffers[slot].Shove(localTick, push);
}

void gdx_net_race_shoved(int slot, unsigned int localTick, float offset[3], float velocity[3]) {
    Store& store = TheStore();

    if (offset == nullptr || velocity == nullptr) {
        return;
    }
    if (!SlotOk(slot)) {
        for (int k = 0; k < 3; k++) {
            offset[k] = velocity[k] = 0.0f;
        }
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    store.buffers[slot].Shoved(localTick, offset, velocity);
}

int gdx_net_race_settle(int slot, unsigned int localTick, const float target[3], const float velocity[3],
                        float out[3]) {
    Store& store = TheStore();

    if (!SlotOk(slot) || target == nullptr || velocity == nullptr || out == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    const bool put = store.buffers[slot].Settle(localTick, target, velocity, out);

    if (put) {
        store.stats.snaps++;
    }
    return put ? 1 : 0;
}

void gdx_net_race_echo(int slot, unsigned int delayTicks) {
    Store& store = TheStore();
    std::lock_guard<std::mutex> guard(store.lock);

    store.echo.Clear();
    if (SlotOk(slot) && delayTicks > 0) {
        store.echoSlot = slot;
        store.echoDelay = delayTicks;
    } else {
        store.echoSlot = -1;
        store.echoDelay = 0;
    }
}

void gdx_net_race_local_pose(const GdxNetPose* pose, unsigned int localTick) {
    Store& store = TheStore();
    std::lock_guard<std::mutex> guard(store.lock);
    GdxNetPose due;

    // In a room's race it leaves for the room. The developer echo sends it back to ourselves instead.
    if (pose != nullptr && sLinked.load(std::memory_order_relaxed) && pose->tick % kSendIntervalTicks == 0) {
        QueueOutgoing(store, store.self, *pose);
    }
    if (store.echoSlot < 0) {
        return;
    }
    while (store.echo.Pop(localTick, due)) {
        store.buffers[store.echoSlot].Push(due);
    }
    if (pose != nullptr && pose->tick % kSendIntervalTicks == 0) {
        store.echo.Push(*pose, localTick + store.echoDelay);
    }
}

void gdx_net_race_link(unsigned int raceIndex, int self) {
    Store& store = TheStore();
    std::lock_guard<std::mutex> guard(store.lock);

    store.outgoing.Clear();
    store.incoming.Clear();
    if (!SlotOk(self)) {
        sLinked.store(0);
        return;
    }
    store.raceIndex = raceIndex;
    store.self = self;
    sLinked.store(1);
}

int gdx_net_race_send_due(unsigned int tick) {
    return sLinked.load(std::memory_order_relaxed) && tick % kSendIntervalTicks == 0;
}

void gdx_net_race_send_pose(int raceSlot, const GdxNetPose* pose) {
    Store& store = TheStore();

    if (!SlotOk(raceSlot) || pose == nullptr || !gdx_net_race_send_due(pose->tick)) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    QueueOutgoing(store, raceSlot, *pose);
}

int gdx_net_race_take_outgoing(unsigned int* raceIndex, GdxNetSlotPose* out, int capacity) {
    Store& store = TheStore();
    int taken = 0;

    if (raceIndex == nullptr || out == nullptr || capacity <= 0 || !sLinked.load(std::memory_order_relaxed)) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    if (!sLinked.load()) {
        return 0; // the race ended while we waited for the lock
    }
    *raceIndex = store.raceIndex;
    while (taken < capacity && store.outgoing.Pop(out[taken])) {
        taken++;
    }
    return taken;
}

void gdx_net_race_deliver(unsigned int raceIndex, int raceSlot, const GdxNetPose* pose) {
    Store& store = TheStore();

    if (!SlotOk(raceSlot) || pose == nullptr || !sLinked.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    if (!sLinked.load() || raceIndex != store.raceIndex) {
        return; // a pose of the race before this one, still on its way
    }
    if (store.incoming.Push(raceSlot, *pose)) {
        store.stats.dropped++;
    }
}

void gdx_net_race_receive(void) {
    Store& store = TheStore();
    GdxNetSlotPose item;

    if (!sLinked.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    while (store.incoming.Pop(item)) {
        const int local = gdx_net_race_local_slot(store.self, item.slot);

        // Slot 0 is our own machine, and a slot that is no puppet is one this copy drives itself.
        if (local == 0 || !gdx_net_race_is_puppet(local)) {
            continue;
        }
        store.buffers[local].Push(item.pose);
        store.stats.received++;
    }
}

void gdx_net_race_stats(GdxNetRaceStats* out) {
    Store& store = TheStore();

    if (out == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(store.lock);
    *out = store.stats;
}

float gdx_net_race_progress(float lapDistance, int lap, float courseLength, float previous, int snapped,
                            int* lapOut) {
    float distance = static_cast<float>(lap - 1) * courseLength + lapDistance;

    // Only next to the line can the lap and the place disagree by a pose. Anywhere else the owner's lap
    // stands, so a real change (a restart, a lap taken back) is followed and not argued with.
    if (!snapped && courseLength > 0.0f &&
        (lapDistance < 0.1f * courseLength || lapDistance > 0.9f * courseLength)) {
        if (distance - previous < -0.5f * courseLength) {
            distance += courseLength;
            lap++;
        } else if (distance - previous > 0.5f * courseLength) {
            distance -= courseLength;
            lap--;
        }
    }
    if (lapOut != nullptr) {
        *lapOut = lap;
    }
    return distance;
}
