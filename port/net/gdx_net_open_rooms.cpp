// port/net/gdx_net_open_rooms.cpp: the Open room list, Quick race and the request cooldown (G-NET O6.2).
// Copyright (c) Zorkats

#include "gdx_net_open_rooms.h"

#include <algorithm>
#include <cstddef>

namespace gdx::net::open_rooms {
namespace {

// Quick race only ever joins a room that can take a player now and that another copy can reach (decision 37).
bool QuickRaceEligible(const RoomRecord& room) {
    return room.roomType == RoomType::Open && room.freeRaceSeats > 0 && room.reachable;
}

}  // namespace

QuickRaceChoice PickQuickRace(const std::vector<RoomRecord>& rooms, uint32_t randomIndex) {
    bool any = false;
    uint8_t most = 0;
    for (const RoomRecord& room : rooms) {
        if (!QuickRaceEligible(room)) {
            continue;
        }
        if (!any || room.humans > most) {
            most = room.humans;
            any = true;
        }
    }
    if (!any) {
        return QuickRaceChoice{};
    }
    // Positions of the tied rooms, in list order. The random index chooses among them, so the list order is part of
    // the rule and the caller's draw is the only randomness.
    std::vector<size_t> tied;
    for (size_t i = 0; i < rooms.size(); i++) {
        if (QuickRaceEligible(rooms[i]) && rooms[i].humans == most) {
            tied.push_back(i);
        }
    }
    QuickRaceChoice choice;
    choice.found = true;
    choice.index = tied[randomIndex % tied.size()];
    return choice;
}

Action Model::Press(Intent intent, uint32_t nowMs) {
    Action action;
    action.intent = intent;
    // Unsigned subtraction keeps the elapsed time right across the 32-bit wrap of a millisecond clock.
    const uint32_t elapsed = nowMs - lastSentMs_;
    if (!sent_ || elapsed >= kRefreshCooldownMs) {
        action.send = true;
        sent_ = true;
        lastSentMs_ = nowMs;
    } else {
        action.waitMs = kRefreshCooldownMs - elapsed;
    }
    return action;
}

void Model::OnReply(const std::vector<RoomRecord>& reply) {
    // The reply's order is the list's order: the directory has already ranked what it sent.
    const size_t keep = std::min(reply.size(), kMaxListed);
    rooms_.assign(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(keep));
}

}  // namespace gdx::net::open_rooms
