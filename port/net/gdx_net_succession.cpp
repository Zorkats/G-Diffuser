// port/net/gdx_net_succession.cpp: successor choice and the split-brain rules (G-NET D28.2).
// Copyright (c) Zorkats

#include "gdx_net_succession.h"

namespace gdx::net::succession {
namespace {

// Lower ranks go first: racers before watchers (decision 19), and within each tier a reachable member before an
// unreachable one. Reachability counts only in a directory room, where the room knows who can take inbound
// connections; elsewhere it would only reorder people by a fact the room has not checked.
int Rank(const Member& member, bool directoryRoom) {
    const int tier = member.racer ? 0 : 2;
    const bool reachable = directoryRoom && member.directReachable;
    return tier + (reachable ? 0 : 1);
}

}  // namespace

std::optional<uint8_t> Successor(const std::vector<Member>& roster, bool directoryRoom) {
    const Member* best = nullptr;
    int bestRank = 0;
    for (const Member& member : roster) {
        if (member.host || !member.present) {
            continue;
        }
        const int rank = Rank(member, directoryRoom);
        // Strictly lower only: on a tie the member earlier in the roster, who joined first, keeps the place.
        if (best == nullptr || rank < bestRank) {
            best = &member;
            bestRank = rank;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return best->id;
}

Step SplitBrain(const SplitBrainInput& in) {
    if (in.promoted) {
        // Only a rejoin window that ran out with nobody rejoining closes the room. Once a rejoin has come in, the
        // room stays open, and nothing the old host says demotes this copy.
        if (in.rejoinsReceived == 0 && in.msSincePromotion >= kRejoinWaitMs) {
            return Step::Close;
        }
        return Step::Wait;
    }
    // A Handover is the old host's own decision, so it does not wait for silence and is not second-guessed by
    // the evidence check that guards the silence path.
    if (in.handoverReceived) {
        return in.isSuccessor ? Step::Promote : Step::Wait;
    }
    if (in.msSinceHostPacket < kHostSilenceMs) {
        return Step::Wait;
    }
    // Silence on this link alone does not prove the host is gone. A successor whose own link broke while the host
    // still runs the room would otherwise open a second room under the same id.
    if (in.stillHearsOldHost) {
        return Step::IgnoreSuccessor;
    }
    return in.isSuccessor ? Step::Promote : Step::Wait;
}

bool NoSuccessorClose(uint32_t msSinceHostPacket, bool successorAnswered) {
    return !successorAnswered && msSinceHostPacket >= kHostSilenceMs + kSuccessorWaitMs;
}

}  // namespace gdx::net::succession
