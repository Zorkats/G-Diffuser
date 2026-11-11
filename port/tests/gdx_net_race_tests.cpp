// Copyright (c) Zorkats
//
// port/tests/gdx_net_race_tests.cpp: the pose buffer of an online race (port/net/gdx_net_race.h), and
// the start of one: the grid, the intro on the host clock, the rules and the course digest.
//
// No network, no game and no real clock. A Sim plays one remote machine driving in a straight line at ten
// units a tick and the copy that shows it, each with its own tick times on one clock that counts in whole
// ms. Because the motion is a straight line at a steady pace, where the machine really is at any moment
// is known, and the place guessed for it is held against that.

#include "../net/gdx_net_race.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

namespace {

using namespace gdx::net::race;

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

constexpr float kSpeed = 10.0f;
constexpr double kTickMs = 1000.0 / 60.0;

bool Near(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

// What the room's clock says at tick `tick` of a game that started with the clock and never drops a frame.
uint32_t StampOf(uint32_t tick) {
    return static_cast<uint32_t>(static_cast<uint64_t>(std::floor(static_cast<double>(tick) * kTickMs)));
}

// The owner's machine at its tick `tick`, driving along x. The flags carry the tick so a test can tell
// which pose a sample took them from.
GdxNetPose PoseAt(uint32_t tick, float x) {
    GdxNetPose pose = {};
    pose.tick = tick;
    pose.timeMs = StampOf(tick);
    pose.pos[0] = x;
    pose.vel[0] = kSpeed;
    pose.forward[0] = 1.0f;
    pose.up[1] = 1.0f;
    pose.lap = 1;
    pose.flags = tick;
    return pose;
}

GdxNetPose OnLine(uint32_t tick) {
    return PoseAt(tick, kSpeed * static_cast<float>(tick));
}

GdxNetRaceStats Stats() {
    GdxNetRaceStats stats = {};

    gdx_net_race_stats(&stats);
    return stats;
}

// One remote machine and the copy that shows it, on one clock. The owner's machine drives along x at
// kSpeed a tick of its own game and sends its pose every second tick; a pose takes a while to arrive.
// Each of our ticks we ask where the machine is now. The motion is a straight line at a steady pace, so
// where the machine really is at any moment is known, and the guess is held against it.
struct Sim {
    BufferConfig config;
    double ownerTickMs = kTickMs; // one tick of the owner's game
    double tickMs = kTickMs;      // one of ours
    double tripMs = 50.0;
    uint32_t ownerBase = 5000; // the two games count their ticks from different numbers
    uint32_t localBase = 1000;
    uint32_t clockBase = 0;
    std::set<uint32_t> lost;        // the owner's ticks, counted from 0, whose pose never arrives
    uint32_t lastSent = 0xFFFFFFFF; // the owner stops sending after this tick
    int ownerSkipsAt = -1;          // the owner's game loses a frame before this tick of its own
    int weSkipAt = -1;              // ours loses one before this tick
    double (*late)(uint32_t) = nullptr; // how many ms late our tick read the clock
    double (*trip)(uint32_t) = nullptr; // the trip of the pose of the owner's tick, where it is not tripMs

    PoseBuffer buffer;
    // One entry for each of our ticks. Before the first pose the numbers are 0 and the result NONE.
    std::vector<int> result;
    std::vector<double> error; // the guess minus the truth, in the owner's ticks
    std::vector<double> step;  // how far the guess moved since our last tick, in the owner's ticks
    std::vector<float> ahead;
    std::vector<uint32_t> poseTick;
    int steps = 0;

    double OwnerTime(uint32_t i) const {
        return 3.0 + (static_cast<double>(i) + ((ownerSkipsAt >= 0 && i >= static_cast<uint32_t>(ownerSkipsAt)) ? 1.0 : 0.0)) * ownerTickMs;
    }

    // The owner's tick count at a time, fractions included: between two ticks its machine is on its way.
    double OwnerTickAt(double ms) const {
        double i = (ms - 3.0) / ownerTickMs;

        if (ownerSkipsAt >= 0 && i >= static_cast<double>(ownerSkipsAt - 1)) {
            const double since = i - static_cast<double>(ownerSkipsAt - 1); // the frame it lost is in here
            i = since < 2.0 ? static_cast<double>(ownerSkipsAt - 1) + 0.5 * since : i - 1.0;
        }
        return i;
    }

    double OurTime(uint32_t n) const {
        return (static_cast<double>(n) + ((weSkipAt >= 0 && n >= static_cast<uint32_t>(weSkipAt)) ? 1.0 : 0.0)) * tickMs +
               (late != nullptr ? late(n) : 0.0);
    }

    void Run(uint32_t ticks) {
        std::vector<bool> delivered;
        uint32_t first = 0;
        double lastGuess = 0.0;
        bool guessed = false;

        buffer = PoseBuffer(config);
        for (uint32_t n = 0; n < ticks; n++) {
            const double now = OurTime(n);

            for (uint32_t i = first; OwnerTime(i) <= now; i += kSendIntervalTicks) {
                const size_t at = i / kSendIntervalTicks;

                if (delivered.size() <= at) {
                    delivered.resize(at + 1, false);
                }
                if (!delivered[at] && OwnerTime(i) + (trip != nullptr ? trip(i) : tripMs) <= now) {
                    delivered[at] = true;
                    if (i <= lastSent && lost.count(i) == 0) {
                        GdxNetPose pose = PoseAt(ownerBase + i, kSpeed * static_cast<float>(i));

                        pose.timeMs = clockBase + static_cast<uint32_t>(std::floor(OwnerTime(i)));
                        buffer.Push(pose);
                    }
                }
            }
            while (first / kSendIntervalTicks < delivered.size() && delivered[first / kSendIntervalTicks]) {
                first += kSendIntervalTicks;
            }

            GdxNetPose out = {};
            float far = 0.0f;
            const int r = buffer.Sample(localBase + n, clockBase + static_cast<uint32_t>(std::floor(now)), out, far);

            result.push_back(r);
            ahead.push_back(far);
            poseTick.push_back(out.tick);
            if (r & GDX_NET_SAMPLE_STEP) {
                steps++;
            }
            if (r & GDX_NET_SAMPLE_POSE) {
                Advance(out, far, nullptr, nullptr);
                const double guess = static_cast<double>(out.pos[0]) / kSpeed;

                error.push_back(guess - OwnerTickAt(now));
                step.push_back(guessed ? guess - lastGuess : 0.0);
                lastGuess = guess;
                guessed = true;
            } else {
                error.push_back(0.0);
                step.push_back(0.0);
            }
        }
    }

    // The largest error, either way, of our ticks `from` to `to`.
    double Worst(uint32_t from, uint32_t to) const {
        double worst = 0.0;

        for (uint32_t n = from; n < to && n < error.size(); n++) {
            worst = std::max(worst, std::fabs(error[n]));
        }
        return worst;
    }

    double Mean(uint32_t from, uint32_t to) const {
        double sum = 0.0;

        for (uint32_t n = from; n < to; n++) {
            sum += error[n];
        }
        return sum / static_cast<double>(to - from);
    }

    // How far from `pace` the guess moved in one of our ticks, at most.
    double Swing(uint32_t from, uint32_t to, double pace = 1.0) const {
        double worst = 0.0;

        for (uint32_t n = from; n < to; n++) {
            worst = std::max(worst, std::fabs(step[n] - pace));
        }
        return worst;
    }

    bool Held(uint32_t from, uint32_t to) const {
        for (uint32_t n = from; n < to; n++) {
            if (result[n] & GDX_NET_SAMPLE_HELD) {
                return true;
            }
        }
        return false;
    }
};

void TestEmpty() {
    PoseBuffer buffer;
    GdxNetPose out = {};
    float ahead = 7.0f;
    float offset[3] = { 9.0f, 9.0f, 9.0f };
    float velocity[3] = { 9.0f, 9.0f, 9.0f };

    CHECK(buffer.Sample(5, 80, out, ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(!buffer.Has() && buffer.Shoves() == 0 && Near(buffer.Pace(), kTicksPerMs, 0.0001f));
    buffer.Shoved(5, offset, velocity);
    CHECK(offset[0] == 0.0f && offset[2] == 0.0f && velocity[0] == 0.0f && velocity[2] == 0.0f);
}

// The normal case: two games at the same pace. The newest pose is as old as its trip and up to two ticks
// more, and guessed ahead by that much it is where the machine is, to the ms the clock counts in.
void TestSteady() {
    std::printf("the guess: two games at the same pace\n");
    Sim sim;

    sim.Run(600);
    CHECK(sim.result[2] == GDX_NET_SAMPLE_NONE); // the first pose is still on its way
    CHECK(sim.result[4] == GDX_NET_SAMPLE_POSE);
    CHECK(sim.Worst(4, 600) < 0.08);
    CHECK(sim.Worst(120, 600) < 0.05);
    CHECK(sim.Swing(120, 600) < 0.02);
    CHECK(sim.steps == 0 && !sim.Held(0, 600));
    bool aged = true;
    bool newest = true;
    for (uint32_t n = 10; n < 600; n++) {
        aged = aged && sim.ahead[n] > 2.8f && sim.ahead[n] < 5.1f;
        // The pose that arrived last: sent four ticks ago, or the tick before that.
        newest = newest && sim.poseTick[n] == sim.ownerBase + ((n - 4) & ~1u);
    }
    CHECK(aged && newest);
    CHECK(Near(sim.buffer.Lead(), 4000.0f, 0.3f));
    CHECK(Near(sim.buffer.Pace(), kTicksPerMs, 0.0003f));

    // The two tick counts and the clock all pass their largest number during the race.
    Sim wrap;
    wrap.ownerBase = 0xFFFFFF80u;
    wrap.localBase = 0xFFFFFE00u;
    wrap.clockBase = 0xFFFFF000u;
    wrap.Run(600);
    CHECK(wrap.result[4] == GDX_NET_SAMPLE_POSE && wrap.steps == 0 && !wrap.Held(0, 600));
    CHECK(wrap.Swing(120, 600) < 0.02);
    CHECK(Near(wrap.buffer.Lead(), 384.0f, 0.3f));
    bool onward = true;
    for (uint32_t n = 10; n < 600; n++) {
        onward = onward && wrap.ahead[n] > 2.8f && wrap.ahead[n] < 5.1f;
    }
    CHECK(onward);
}

// What the old display could not do: the trip changes and the machine shown does not move, because a pose
// says when it was taken and how long it travelled no longer matters.
void TestTripChange() {
    std::printf("the guess: a trip that changes\n");
    Sim sim;

    sim.trip = [](uint32_t i) { return i < 300 ? 30.0 : 120.0; };
    sim.Run(600);
    CHECK(sim.Worst(120, 600) < 0.06);
    CHECK(sim.Swing(120, 600) < 0.02);
    CHECK(sim.steps == 0 && !sim.Held(0, 600));
    CHECK(sim.ahead[200] < 4.0f && sim.ahead[500] > 7.0f);

    // Every pose with a trip of its own, some overtaking others.
    Sim shaky;
    shaky.trip = [](uint32_t i) { return 20.0 + static_cast<double>((i * 2654435761u) >> 26); }; // 20 to 83 ms
    shaky.Run(600);
    CHECK(shaky.Worst(120, 600) < 0.06);
    CHECK(shaky.Swing(120, 600) < 0.02);
    CHECK(shaky.steps == 0 && !shaky.Held(0, 600));
}

// Our game does not read the clock at the same point of every frame. Taken as it comes, each reading
// would move the machine by that much; the lead is followed slowly so it does not.
void TestJitter() {
    std::printf("the guess: a clock read at uneven moments\n");
    Sim sim;

    sim.late = [](uint32_t n) { return (n % 2) != 0 ? 6.0 : 0.0; }; // 6 ms is 0.36 of a tick
    sim.Run(600);
    CHECK(sim.Swing(120, 600) < 0.06);
    CHECK(std::fabs(sim.Mean(120, 600)) < 0.05); // in the middle of the readings, not at one end
    CHECK(sim.steps == 0);

    Sim raw;
    raw.config.leadGain = 1.0f; // every reading as it comes
    raw.config.leadDriftGain = 0.0f;
    raw.late = sim.late;
    raw.Run(600);
    CHECK(raw.Swing(120, 600) > 0.25);
}

// A game that cannot hold its 60 frames runs slower than the clock. Its pace is measured from its poses,
// and the lead, which now falls by a little every tick, is followed with nothing left over.
void TestSlowOwner() {
    std::printf("the guess: an owner whose game runs 5%% slow\n");
    Sim sim;

    sim.ownerTickMs = kTickMs / 0.95;
    sim.Run(900);
    CHECK(sim.Worst(400, 900) < 0.08);
    CHECK(std::fabs(sim.Mean(400, 900)) < 0.03);
    CHECK(sim.Swing(400, 900, 0.95) < 0.02);
    CHECK(sim.steps == 0);
    CHECK(Near(sim.buffer.Pace(), 0.95f * kTicksPerMs, 0.0004f));

    // Without the pace a pose is taken to be more of the owner's ticks old than it is.
    Sim paceless;
    paceless.config.paceGain = 0.0f;
    paceless.ownerTickMs = sim.ownerTickMs;
    paceless.Run(900);
    CHECK(paceless.Mean(400, 900) > 0.12);

    // Without the drift the lead is always on its way to where it should be.
    Sim driftless;
    driftless.config.leadDriftGain = 0.0f;
    driftless.ownerTickMs = sim.ownerTickMs;
    driftless.Run(900);
    CHECK(driftless.Mean(400, 900) > 0.25);

    // Ours is the slow one: the same, the other way round.
    Sim slowUs;
    slowUs.tickMs = kTickMs / 0.95;
    slowUs.Run(900);
    CHECK(slowUs.Worst(400, 900) < 0.08);
    CHECK(slowUs.Swing(400, 900, 1.0 / 0.95) < 0.02);
    CHECK(slowUs.steps == 0);

    // A game at a fifth of its pace is outside what is believed: the pace stops at its least.
    Sim crawl;
    crawl.ownerTickMs = kTickMs * 5.0;
    crawl.Run(900);
    CHECK(Near(crawl.buffer.Pace(), kPaceLeast, 0.0001f));
}

// A game that drops a frame loses a whole tick against the clock. That is a step, not a shaky reading:
// the lead takes it at once, where easing into it would show the machine a tick off for a second.
void TestSkippedTick() {
    std::printf("the guess: a game that drops a frame\n");
    Sim sim;

    sim.ownerSkipsAt = 200;
    sim.Run(600);
    // Nobody here knows until the first pose from after it arrives, three ticks later.
    CHECK(sim.Worst(120, 199) < 0.05);
    CHECK(sim.error[203] > 0.5);
    CHECK(sim.steps == 1);
    CHECK(sim.Worst(208, 260) < 0.15); // the pace measured across the lost frame is a little low for a while
    CHECK(sim.Worst(400, 600) < 0.06);

    Sim easing;
    easing.config.leadResyncTicks = 100.0f; // never a step
    easing.ownerSkipsAt = 200;
    easing.Run(600);
    CHECK(easing.steps == 0);
    CHECK(easing.error[210] > 0.3);

    // Ours drops one: the clock says so on the very tick.
    Sim ours;
    ours.weSkipAt = 200;
    ours.Run(600);
    CHECK(ours.steps == 1 && (ours.result[200] & GDX_NET_SAMPLE_STEP));
    CHECK(ours.Worst(120, 600) < 0.08);
}

// Lost poses cost nothing while the machine does what it did: the guess runs on from an older pose. A
// machine nothing is heard of stands after kAheadMostTicks.
void TestLossAndHold() {
    std::printf("the guess: poses that never arrive\n");
    Sim sim;

    sim.lost = { 100, 102, 104, 106, 108 };
    sim.Run(600);
    CHECK(sim.Worst(60, 600) < 0.06);
    CHECK(sim.steps == 0 && !sim.Held(0, 600));
    CHECK(sim.poseTick[112] == sim.ownerBase + 98 && sim.ahead[112] > 13.0f);

    Sim silent;
    silent.lastSent = 300;
    silent.Run(600);
    CHECK(silent.Worst(60, 340) < 0.1);
    CHECK(!silent.Held(0, 340));
    bool stands = true;
    for (uint32_t n = 345; n < 600; n++) {
        stands = stands && silent.result[n] == (GDX_NET_SAMPLE_POSE | GDX_NET_SAMPLE_HELD) &&
                 silent.ahead[n] == kAheadMostTicks && silent.poseTick[n] == silent.ownerBase + 300;
    }
    CHECK(stands);
    CHECK(silent.Swing(346, 600, 0.0) < 0.001);

    Sim sooner;
    sooner.config.aheadMostTicks = 10.0f;
    sooner.lastSent = 300;
    sooner.Run(400);
    CHECK(!sooner.Held(0, 310) && sooner.Held(316, 317));
}

// Only the newest pose is used: one that arrives late, or twice, changes nothing.
void TestLateAndDuplicate() {
    PoseBuffer buffer;
    GdxNetPose out = {};
    float ahead = 0.0f;

    CHECK(buffer.Push(OnLine(10)));
    CHECK(!buffer.Push(OnLine(10)));
    CHECK(!buffer.Push(OnLine(8)));
    CHECK(buffer.Sample(13, StampOf(13), out, ahead) == GDX_NET_SAMPLE_POSE && out.tick == 10);
    CHECK(buffer.Push(OnLine(11)));
    CHECK(buffer.Sample(14, StampOf(14), out, ahead) == GDX_NET_SAMPLE_POSE && out.tick == 11);
    CHECK(Near(ahead, 3.0f, 0.07f));
    // A tick count that passed its largest number is still newer.
    PoseBuffer wrap;
    CHECK(wrap.Push(OnLine(0xFFFFFFFEu)) && wrap.Push(OnLine(2)) && !wrap.Push(OnLine(0xFFFFFFFEu)));
    // A pose stamped later than our reading of the clock is shown as it is, not guessed backwards.
    PoseBuffer early;
    GdxNetPose pose = OnLine(10);
    pose.timeMs = 500;
    early.Push(pose);
    CHECK(early.Sample(10, 400, out, ahead) == GDX_NET_SAMPLE_POSE);
    pose.tick = 12;
    pose.timeMs = 533;
    early.Push(pose);
    CHECK(early.Sample(11, 417, out, ahead) == GDX_NET_SAMPLE_POSE && ahead == 0.0f);

    buffer.Clear();
    CHECK(!buffer.Has() && buffer.Sample(14, StampOf(14), out, ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(buffer.Push(OnLine(4)));
}

// ---- The road (slice S6) ----

// A road that is a circle in the xz plane round the origin, driven anticlockwise seen from above.
struct Ring {
    float radius = 2000.0f;
    float halfWidth = 400.0f;
    int calls = 0;
    int endsAfter = 1 << 30; // calls answered before the road is gone
};

int RingRoad(void* context, const float pos[3], float frame[9]) {
    Ring& ring = *static_cast<Ring*>(context);
    const float r = std::sqrt(pos[0] * pos[0] + pos[2] * pos[2]);

    if (ring.calls++ >= ring.endsAfter || r < 1.0f || std::fabs(r - ring.radius) > ring.halfWidth) {
        return 0;
    }
    const float x = pos[0] / r;
    const float z = pos[2] / r;
    const float made[9] = { -z, 0.0f, x, 0.0f, 1.0f, 0.0f, -x, 0.0f, -z }; // forward, up, left (to the middle)

    std::copy(made, made + 9, frame);
    return 1;
}

// A road that is a loop in the xy plane, driven on its inside: up is toward the middle. The context
// counts the calls.
int LoopRoad(void* context, const float pos[3], float frame[9]) {
    const float r = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1]);

    (*static_cast<int*>(context))++;
    if (r < 1.0f) {
        return 0;
    }
    const float x = pos[0] / r;
    const float y = pos[1] / r;
    const float made[9] = { -y, x, 0.0f, -x, -y, 0.0f, 0.0f, 0.0f, 1.0f };

    std::copy(made, made + 9, frame);
    return 1;
}

float RadiusOf(const GdxNetPose& pose) {
    return std::sqrt(pose.pos[0] * pose.pos[0] + pose.pos[2] * pose.pos[2]);
}

// A machine 30 units outside the middle of the ring road and 5 above it, at 36 units a tick.
GdxNetPose OnRing() {
    GdxNetPose pose = {};

    pose.pos[0] = 2030.0f;
    pose.pos[1] = 5.0f;
    pose.vel[2] = 36.0f;
    pose.forward[2] = 1.0f;
    pose.up[1] = 1.0f;
    return pose;
}

void TestRoad() {
    std::printf("the guess: along the road\n");
    Ring ring;
    GdxNetPose pose = OnRing();

    // Eight ticks round the bend: still in its lane, at its height, at its speed, facing along the road.
    Advance(pose, 8.0f, RingRoad, &ring);
    const float angle = std::atan2(pose.pos[2], pose.pos[0]);
    CHECK(Near(RadiusOf(pose), 2030.0f, 0.05f));
    CHECK(Near(angle, 8.0f * 36.0f / 2030.0f, 0.0005f));
    CHECK(Near(pose.pos[1], 5.0f, 0.001f));
    CHECK(Near(std::sqrt(pose.vel[0] * pose.vel[0] + pose.vel[2] * pose.vel[2]), 36.0f, 0.01f) && pose.vel[1] == 0.0f);
    CHECK(Near(pose.vel[0] * std::cos(angle) + pose.vel[2] * std::sin(angle), 0.0f, 0.02f)); // none of it outward
    CHECK(Near(pose.forward[0], -std::sin(angle), 0.001f) && Near(pose.forward[2], std::cos(angle), 0.001f));
    CHECK(Near(pose.up[0], 0.0f, 0.001f) && Near(pose.up[1], 1.0f, 0.001f) && Near(pose.up[2], 0.0f, 0.001f));
    CHECK(ring.calls <= 10);

    // The same eight ticks in a straight line leave the lane by 20 units.
    GdxNetPose straight = OnRing();
    Advance(straight, 8.0f, nullptr, nullptr);
    CHECK(Near(straight.pos[0], 2030.0f, 0.001f) && Near(straight.pos[2], 288.0f, 0.001f));
    CHECK(RadiusOf(straight) - 2030.0f > 15.0f);
    CHECK(straight.forward[2] == 1.0f && straight.vel[2] == 36.0f);

    // A machine that drifts outward at 2 a tick and faces 0.2 into the bend keeps doing both.
    ring = Ring();
    GdxNetPose sliding = OnRing();
    sliding.vel[0] = 2.0f;
    sliding.forward[0] = -std::sin(0.2f);
    sliding.forward[2] = std::cos(0.2f);
    Advance(sliding, 8.0f, RingRoad, &ring);
    const float slid = std::atan2(sliding.pos[2], sliding.pos[0]);
    CHECK(Near(RadiusOf(sliding), 2046.0f, 0.3f));
    CHECK(Near(sliding.vel[0] * std::cos(slid) + sliding.vel[2] * std::sin(slid), 2.0f, 0.02f));
    CHECK(Near(sliding.forward[0] * -std::sin(slid) + sliding.forward[2] * std::cos(slid), std::cos(0.2f), 0.001f));

    // A part of a tick is a part of the step, and two parts are the whole.
    ring = Ring();
    GdxNetPose halves = OnRing();
    GdxNetPose whole = OnRing();
    Advance(halves, 0.5f, RingRoad, &ring);
    CHECK(Near(std::atan2(halves.pos[2], halves.pos[0]), 0.5f * 36.0f / 2030.0f, 0.0001f));
    Advance(halves, 2.0f, RingRoad, &ring);
    Advance(whole, 2.5f, RingRoad, &ring);
    CHECK(Near(halves.pos[0], whole.pos[0], 0.01f) && Near(halves.pos[2], whole.pos[2], 0.01f));
    CHECK(Near(RadiusOf(whole), 2030.0f, 0.02f));

    // Off the road it goes straight, and so it does where the road ends under it.
    Ring narrow;
    narrow.halfWidth = 10.0f;
    GdxNetPose off = OnRing();
    Advance(off, 8.0f, RingRoad, &narrow);
    CHECK(Near(off.pos[0], 2030.0f, 0.001f) && Near(off.pos[2], 288.0f, 0.001f) && off.forward[2] == 1.0f);
    Ring ends;
    ends.endsAfter = 5; // the road is there for four steps
    GdxNetPose leaving = OnRing();
    Advance(leaving, 8.0f, RingRoad, &ends);
    CHECK(RadiusOf(leaving) - 2030.0f > 2.0f && RadiusOf(leaving) - 2030.0f < 15.0f);
    CHECK(ends.calls <= 14);

    // A loop: the road turns upward, and the machine's up turns with it and stays toward the middle.
    GdxNetPose looping = {};
    int loopCalls = 0;
    looping.pos[0] = 2000.0f;
    looping.vel[1] = 36.0f;
    looping.forward[1] = 1.0f;
    looping.up[0] = -1.0f;
    Advance(looping, 8.0f, LoopRoad, &loopCalls);
    const float climbed = std::atan2(looping.pos[1], looping.pos[0]);
    CHECK(Near(climbed, 8.0f * 36.0f / 2000.0f, 0.0005f) && loopCalls == 9);
    CHECK(Near(std::sqrt(looping.pos[0] * looping.pos[0] + looping.pos[1] * looping.pos[1]), 2000.0f, 0.05f));
    CHECK(Near(looping.up[0], -std::cos(climbed), 0.001f) && Near(looping.up[1], -std::sin(climbed), 0.001f));
    CHECK(Near(looping.forward[0], -std::sin(climbed), 0.001f) && Near(looping.forward[1], std::cos(climbed), 0.001f));
    CHECK(looping.pos[2] == 0.0f && looping.up[2] == 0.0f);

    // No time, no move. And a number that makes no sense does not hang the game.
    GdxNetPose still = OnRing();
    Advance(still, 0.0f, RingRoad, &ring);
    Advance(still, -3.0f, RingRoad, &ring);
    Advance(still, std::nanf(""), RingRoad, &ring);
    CHECK(still.pos[0] == 2030.0f && still.pos[2] == 0.0f);
    Advance(still, 1.0e9f, nullptr, nullptr);
    CHECK(Near(still.pos[2], 36.0f * 4.0f * kAheadMostTicks, 0.1f));
}

// ---- A correction is spread (slice S6) ----

void TestSettle() {
    std::printf("the place shown: corrections\n");
    PoseBuffer buffer;
    const float moving[3] = { 10.0f, 0.0f, 2.0f };
    float target[3] = { 100.0f, 5.0f, -40.0f };
    float out[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t tick = 10;
    const auto drive = [&]() {
        for (int k = 0; k < 3; k++) {
            target[k] += moving[k];
        }
        tick++;
    };
    const auto at = [&](float off) {
        return Near(out[0], target[0], 0.001f) && Near(out[1], target[1], 0.001f) && Near(out[2] - target[2], off, 0.001f);
    };

    // The first place is where the guess is.
    CHECK(buffer.Settle(tick, target, moving, out) && at(0.0f));
    // A guess that moves as its velocity says leaves nothing to correct.
    bool exact = true;
    for (int i = 0; i < 10; i++) {
        drive();
        exact = exact && !buffer.Settle(tick, target, moving, out) && at(0.0f);
    }
    CHECK(exact);

    // The guess is 40 units from where it was expected: a quarter of what is left each tick.
    drive();
    target[2] += 40.0f;
    CHECK(!buffer.Settle(tick, target, moving, out) && at(-30.0f));
    drive();
    CHECK(!buffer.Settle(tick, target, moving, out) && at(-22.5f));
    drive();
    CHECK(!buffer.Settle(tick, target, moving, out) && at(-16.875f));
    // Two of our ticks at once: two quarters.
    drive();
    drive();
    CHECK(!buffer.Settle(tick, target, moving, out) && at(-16.875f * 0.5625f));
    for (int i = 0; i < 40; i++) {
        drive();
        buffer.Settle(tick, target, moving, out);
    }
    CHECK(at(0.0f));

    // Just inside the limit it is eased, just outside it is put there.
    drive();
    target[2] += 199.0f;
    CHECK(!buffer.Settle(tick, target, moving, out) && at(-149.25f));
    drive();
    target[2] += 60.0f; // 149.25 * 0.75 is still to go, and 60 more: 209.25
    CHECK(buffer.Settle(tick, target, moving, out) && at(0.0f));

    // A long silence is not a thousand ticks of motion.
    tick += 1000;
    CHECK(!buffer.Settle(tick, target, moving, out));
    CHECK(Near(out[0] - target[0], 80.0f * std::pow(0.75f, 8.0f), 0.01f));

    // The numbers are the config's.
    BufferConfig config;
    config.easeKeep = 0.5f;
    config.easeSnapUnits = 10.0f;
    PoseBuffer other(config);
    const float stopped[3] = { 0.0f, 0.0f, 0.0f };
    float place[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(other.Settle(1, place, stopped, out));
    place[2] = 8.0f;
    CHECK(!other.Settle(2, place, stopped, out) && Near(out[2], 4.0f, 0.001f));
    place[2] = 15.0f;
    CHECK(other.Settle(3, place, stopped, out) && out[2] == 15.0f);
    // A new race starts from nothing.
    other.Clear();
    place[2] = 1000.0f;
    CHECK(other.Settle(4, place, stopped, out) && out[2] == 1000.0f);
}

// ---- What we pushed a remote machine (slice S6) ----

// The owner's tick 5000 ran when the clock said 1000; our tick 1000 reads 1042, and so on at 60 a second.
struct ShoveRig {
    PoseBuffer buffer;
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    float velocity[3] = { 0.0f, 0.0f, 0.0f };

    void Pose(uint32_t ownerTick) {
        GdxNetPose pose = PoseAt(ownerTick, 0.0f);

        pose.timeMs = 1000 + static_cast<uint32_t>(std::floor((ownerTick - 5000) * kTickMs));
        buffer.Push(pose);
    }

    void Tick(uint32_t local) {
        GdxNetPose out = {};
        float ahead = 0.0f;

        buffer.Sample(local, 1042 + static_cast<uint32_t>(std::floor((local - 1000) * kTickMs)), out, ahead);
        buffer.Shoved(local, offset, velocity);
    }

    bool Is(float vx, float vz, float ox, float oz) const {
        return Near(velocity[0], vx, 0.001f) && Near(velocity[2], vz, 0.001f) && Near(offset[0], ox, 0.001f) &&
               Near(offset[2], oz, 0.001f) && velocity[1] == 0.0f && offset[1] == 0.0f;
    }
};

void TestShoves() {
    std::printf("the place shown: what we pushed\n");
    const float side[3] = { 0.0f, 0.0f, 2.0f };
    const float back[3] = { -1.0f, 0.0f, 0.0f };
    ShoveRig rig;

    rig.Pose(5000);
    rig.Tick(1000);
    CHECK(Near(rig.buffer.Lead(), 4002.52f, 0.01f) && rig.Is(0.0f, 0.0f, 0.0f, 0.0f));

    // The game adds a push to the velocity and moves the machine by it in the same tick, and every tick after.
    rig.buffer.Shove(1000, side);
    rig.Tick(1000);
    CHECK(rig.Is(0.0f, 2.0f, 0.0f, 2.0f));
    rig.buffer.Shove(1001, back);
    rig.buffer.Shove(1001, back); // two machines in the same tick are one push
    CHECK(rig.buffer.Shoves() == 2);
    rig.Tick(1001);
    CHECK(rig.Is(-2.0f, 2.0f, -2.0f, 4.0f));

    // A pose from before the bump does not hold it: ours still counts.
    rig.Pose(5002);
    rig.Tick(1002);
    CHECK(rig.Is(-2.0f, 2.0f, -4.0f, 6.0f));
    // Our tick 1000 was the owner's 5002 and a half. Its pose of tick 5003 is its own account of that bump.
    rig.Pose(5003);
    rig.Tick(1003);
    CHECK(rig.Is(-2.0f, 0.0f, -6.0f, 0.0f));
    rig.Pose(5004);
    rig.Tick(1004);
    CHECK(rig.Is(0.0f, 0.0f, 0.0f, 0.0f));

    // No more than kShoveCapacity ticks of pushes are kept, and none older than that counts.
    rig.buffer.Clear();
    CHECK(rig.buffer.Shoves() == 0);
    rig.Pose(5000);
    rig.Tick(1000);
    for (uint32_t tick = 1001; tick <= 1060; tick++) {
        rig.buffer.Shove(tick, side);
    }
    CHECK(rig.buffer.Shoves() == kShoveCapacity);
    rig.Tick(1060);
    CHECK(Near(rig.velocity[2], 2.0f * static_cast<float>(kShoveCapacity), 0.001f));
    rig.Tick(1070);
    CHECK(Near(rig.velocity[2], 2.0f * static_cast<float>(kShoveCapacity - 10), 0.001f));
}

void TestDelayLine() {
    DelayLine line;
    GdxNetPose out = {};

    CHECK(!line.Pop(0, out));
    line.Push(OnLine(2), 10);
    line.Push(OnLine(4), 12);
    CHECK(line.Size() == 2);
    CHECK(!line.Pop(9, out));
    CHECK(line.Pop(10, out) && out.tick == 2);
    CHECK(!line.Pop(11, out));
    CHECK(line.Pop(40, out) && out.tick == 4);
    CHECK(!line.Pop(40, out) && line.Size() == 0);

    // A full line drops its oldest pose.
    for (uint32_t i = 0; i < DelayLine::kCapacity + 3; i++) {
        line.Push(OnLine(i), i);
    }
    CHECK(line.Size() == DelayLine::kCapacity);
    CHECK(line.Pop(0xFFFF, out) && out.tick == 3);
    line.Clear();
    CHECK(line.Size() == 0 && !line.Pop(0xFFFF, out));
}

void TestProgress() {
    const float length = 1000.0f;
    int lap = -1;

    // Before the first crossing the game counts lap 0 and a negative distance.
    CHECK(Near(gdx_net_race_progress(990.0f, 0, length, -20.0f, 0, &lap), -10.0f) && lap == 0);
    // Mid lap: the owner's lap, as it is.
    CHECK(Near(gdx_net_race_progress(500.0f, 2, length, 1490.0f, 0, &lap), 1500.0f) && lap == 2);
    // The place crossed the line a pose before the lap count did.
    CHECK(Near(gdx_net_race_progress(10.0f, 1, length, 990.0f, 0, &lap), 1010.0f) && lap == 2);
    // The lap count crossed a pose before the place did.
    CHECK(Near(gdx_net_race_progress(990.0f, 2, length, 980.0f, 0, &lap), 990.0f) && lap == 1);
    // Both agree at the line.
    CHECK(Near(gdx_net_race_progress(10.0f, 2, length, 990.0f, 0, &lap), 1010.0f) && lap == 2);
    // Driving back over the line.
    CHECK(Near(gdx_net_race_progress(990.0f, 1, length, 1010.0f, 0, &lap), 990.0f) && lap == 1);
    // After a jump the owner's lap is taken as it is.
    CHECK(Near(gdx_net_race_progress(10.0f, 1, length, 990.0f, 1, &lap), 10.0f) && lap == 1);
    // Away from the line a lap that really changed is followed, not corrected.
    CHECK(Near(gdx_net_race_progress(400.0f, 1, length, 2390.0f, 0, &lap), 400.0f) && lap == 1);
    // No track length, no correction, and no lap pointer is fine.
    CHECK(Near(gdx_net_race_progress(10.0f, 3, 0.0f, 990.0f, 0, nullptr), 10.0f));
}

// The store the game bridge talks to.
void TestStore() {
    const float push[3] = { 0.0f, 0.0f, 2.0f };
    GdxNetPose out = {};
    float ahead = 0.0f;
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    float velocity[3] = { 0.0f, 0.0f, 0.0f };
    float shown[3] = { 0.0f, 0.0f, 0.0f };

    gdx_net_race_reset();
    CHECK(!gdx_net_race_active());
    for (int slot = -2; slot < GDX_NET_RACE_MAX_RACERS + 2; slot++) {
        CHECK(!gdx_net_race_is_puppet(slot));
    }

    gdx_net_race_set_active(7);
    CHECK(gdx_net_race_active() == 1);
    gdx_net_race_set_puppet(1, 1);
    gdx_net_race_set_puppet(29, 1);
    gdx_net_race_set_puppet(30, 1);
    gdx_net_race_set_puppet(-1, 1);
    CHECK(gdx_net_race_is_puppet(1) && gdx_net_race_is_puppet(29));
    CHECK(!gdx_net_race_is_puppet(0) && !gdx_net_race_is_puppet(2) && !gdx_net_race_is_puppet(30));
    gdx_net_race_set_puppet(29, 0);
    CHECK(gdx_net_race_is_puppet(1) && !gdx_net_race_is_puppet(29));

    // Each slot has its own poses, and a bad slot or pointer is ignored.
    for (uint32_t tick = 0; tick <= 20; tick += 2) {
        GdxNetPose pose = OnLine(tick);

        gdx_net_race_push(1, &pose);
        gdx_net_race_push(30, &pose);
        gdx_net_race_push(1, nullptr);
    }
    CHECK(gdx_net_race_sample(1, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_POSE);
    CHECK(out.tick == 20 && Near(ahead, 4.0f, 0.07f));
    CHECK(gdx_net_race_sample(2, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(gdx_net_race_sample(30, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(gdx_net_race_sample(1, 24, StampOf(24), nullptr, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(gdx_net_race_sample(1, 24, StampOf(24), &out, nullptr) == GDX_NET_SAMPLE_NONE);

    // The pose guessed ahead: four ticks on, where our own tick 24 would have it.
    gdx_net_race_advance(&out, ahead, nullptr, nullptr);
    CHECK(Near(out.pos[0], 240.0f, 0.7f));
    gdx_net_race_advance(nullptr, 1.0f, nullptr, nullptr);

    // What we pushed it, and where it is shown.
    gdx_net_race_shove(1, 24, push);
    gdx_net_race_shove(30, 24, push);
    gdx_net_race_shove(1, 24, nullptr);
    gdx_net_race_shoved(1, 24, offset, velocity);
    CHECK(velocity[2] == 2.0f && offset[2] == 2.0f);
    gdx_net_race_shoved(30, 24, offset, velocity);
    CHECK(velocity[2] == 0.0f && offset[2] == 0.0f);
    offset[2] = velocity[2] = 9.0f;
    gdx_net_race_shoved(2, 24, offset, velocity); // no pose, no push
    CHECK(velocity[2] == 0.0f && offset[2] == 0.0f);
    gdx_net_race_shoved(1, 24, nullptr, velocity);
    gdx_net_race_shoved(1, 24, offset, nullptr);
    CHECK(gdx_net_race_settle(1, 24, out.pos, out.vel, shown) == 1 && shown[0] == out.pos[0]);
    CHECK(gdx_net_race_settle(30, 24, out.pos, out.vel, shown) == 0);
    CHECK(gdx_net_race_settle(1, 25, nullptr, out.vel, shown) == 0 && gdx_net_race_settle(1, 25, out.pos, nullptr, shown) == 0 &&
          gdx_net_race_settle(1, 25, out.pos, out.vel, nullptr) == 0);
    out.pos[0] += kSpeed + 4.0f; // a tick on, and four units more than its velocity says
    CHECK(gdx_net_race_settle(1, 25, out.pos, out.vel, shown) == 0 && Near(shown[0], out.pos[0] - 3.0f, 0.001f));
    CHECK(Stats().snaps == 1 && Stats().held == 0 && Stats().steps == 0);

    // A new race forgets all of it.
    gdx_net_race_reset();
    CHECK(!gdx_net_race_active() && !gdx_net_race_is_puppet(1));
    CHECK(gdx_net_race_sample(1, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(gdx_net_race_settle(1, 26, out.pos, out.vel, shown) == 1);
    gdx_net_race_reset();
}

// The one-copy developer check: our own poses come back to a slot, late, as another player's would. Late
// as they are, they say when they were taken, so the machine guessed from them rides where we are.
void TestEcho() {
    const uint32_t delay = 30;
    GdxNetPose out = {};
    float ahead = 0.0f;
    int odd = 0;
    int offLine = 0;

    gdx_net_race_reset();

    // With no echo set our poses go nowhere.
    for (uint32_t tick = 1; tick <= 10; tick++) {
        GdxNetPose pose = OnLine(tick);

        gdx_net_race_local_pose(&pose, tick);
    }
    CHECK(gdx_net_race_sample(1, 10, StampOf(10), &out, &ahead) == GDX_NET_SAMPLE_NONE);

    gdx_net_race_echo(1, delay);
    for (uint32_t tick = 1; tick <= 200; tick++) {
        GdxNetPose pose = OnLine(tick);
        int result;

        gdx_net_race_local_pose(&pose, tick);
        result = gdx_net_race_sample(1, tick, StampOf(tick), &out, &ahead);
        if (tick <= delay + 1) {
            CHECK(result == GDX_NET_SAMPLE_NONE); // the first pose sent is tick 2
            continue;
        }
        // The newest pose is `delay` ticks old, or one more.
        if (result != GDX_NET_SAMPLE_POSE || out.tick != ((tick - delay) & ~1u) ||
            !Near(ahead, static_cast<float>(tick - out.tick), 0.1f)) {
            odd++;
        }
        gdx_net_race_advance(&out, ahead, nullptr, nullptr);
        if (!Near(out.pos[0], kSpeed * static_cast<float>(tick), 0.8f)) {
            offLine++;
        }
    }
    CHECK(odd == 0 && offLine == 0);
    CHECK(Stats().held == 0 && Stats().steps == 0);
    CHECK(gdx_net_race_sample(2, 200, StampOf(200), &out, &ahead) == GDX_NET_SAMPLE_NONE);

    // Turned off, nothing more arrives: the slot is guessed on for a while and then stands.
    gdx_net_race_echo(1, 0);
    for (uint32_t tick = 201; tick <= 300; tick++) {
        GdxNetPose pose = OnLine(tick);

        gdx_net_race_local_pose(&pose, tick);
    }
    CHECK(gdx_net_race_sample(1, 300, StampOf(300), &out, &ahead) == (GDX_NET_SAMPLE_POSE | GDX_NET_SAMPLE_HELD));
    CHECK(out.tick == 170 && ahead == kAheadMostTicks);

    // A bad slot turns it off too, and a missing pose is not an error.
    gdx_net_race_reset();
    gdx_net_race_echo(30, delay);
    gdx_net_race_local_pose(nullptr, 1);
    gdx_net_race_echo(1, delay);
    gdx_net_race_local_pose(nullptr, 1);
    CHECK(gdx_net_race_sample(1, 100, StampOf(100), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    gdx_net_race_reset();
}

// ---- Poses between copies (slice S4) ----

// The two boxes the game thread and the network thread pass poses through.
void TestPoseBoxes() {
    std::printf("poses: the boxes between the game and the network\n");
    static GdxNetSlotPose taken[GDX_NET_RACE_OUT_CAPACITY + 8];
    unsigned int race = 99;
    GdxNetPose pose = OnLine(4);
    GdxNetPose odd = OnLine(5);
    GdxNetPose out = {};
    float ahead = 0.0f;
    float shown[3] = { 0.0f, 0.0f, 0.0f };

    // No room's race: nothing is queued, taken or let in.
    gdx_net_race_reset();
    CHECK(!gdx_net_race_send_due(4));
    gdx_net_race_local_pose(&pose, 4);
    gdx_net_race_send_pose(5, &pose);
    CHECK(gdx_net_race_take_outgoing(&race, taken, 8) == 0 && race == 99);
    gdx_net_race_set_puppet(5, 1);
    gdx_net_race_deliver(0, 5, &pose);
    gdx_net_race_receive();
    CHECK(gdx_net_race_sample(5, 10, StampOf(10), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(Stats().sent == 0 && Stats().received == 0 && Stats().dropped == 0);

    // Race 7, and we are the third player on its grid. A pose leaves on every second tick only.
    gdx_net_race_reset();
    gdx_net_race_link(7, 2);
    CHECK(gdx_net_race_send_due(4) && !gdx_net_race_send_due(5));
    gdx_net_race_local_pose(&pose, 4); // our own machine, under our race slot
    gdx_net_race_local_pose(&odd, 5);
    gdx_net_race_local_pose(nullptr, 6);
    gdx_net_race_send_pose(9, &pose); // a machine this copy drives for the others
    gdx_net_race_send_pose(9, &odd);
    gdx_net_race_send_pose(30, &pose);
    gdx_net_race_send_pose(-1, &pose);
    gdx_net_race_send_pose(9, nullptr);
    CHECK(Stats().sent == 2);
    CHECK(gdx_net_race_take_outgoing(nullptr, taken, 8) == 0 && gdx_net_race_take_outgoing(&race, nullptr, 8) == 0 &&
          gdx_net_race_take_outgoing(&race, taken, 0) == 0);
    CHECK(gdx_net_race_take_outgoing(&race, taken, 1) == 1);
    CHECK(race == 7 && taken[0].slot == 2 && taken[0].pose.tick == 4 && Near(taken[0].pose.pos[0], 40.0f));
    CHECK(gdx_net_race_take_outgoing(&race, taken, 8) == 1 && taken[0].slot == 9 && taken[0].pose.tick == 4);
    CHECK(gdx_net_race_take_outgoing(&race, taken, 8) == 0);

    // A network thread that stopped taking: the box keeps the newest poses and counts the ones it let go.
    for (uint32_t i = 0; i < GDX_NET_RACE_OUT_CAPACITY + 3; i++) {
        GdxNetPose next = OnLine(2 * i);

        gdx_net_race_send_pose(3, &next);
    }
    CHECK(gdx_net_race_take_outgoing(&race, taken, GDX_NET_RACE_OUT_CAPACITY + 8) == GDX_NET_RACE_OUT_CAPACITY);
    CHECK(taken[0].pose.tick == 6 && taken[GDX_NET_RACE_OUT_CAPACITY - 1].pose.tick == 2 * (GDX_NET_RACE_OUT_CAPACITY + 2));
    bool ordered = true;
    for (int i = 1; i < GDX_NET_RACE_OUT_CAPACITY; i++) {
        ordered = ordered && taken[i].pose.tick == taken[i - 1].pose.tick + 2 && taken[i].slot == 3;
    }
    CHECK(ordered);
    CHECK(Stats().sent == 2 + GDX_NET_RACE_OUT_CAPACITY + 3 && Stats().dropped == 3);

    // What the room sent. Race slot 0 is the host, which this copy keeps in its own slot 2; race slot 2 is
    // our own machine; local slot 6 is no remote racer.
    gdx_net_race_set_puppet(2, 1);
    gdx_net_race_set_puppet(5, 1);
    for (uint32_t tick = 0; tick <= 20; tick += 2) {
        GdxNetPose next = OnLine(tick);

        gdx_net_race_deliver(7, 0, &next);
        gdx_net_race_deliver(7, 5, &next);
        gdx_net_race_deliver(7, 2, &next);
        gdx_net_race_deliver(7, 6, &next);
        gdx_net_race_deliver(8, 5, &next); // another race
        gdx_net_race_deliver(7, 30, &next);
        gdx_net_race_deliver(7, 5, nullptr);
    }
    CHECK(gdx_net_race_sample(2, 20, StampOf(20), &out, &ahead) == GDX_NET_SAMPLE_NONE); // the game takes them in on its tick
    gdx_net_race_receive();
    CHECK(Stats().received == 22);
    CHECK(gdx_net_race_sample(2, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_POSE && out.tick == 20 &&
          Near(out.pos[0], 200.0f) && Near(ahead, 4.0f, 0.07f));
    CHECK(gdx_net_race_sample(5, 22, StampOf(22), &out, &ahead) == GDX_NET_SAMPLE_POSE && out.tick == 20 &&
          Near(ahead, 2.0f, 0.07f));
    CHECK(gdx_net_race_sample(6, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    CHECK(gdx_net_race_sample(0, 24, StampOf(24), &out, &ahead) == GDX_NET_SAMPLE_NONE);
    gdx_net_race_receive();
    CHECK(Stats().received == 22);

    // A game that stopped ticking: the same rule on the way in.
    for (uint32_t i = 0; i < GDX_NET_RACE_IN_CAPACITY + 2; i++) {
        GdxNetPose next = OnLine(100 + 2 * i);

        gdx_net_race_deliver(7, 5, &next);
    }
    CHECK(Stats().dropped == 5);
    gdx_net_race_receive();
    CHECK(Stats().received == 22 + GDX_NET_RACE_IN_CAPACITY);

    // The next race starts with empty boxes. As the host our slot is 0.
    gdx_net_race_send_pose(3, &pose);
    gdx_net_race_deliver(7, 5, &pose);
    gdx_net_race_link(8, 0);
    CHECK(gdx_net_race_take_outgoing(&race, taken, 8) == 0);
    gdx_net_race_deliver(7, 5, &pose); // the race before, still on its way
    gdx_net_race_receive();
    CHECK(Stats().received == 22 + GDX_NET_RACE_IN_CAPACITY);
    gdx_net_race_local_pose(&pose, 4);
    CHECK(gdx_net_race_take_outgoing(&race, taken, 8) == 1 && race == 8 && taken[0].slot == 0);

    // A slot that is none, and the end of the race.
    gdx_net_race_link(9, 30);
    CHECK(!gdx_net_race_send_due(4));
    gdx_net_race_link(9, -1);
    gdx_net_race_send_pose(3, &pose);
    CHECK(!gdx_net_race_send_due(4) && gdx_net_race_take_outgoing(&race, taken, 8) == 0);
    gdx_net_race_link(9, 1);
    gdx_net_race_send_pose(3, &pose);
    gdx_net_race_reset();
    CHECK(!gdx_net_race_send_due(4) && gdx_net_race_take_outgoing(&race, taken, 8) == 0);
    CHECK(Stats().sent == 0 && Stats().received == 0 && Stats().dropped == 0);
    gdx_net_race_stats(nullptr);

    // The three counts a race log ends with: samples that stood still, jumps, and skipped ticks.
    gdx_net_race_push(1, &pose);
    CHECK(gdx_net_race_sample(1, 4, StampOf(4), &out, &ahead) == GDX_NET_SAMPLE_POSE);
    CHECK(gdx_net_race_settle(1, 4, out.pos, out.vel, shown) == 1);
    CHECK(gdx_net_race_sample(1, 50, StampOf(50), &out, &ahead) == (GDX_NET_SAMPLE_POSE | GDX_NET_SAMPLE_HELD));
    CHECK(Stats().held == 1 && Stats().snaps == 1 && Stats().steps == 0);
    GdxNetPose skipped = OnLine(60);
    skipped.timeMs = StampOf(61); // its owner's game lost a frame: tick 60 ran a tick late
    gdx_net_race_push(1, &skipped);
    CHECK(gdx_net_race_sample(1, 62, StampOf(62), &out, &ahead) == (GDX_NET_SAMPLE_POSE | GDX_NET_SAMPLE_STEP));
    CHECK(Near(ahead, 1.0f, 0.07f));
    CHECK(Stats().held == 1 && Stats().snaps == 1 && Stats().steps == 1);
    gdx_net_race_reset();
}


// ---- The start of an online race (slice S3) ----

bool SameGrid(const GdxNetGrid& a, const GdxNetGrid& b) {
    if (a.racers != b.racers || a.humans != b.humans) {
        return false;
    }
    for (int i = 0; i < a.racers; i++) {
        if (a.character[i] != b.character[i] || a.skin[i] != b.skin[i]) {
            return false;
        }
    }
    return true;
}

std::set<int> Machines(const GdxNetGrid& grid, int from) {
    std::set<int> machines;
    for (int i = from; i < grid.racers; i++) {
        machines.insert(grid.character[i]);
    }
    return machines;
}

void TestGridBuilder() {
    std::printf("grid: the machines of the field\n");
    const unsigned char machine[3] = { 5, 5, 12 };
    const unsigned char skin[3] = { 1, 2, 3 };
    GdxNetGrid grid;
    gdx_net_race_build_grid(0x1234, machine, skin, 3, 27, &grid);
    CHECK(grid.racers == 30 && grid.humans == 3);
    CHECK(grid.character[0] == 5 && grid.character[1] == 5 && grid.character[2] == 12);
    CHECK(grid.skin[0] == 1 && grid.skin[1] == 2 && grid.skin[2] == 3);
    for (int i = 3; i < 30; i++) {
        CHECK(grid.character[i] < 30 && grid.character[i] != 5 && grid.character[i] != 12 && grid.skin[i] == 0);
    }
    CHECK(Machines(grid, 3).size() == 27); // 28 machines are free: no CPU shares one

    // The same seed deals the same field on every copy; another seed deals another.
    GdxNetGrid again;
    gdx_net_race_build_grid(0x1234, machine, skin, 3, 27, &again);
    CHECK(SameGrid(grid, again));
    GdxNetGrid other;
    gdx_net_race_build_grid(0x1235, machine, skin, 3, 27, &other);
    CHECK(!SameGrid(grid, other) && other.racers == 30 && Machines(other, 3).size() == 27);
    gdx_net_race_build_grid(0, machine, skin, 3, 27, &other); // a seed of 0 still shuffles
    CHECK(other.racers == 30 && Machines(other, 3).size() == 27);
    int moved = 0;
    for (int i = 3; i + 1 < 30; i++) {
        moved += other.character[i] > other.character[i + 1] ? 1 : 0;
    }
    CHECK(moved > 0);

    // A full room: twelve players and eighteen CPUs put every machine on the grid once.
    unsigned char twelve[12];
    unsigned char plain[12] = {};
    for (int i = 0; i < 12; i++) {
        twelve[i] = static_cast<unsigned char>(29 - 2 * i);
    }
    gdx_net_race_build_grid(77, twelve, plain, 12, 18, &grid);
    CHECK(grid.racers == 30 && grid.humans == 12 && Machines(grid, 0).size() == 30);
    // One player and no CPU at all.
    gdx_net_race_build_grid(77, twelve, plain, 1, 0, &grid);
    CHECK(grid.racers == 1 && grid.humans == 1 && grid.character[0] == 29);
    // A machine outside the roster becomes the first one.
    const unsigned char wild[1] = { 200 };
    gdx_net_race_build_grid(77, wild, plain, 1, 4, &grid);
    CHECK(grid.racers == 5 && grid.character[0] == 0 && Machines(grid, 1).count(0) == 0);

    // Counts that make no race give no grid.
    gdx_net_race_build_grid(1, machine, skin, 0, 5, &grid);
    CHECK(grid.racers == 0 && grid.humans == 0);
    gdx_net_race_build_grid(1, machine, skin, 3, -1, &grid);
    CHECK(grid.racers == 0);
    gdx_net_race_build_grid(1, machine, skin, 3, 28, &grid);
    CHECK(grid.racers == 0);
    gdx_net_race_build_grid(1, nullptr, skin, 3, 27, &grid);
    CHECK(grid.racers == 0);
    gdx_net_race_build_grid(1, machine, nullptr, 3, 27, &grid);
    CHECK(grid.racers == 0);
    gdx_net_race_build_grid(1, machine, skin, 3, 27, nullptr);
}

void TestSlots() {
    std::printf("grid: slots and places\n");
    for (int self = -1; self < 12; self++) {
        std::set<int> seen;
        for (int slot = 0; slot < 30; slot++) {
            const int local = gdx_net_race_local_slot(self, slot);
            seen.insert(local);
            CHECK(gdx_net_race_local_slot(self, local) == slot); // its own inverse
            if (self <= 0) {
                CHECK(local == slot);
            }
        }
        CHECK(seen.size() == 30);
        if (self > 0) {
            CHECK(gdx_net_race_local_slot(self, self) == 0 && gdx_net_race_local_slot(self, 0) == self);
            CHECK(gdx_net_race_local_slot(self, 29) == 29);
        }
    }

    // Three players and 27 CPUs: the CPUs ahead, the players behind them in grid order.
    CHECK(gdx_net_race_grid_place(3, 30, 3) == 1 && gdx_net_race_grid_place(3, 30, 29) == 27);
    CHECK(gdx_net_race_grid_place(3, 30, 0) == 28 && gdx_net_race_grid_place(3, 30, 1) == 29 &&
          gdx_net_race_grid_place(3, 30, 2) == 30);
    std::set<int> places;
    for (int slot = 0; slot < 30; slot++) {
        places.insert(gdx_net_race_grid_place(3, 30, slot));
    }
    CHECK(places.size() == 30 && *places.begin() == 1 && *places.rbegin() == 30);
    CHECK(gdx_net_race_grid_place(2, 2, 0) == 1 && gdx_net_race_grid_place(2, 2, 1) == 2);
    CHECK(gdx_net_race_grid_place(12, 30, 11) == 30 && gdx_net_race_grid_place(12, 30, 12) == 1);
}

void TestIntroClock() {
    std::printf("intro: the timer the host clock asks for\n");
    CHECK(gdx_net_race_intro_target(-1) == 460 && gdx_net_race_intro_target(-5000) == 460);
    CHECK(gdx_net_race_intro_target(0) == 459 && gdx_net_race_intro_target(16) == 459);
    CHECK(gdx_net_race_intro_target(17) == 458 && gdx_net_race_intro_target(1000) == 399);
    CHECK(gdx_net_race_intro_target(6983) == 41 && gdx_net_race_intro_target(6984) == 40);
    CHECK(gdx_net_race_intro_target(7000) == 40 && gdx_net_race_intro_target(2000000000) == 40);

    CHECK(gdx_net_race_intro_run(460, -1) == 0 && gdx_net_race_intro_run(300, -200) == 0); // no start yet
    CHECK(gdx_net_race_intro_run(40, -1) == 1 && gdx_net_race_intro_run(12, -1) == 1); // from GO on, freely
    CHECK(gdx_net_race_intro_run(460, 0) == 1);                                        // the first tick
    CHECK(gdx_net_race_intro_run(459, 0) == 0 && gdx_net_race_intro_run(459, 16) == 0); // ahead: wait
    CHECK(gdx_net_race_intro_run(459, 17) == 1);
    CHECK(gdx_net_race_intro_run(221, 0) == 0);
    CHECK(gdx_net_race_intro_run(220, 0) == 1 && gdx_net_race_intro_run(160, 0) == 1 &&
          gdx_net_race_intro_run(100, 0) == 1); // a beep is never held for a second frame
    CHECK(gdx_net_race_intro_run(41, 6983) == 0 && gdx_net_race_intro_run(41, 6984) == 1);

    CHECK(gdx_net_race_intro_catch_up(459, -1) == 459);
    CHECK(gdx_net_race_intro_catch_up(40, 99999) == 40 && gdx_net_race_intro_catch_up(30, 99999) == 30);
    CHECK(gdx_net_race_intro_catch_up(459, 0) == 459 && gdx_net_race_intro_catch_up(459, 16) == 459); // on time
    CHECK(gdx_net_race_intro_catch_up(300, 0) == 300);    // ahead: the timer never goes back up
    CHECK(gdx_net_race_intro_catch_up(459, 1000) == 399); // a second behind
    CHECK(gdx_net_race_intro_catch_up(459, 5000) == 220); // five behind: as far as the first beep
    CHECK(gdx_net_race_intro_catch_up(221, 4000) == 220);
    CHECK(gdx_net_race_intro_catch_up(220, 5000) == 220); // the beep itself is shown before anything is skipped
    CHECK(gdx_net_race_intro_catch_up(219, 5000) == 160);
    CHECK(gdx_net_race_intro_catch_up(159, 5000) == 159);
    CHECK(gdx_net_race_intro_catch_up(219, 99999) == 160 && gdx_net_race_intro_catch_up(159, 99999) == 100);
    CHECK(gdx_net_race_intro_catch_up(101, 99999) == 100 && gdx_net_race_intro_catch_up(99, 99999) == 40);
}

// One copy of the game running the race intro the way Racer_Update does, on the host clock in
// microseconds since the room's start time. It ticks every periodUs from firstUs on, believes the host
// clock reads errorMs more than it does, and can stall once, when its timer holds stallAt.
struct IntroRun {
    long long goUs = -1; // the host time of the tick that reached GO
    int goFrames = 0;    // frames the GO value was on screen
    int beeps[3] = { 0, 0, 0 };
    bool wentUp = false;
    bool finished = false;
};

IntroRun RunIntro(long long firstUs, long long periodUs, int errorMs = 0, int stallAt = -1, long long stallUs = 0) {
    static const int kBeeps[3] = { 220, 160, 100 };
    IntroRun r;
    int timer = GDX_NET_INTRO_START;
    long long t = firstUs;
    bool stalled = false;
    for (int guard = 0; guard < 200000 && timer > 0; guard++, t += periodUs) {
        if (!stalled && timer == stallAt) {
            stalled = true;
            t += stallUs;
        }
        const long long ms = t >= 0 ? t / 1000 : -((-t + 999) / 1000);
        const int elapsed = static_cast<int>(ms) + errorMs;
        const int before = timer;
        if (gdx_net_race_intro_run(timer, elapsed)) {
            timer--;
            timer = gdx_net_race_intro_catch_up(timer, elapsed);
            if (timer == GDX_NET_INTRO_GO) {
                r.goUs = t;
            }
        }
        r.wentUp = r.wentUp || timer > before;
        // What this frame shows.
        for (int i = 0; i < 3; i++) {
            r.beeps[i] += timer == kBeeps[i] ? 1 : 0;
        }
        r.goFrames += timer == GDX_NET_INTRO_GO ? 1 : 0;
    }
    r.finished = timer == 0;
    return r;
}

bool Clean(const IntroRun& r) {
    return r.finished && !r.wentUp && r.goFrames == 1 && r.beeps[0] == 1 && r.beeps[1] == 1 && r.beeps[2] == 1;
}

// GO is due on the first tick at or after this much of the host clock.
constexpr long long kGoDueUs = 6984000;
constexpr long long kTickUs = 16667;

// True when the copy reached GO on its first tick at or after the moment it believes GO is due.
bool OnTheBeat(const IntroRun& r, long long periodUs, int errorMs = 0) {
    const long long due = kGoDueUs - 1000LL * errorMs;
    return r.goUs >= due && r.goUs < due + periodUs + 1000;
}

void TestIntroTogether() {
    std::printf("intro: every copy reaches GO together\n");

    // Copies that were waiting on the grid, their ticks out of step by every part of a tick.
    long long first = -1;
    long long last = -1;
    for (long long phase = 0; phase < kTickUs; phase += 500) {
        const IntroRun r = RunIntro(-300000 + phase, kTickUs);
        CHECK(Clean(r) && OnTheBeat(r, kTickUs));
        first = first < 0 || r.goUs < first ? r.goUs : first;
        last = r.goUs > last ? r.goUs : last;
    }
    CHECK(last - first <= kTickUs + 1000); // within one tick of each other

    // A copy that loaded late: it joins the countdown where the others are, hearing every beep it is
    // still in time for exactly once.
    const long long late[] = { 1000, 300000, 1500000, 3990000, 4010000, 4100000, 5990000, 6500000, 6900000 };
    for (long long lateUs : late) {
        for (long long phase = 0; phase < kTickUs; phase += 3300) {
            const IntroRun r = RunIntro(lateUs + phase, kTickUs);
            CHECK(Clean(r) && OnTheBeat(r, kTickUs));
        }
    }

    // Later than GO itself: the three beeps and GO on four ticks in a row, never one skipped.
    const IntroRun tooLate = RunIntro(8000000, kTickUs);
    CHECK(Clean(tooLate) && tooLate.goUs == 8000000 + 3 * kTickUs);

    // A screen that is not 60 Hz: a faster copy waits for the clock, a slower one keeps up with it.
    const long long periods[] = { 16000, 16393, 17000, 20000, 33333 };
    for (long long period : periods) {
        for (long long phase = 0; phase < period; phase += 2100) {
            const IntroRun r = RunIntro(-200000 + phase, period);
            CHECK(Clean(r) && OnTheBeat(r, period));
        }
    }

    // A stall in the middle of the countdown, and one that ends past a beep.
    const IntroRun hitch = RunIntro(-100000, kTickUs, 0, 300, 250000);
    CHECK(Clean(hitch) && OnTheBeat(hitch, kTickUs));
    const IntroRun overBeep = RunIntro(-100000, kTickUs, 0, 222, 2000000);
    CHECK(Clean(overBeep) && OnTheBeat(overBeep, kTickUs));
    const IntroRun overAll = RunIntro(-100000, kTickUs, 0, 230, 9000000);
    CHECK(Clean(overAll) && overAll.goUs > kGoDueUs);

    // A copy whose idea of the host clock is off by some milliseconds is off by that much, no more.
    const int errors[] = { -12, -5, 5, 12 };
    for (int error : errors) {
        const IntroRun r = RunIntro(-300000, kTickUs, error);
        CHECK(Clean(r) && OnTheBeat(r, kTickUs, error));
    }
}

void TestRules() {
    std::printf("rules: the host's values while a room's race runs\n");
    gdx_net_race_reset();
    CHECK(!gdx_net_race_online());
    CHECK(gdx_net_race_rule_ignore_track_settings(0) == 0 && gdx_net_race_rule_ignore_track_settings(1) == 1);
    CHECK(gdx_net_race_rule_lava_no_kill(0) == 0 && gdx_net_race_rule_lava_no_kill(1) == 1);
    CHECK(gdx_net_race_rule_magnet_range(80.0f) == 80.0f && gdx_net_race_rule_magnet_strength(1.7f) == 1.7f);

    GdxNetRules rules = {};
    rules.laps = 5;
    rules.ignoreTrackSettings = 1;
    rules.lavaNoKill = 0;
    rules.magnetRange = 120.0f;
    rules.magnetStrength = 2.5f;
    const auto hosts = [] {
        return gdx_net_race_rule_ignore_track_settings(0) == 1 && gdx_net_race_rule_lava_no_kill(1) == 0 &&
               gdx_net_race_rule_magnet_range(80.0f) == 120.0f && gdx_net_race_rule_magnet_strength(1.7f) == 2.5f;
    };
    const auto own = [] {
        return gdx_net_race_rule_ignore_track_settings(0) == 0 && gdx_net_race_rule_lava_no_kill(1) == 1 &&
               gdx_net_race_rule_magnet_range(80.0f) == 80.0f && gdx_net_race_rule_magnet_strength(1.7f) == 1.7f;
    };
    gdx_net_race_set_rules(&rules);
    CHECK(hosts() && !own());
    gdx_net_race_set_rules(nullptr);
    CHECK(own());

    // The race ends: the player has their own values back, whichever way it ended.
    gdx_net_race_set_rules(&rules);
    gdx_net_race_set_online(1);
    CHECK(gdx_net_race_online() && hosts());
    gdx_net_race_set_online(0);
    CHECK(!gdx_net_race_online() && own());
    gdx_net_race_set_rules(&rules);
    gdx_net_race_set_online(1);
    gdx_net_race_reset();
    CHECK(!gdx_net_race_online() && own());

    // Remote racers alone do not make a room's race: the developer echo has them too.
    gdx_net_race_set_active(1);
    CHECK(gdx_net_race_active() && !gdx_net_race_online() && own());
    gdx_net_race_reset();
}

void TestDigest() {
    std::printf("digest: SHA-256 of what is fed\n");
    static const unsigned char kAbc[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                            0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                            0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    static const unsigned char kEmpty[32] = { 0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                              0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                              0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    const auto same = [](const unsigned char* a, const unsigned char* b) {
        for (int i = 0; i < 32; i++) {
            if (a[i] != b[i]) {
                return false;
            }
        }
        return true;
    };
    unsigned char out[32];
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed("abc", 3);
    gdx_net_race_digest_end(out);
    CHECK(same(out, kAbc));

    // Fed in pieces it is the same value, and a begin forgets what came before.
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed("zzz", 3);
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed("a", 1);
    gdx_net_race_digest_feed(nullptr, 8);
    gdx_net_race_digest_feed("x", 0);
    gdx_net_race_digest_feed("bc", 2);
    gdx_net_race_digest_end(out);
    CHECK(same(out, kAbc));

    gdx_net_race_digest_begin();
    gdx_net_race_digest_end(out);
    CHECK(same(out, kEmpty));

    // One control point a unit off is another track.
    float points[6] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
    unsigned char first[32];
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed(points, sizeof(points));
    gdx_net_race_digest_end(first);
    points[4] += 1.0f;
    gdx_net_race_digest_begin();
    gdx_net_race_digest_feed(points, sizeof(points));
    gdx_net_race_digest_end(out);
    CHECK(!same(out, first));
}

} // namespace

int main() {
    TestEmpty();
    TestSteady();
    TestTripChange();
    TestJitter();
    TestSlowOwner();
    TestSkippedTick();
    TestLossAndHold();
    TestLateAndDuplicate();
    TestRoad();
    TestSettle();
    TestShoves();
    TestDelayLine();
    TestProgress();
    TestStore();
    TestEcho();
    TestPoseBoxes();
    TestGridBuilder();
    TestSlots();
    TestIntroClock();
    TestIntroTogether();
    TestRules();
    TestDigest();
    std::printf("%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
