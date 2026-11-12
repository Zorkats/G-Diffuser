// port/tests/gdx_net_succession_tests.cpp: successor choice in every tier and tie order, and the split-brain and
// no-successor boundaries (G-NET D28.2).
// Copyright (c) Zorkats
//
// Usage: gdx_net_succession_tests
// Prints one line per failed check, then "<N> checks, <M> failed". Exit 0 only when M is 0.

#include "gdx_net_succession.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using gdx::net::succession::Member;
using gdx::net::succession::NoSuccessorClose;
using gdx::net::succession::SplitBrain;
using gdx::net::succession::SplitBrainInput;
using gdx::net::succession::Step;
using gdx::net::succession::Successor;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

Member Host(uint8_t id) {
    Member member;
    member.id = id;
    member.host = true;
    member.racer = true;
    return member;
}

Member Racer(uint8_t id, bool reachable = false, bool present = true) {
    Member member;
    member.id = id;
    member.racer = true;
    member.directReachable = reachable;
    member.present = present;
    return member;
}

Member Watcher(uint8_t id, bool reachable = false, bool present = true) {
    Member member = Racer(id, reachable, present);
    member.racer = false;
    return member;
}

// The id Successor names, or kNone for no successor, so one expectation covers both answers.
constexpr int kNone = -1;

int Pick(const std::vector<Member>& roster, bool directoryRoom) {
    const std::optional<uint8_t> id = Successor(roster, directoryRoom);
    return id ? static_cast<int>(*id) : kNone;
}

void CheckPick(const std::vector<Member>& roster, bool directoryRoom, int expect, const std::string& what) {
    const int got = Pick(roster, directoryRoom);
    Check(got == expect, what + ": expected " + std::to_string(expect) + ", got " + std::to_string(got));
}

const char* StepName(Step step) {
    switch (step) {
    case Step::Wait:
        return "Wait";
    case Step::Promote:
        return "Promote";
    case Step::IgnoreSuccessor:
        return "IgnoreSuccessor";
    case Step::Close:
        return "Close";
    }
    return "?";
}

void CheckStep(const SplitBrainInput& in, Step expect, const std::string& what) {
    const Step got = SplitBrain(in);
    Check(got == expect, what + ": expected " + StepName(expect) + ", got " + StepName(got));
}

SplitBrainInput Silent(uint32_t silenceMs, bool successor) {
    SplitBrainInput in;
    in.msSinceHostPacket = silenceMs;
    in.isSuccessor = successor;
    return in;
}

void CheckSuccessor() {
    // Racers before watchers, whatever the join order.
    CheckPick({Host(0), Watcher(1), Racer(2)}, false, 2, "a racer beats an earlier watcher");
    // Among racers the first to join wins.
    CheckPick({Host(0), Racer(1), Racer(2), Watcher(3)}, false, 1, "the first racer in join order");
    // The host never succeeds itself, whatever its seat.
    CheckPick({Host(0), Watcher(1)}, false, 1, "the host is excluded: the watcher behind it wins");
    CheckPick({Host(0), Racer(1)}, false, 1, "the host is excluded: the racer behind it wins");
    CheckPick({Host(0)}, false, kNone, "the host alone has no successor");
    // Join order, not id order, decides.
    CheckPick({Host(0), Racer(7), Racer(2)}, false, 7, "join order decides, not the id");
    // An absent member is skipped in both tiers.
    CheckPick({Host(0), Racer(1, false, false), Racer(2)}, false, 2, "an absent racer is skipped");
    CheckPick({Host(0), Racer(1, false, false), Watcher(2)}, false, 2, "an absent racer is skipped for a watcher");
    CheckPick({Host(0), Watcher(1, false, false), Watcher(2)}, false, 2, "an absent watcher is skipped");
    // Watchers only when no racer is left.
    CheckPick({Host(0), Watcher(1), Watcher(2)}, false, 1, "the first watcher when no racer is left");
    CheckPick({}, false, kNone, "an empty roster has no successor");
    CheckPick({Host(0), Racer(1, false, false), Watcher(2, false, false)}, false, kNone, "everyone absent");
    CheckPick({Host(0), Racer(1, false, false), Watcher(2, false, false)}, true, kNone, "everyone absent, directory");

    // Reachability ranks nothing outside a directory room.
    CheckPick({Host(0), Racer(1, false), Racer(2, true)}, false, 1, "reachability ignored outside a directory room");
    CheckPick({Host(0), Watcher(1, false), Watcher(2, true)}, false, 1, "reachability ignored for watchers outside a directory room");

    // Inside a directory room, the four ranks: reachable racer, unreachable racer, reachable watcher, unreachable watcher.
    CheckPick({Host(0), Racer(1, false), Racer(2, true)}, true, 2, "directory: a reachable racer beats an earlier unreachable one");
    CheckPick({Host(0), Watcher(1, true), Racer(2, false)}, true, 2, "directory: an unreachable racer beats a reachable watcher");
    CheckPick({Host(0), Watcher(1, false), Watcher(2, true)}, true, 2, "directory: a reachable watcher beats an earlier unreachable one");
    CheckPick({Host(0), Racer(1, true), Racer(2, true), Racer(3, false)}, true, 1, "directory: join order among reachable racers");
    CheckPick({Host(0), Racer(1, false), Racer(2, false)}, true, 1, "directory: join order among unreachable racers");

    // Each rank in turn, from a roster in the worst order: removing the chosen member exposes the next rank.
    std::vector<Member> ranked = {Host(0), Watcher(1, false), Watcher(2, true), Racer(3, false), Racer(4, true)};
    CheckPick(ranked, true, 4, "rank 1 (reachable racer) is chosen first");
    ranked[4].present = false;
    CheckPick(ranked, true, 3, "rank 2 (unreachable racer) is chosen next");
    ranked[3].present = false;
    CheckPick(ranked, true, 2, "rank 3 (reachable watcher) is chosen next");
    ranked[2].present = false;
    CheckPick(ranked, true, 1, "rank 4 (unreachable watcher) is chosen last");
    ranked[1].present = false;
    CheckPick(ranked, true, kNone, "nobody is left once every rank is gone");

    // The same roster gives the same answer every time, and a copy of it gives the same answer too.
    const std::vector<Member> roster = {Host(0), Racer(3, false), Racer(5, true), Racer(9, true)};
    const int first = Pick(roster, true);
    Check(first == 5, "directory: the first reachable racer (5) is the successor");
    for (int i = 0; i < 3; i++) {
        Check(Pick(roster, true) == first, "the same roster gives the same answer on every call");
    }
    const std::vector<Member> copy = roster;
    Check(Pick(copy, true) == first, "a copy of the roster gives the same answer");

    // Join order is the tie-break: swapping two equal members swaps the answer.
    CheckPick({Host(0), Racer(1), Racer(2)}, false, 1, "join order: racer 1 joined first");
    CheckPick({Host(0), Racer(2), Racer(1)}, false, 2, "join order: racer 2 joined first");
}

void CheckSplitBrain() {
    // Silence, successor, old host gone: promotion at exactly 3000 ms.
    CheckStep(Silent(0, true), Step::Wait, "successor at 0 ms of silence");
    CheckStep(Silent(2999, true), Step::Wait, "successor at 2999 ms of silence");
    CheckStep(Silent(3000, true), Step::Promote, "successor at 3000 ms of silence");
    CheckStep(Silent(60000, true), Step::Promote, "successor long after the silence");

    // Silence, successor, but the old host is still alive by other evidence: the successor stands down at 3000 ms.
    SplitBrainInput alive = Silent(2999, true);
    alive.stillHearsOldHost = true;
    CheckStep(alive, Step::Wait, "alive host at 2999 ms of silence");
    alive.msSinceHostPacket = 3000;
    CheckStep(alive, Step::IgnoreSuccessor, "alive host at 3000 ms of silence");

    // A Handover is the host's own decision: it promotes a successor at once, with or without evidence.
    SplitBrainInput handover = Silent(0, true);
    handover.handoverReceived = true;
    CheckStep(handover, Step::Promote, "a Handover promotes a successor at once");
    handover.stillHearsOldHost = true;
    CheckStep(handover, Step::Promote, "a Handover promotes even while the old host is heard");

    // A copy the roster does not name as successor never promotes.
    CheckStep(Silent(3000, false), Step::Wait, "a client at 3000 ms with no evidence waits for the successor");
    SplitBrainInput client = Silent(3000, false);
    client.stillHearsOldHost = true;
    CheckStep(client, Step::IgnoreSuccessor, "a client that still hears the host ignores a successor");
    SplitBrainInput clientHandover = Silent(0, false);
    clientHandover.handoverReceived = true;
    CheckStep(clientHandover, Step::Wait, "a Handover addressed to a client does not promote it");

    // Promoted: the 10 s rejoin window, closing at exactly 10000 ms with no rejoin.
    SplitBrainInput promo;
    promo.promoted = true;
    promo.isSuccessor = true;
    promo.msSincePromotion = 9999;
    CheckStep(promo, Step::Wait, "promoted at 9999 ms with no rejoin");
    promo.msSincePromotion = 10000;
    CheckStep(promo, Step::Close, "promoted at 10000 ms with no rejoin");
    promo.msSincePromotion = 60000;
    CheckStep(promo, Step::Close, "promoted long after with no rejoin");
    promo.rejoinsReceived = 1;
    CheckStep(promo, Step::Wait, "a rejoin keeps a promoted room open at 10000 ms");
    promo.msSincePromotion = 9999;
    CheckStep(promo, Step::Wait, "a rejoin keeps a promoted room open at 9999 ms");
    promo.rejoinsReceived = 4;
    promo.msSincePromotion = 600000;
    CheckStep(promo, Step::Wait, "later rejoins keep a promoted room open");

    // Promoted copies are not demoted by the old host, and silence means nothing once promoted.
    SplitBrainInput demoted;
    demoted.promoted = true;
    demoted.isSuccessor = true;
    demoted.msSinceHostPacket = 99999;
    demoted.stillHearsOldHost = true;
    demoted.msSincePromotion = 0;
    CheckStep(demoted, Step::Wait, "a promoted copy is not demoted by the old host");

    // Before promotion, the time since promotion means nothing.
    SplitBrainInput early = Silent(0, true);
    early.msSincePromotion = 20000;
    CheckStep(early, Step::Wait, "time since promotion is ignored before promotion");
}

void CheckNoSuccessorClose() {
    // Ten seconds after noticing the loss at 3000 ms of silence: the room closes at 13000 ms in all.
    Check(!NoSuccessorClose(0, false), "no close at 0 ms of silence");
    Check(!NoSuccessorClose(2999, false), "no close at 2999 ms: the host is not yet lost");
    Check(!NoSuccessorClose(3000, false), "no close at 3000 ms: the host is lost, the wait has just begun");
    Check(!NoSuccessorClose(12999, false), "no close at 12999 ms");
    Check(NoSuccessorClose(13000, false), "close at 13000 ms with no successor");
    Check(NoSuccessorClose(60000, false), "close later with no successor");
    // A successor that answered keeps the room open, whatever the silence from the old host.
    Check(!NoSuccessorClose(13000, true), "an answering successor keeps the room open at 13000 ms");
    Check(!NoSuccessorClose(999999, true), "an answering successor keeps the room open after a long silence");
}

}  // namespace

int main() {
    CheckSuccessor();
    CheckSplitBrain();
    CheckNoSuccessorClose();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
