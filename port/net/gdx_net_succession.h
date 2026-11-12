// port/net/gdx_net_succession.h: who takes a room over when its host is lost, and when a copy stops waiting (G-NET D28.2).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no thread, no game header. Every time is a millisecond argument, so the rules run on a
// fake clock in the tests. The caller builds the roster from its own bookkeeping and acts on the answer.
//
// Decision 19. The successor is the first human racer in join order, host excluded. With no racer left, it is the
// first watcher, who races from the next grid, because the host always races. In a directory room, members that
// reported a port mapping or a global IPv6 address in hello come first within each of those two tiers. Every copy
// computes the same answer from the same roster, so nobody has to vote while the host is gone.
//
// Decision 22. A successor promotes on a Handover, or after kHostSilenceMs of host silence, and never on silence
// while the old host is still alive by other evidence. A promoted successor that gets no rejoin within
// kRejoinWaitMs closes the room. Decision 16: a client waits kSuccessorWaitMs after noticing the loss for a
// successor to answer, then closes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gdx::net::succession {

// The host is lost once its link has been silent this long (kLinkTimeoutMs, gdx_net_lobby_session.h).
constexpr uint32_t kHostSilenceMs = 3000;
// A client waits this long after noticing the loss for a successor to answer (decision 16).
constexpr uint32_t kSuccessorWaitMs = 10000;
// A promoted successor waits this long for a rejoin before it closes the room (decision 22).
constexpr uint32_t kRejoinWaitMs = 10000;

// One roster entry, in join order. The caller fills it from PlayerInfo and its own reachability record.
struct Member {
    uint8_t id = 0;
    bool host = false;             // the room's host: never the successor
    bool racer = false;            // a race seat; false for a watch seat
    bool present = true;           // connected now: an absent member is never chosen
    bool directReachable = false;  // reported a port mapping or a global IPv6 address in hello
};

// The successor's id, or no value when nobody is eligible. The roster is in join order, and a tie goes to the member
// who joined first, so the same roster always gives the same answer. Reachability ranks members only in a directory
// room, where the room knows which members can take inbound connections.
std::optional<uint8_t> Successor(const std::vector<Member>& roster, bool directoryRoom);

// What one copy does about the host. The caller reports stillHearsOldHost from evidence other than this copy's own
// link, because a silent link alone does not show that the host is gone.
enum class Step : uint8_t {
    Wait,            // keep following the host, or keep waiting for a rejoin
    Promote,         // this copy takes the room over
    IgnoreSuccessor, // the host has gone quiet on this link but is still alive: take nothing over
    Close,           // a promoted copy got no rejoin in time: close the room
};

struct SplitBrainInput {
    uint32_t msSinceHostPacket = 0;  // silence on this copy's link to the old host
    bool handoverReceived = false;   // the old host handed the room over. It is the host's own word, so it promotes at once
    bool stillHearsOldHost = false;  // the old host is alive by evidence other than this link (decision 22)
    bool isSuccessor = false;        // the roster names this copy as the successor
    bool promoted = false;           // this copy already runs the room
    uint32_t msSincePromotion = 0;   // meaningful only when promoted
    uint32_t rejoinsReceived = 0;    // rejoins admitted since promotion
};

Step SplitBrain(const SplitBrainInput& in);

// A client's wait for a successor. True once the host has been silent for kHostSilenceMs + kSuccessorWaitMs and no
// successor has answered. The room then closes with the plain message (decision 16). A successor that answered keeps
// the room open, whatever the silence from the old host.
bool NoSuccessorClose(uint32_t msSinceHostPacket, bool successorAnswered);

}  // namespace gdx::net::succession
