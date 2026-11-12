// port/net/gdx_net_open_rooms.h: the Open room list and Quick race, a headless model for the native page (G-NET O6.2).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no thread, no game header. Every time is a millisecond argument. The clock must be
// monotonic: the cooldown reads elapsed time as the difference of two readings.
//
// The model never sends. Press returns the action the page should take, and the caller sends that one request.
// Nothing reaches the directory without a press (decision 38). The directory server picks a Quick race too (another
// worker writes it). PickQuickRace is the same rule on a list the client holds, for tests and for a reply that
// lists rooms.
//
// Decision 38. The list keeps at most kMaxListed rooms, and a request may go out once kRefreshCooldownMs have passed
// since the last request of either kind, the first one always. That caps a player at two requests a minute.
// Decision 37. Quick race takes the Open room with the most humans that has a free race seat and passed the
// reachability test, ties at random. With no such room, the page offers to host an Open room.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx::net::open_rooms {

constexpr uint32_t kRefreshCooldownMs = 30000;
constexpr size_t kMaxListed = 10;

// The room types of O6.1, in the order the list shows them. Their wire values come with O6.1, not here.
enum class RoomType : uint8_t {
    Code,
    Open,
    FriendsOnly,
    Lan,
};

struct RoomRecord {
    std::string code;          // the room code (O5.2), as the directory gave it
    uint8_t humans = 0;
    uint8_t freeRaceSeats = 0;
    bool reachable = false;    // passed the reachability test (decision 33)
    RoomType roomType = RoomType::Code;
};

// The two requests the page can make. Both count against the same cooldown.
enum class Intent : uint8_t {
    Refresh,
    QuickRace,
};

// What the page does with a press. When send is true, the caller sends one request of `intent` and no other. When
// send is false, nothing changed and waitMs is what is left of the cooldown.
struct Action {
    bool send = false;
    Intent intent = Intent::Refresh;
    uint32_t waitMs = 0;
};

struct QuickRaceChoice {
    bool found = false;  // false: offer to host an Open room
    size_t index = 0;    // into the list given, when found
};

// The room Quick race picks from `rooms`. Among the Open rooms with a free race seat that passed the reachability
// test, the one with the most humans wins. Among rooms tied on humans, randomIndex picks by position in the list, so
// the caller's random draw decides, and the same index always gives the same room.
QuickRaceChoice PickQuickRace(const std::vector<RoomRecord>& rooms, uint32_t randomIndex);

class Model {
public:
    // The page's press. A send records the time. A refused press records nothing, so it cannot push the next one back.
    Action Press(Intent intent, uint32_t nowMs);

    // Replaces the list with the first kMaxListed rooms of the reply, in the order given.
    void OnReply(const std::vector<RoomRecord>& reply);

    const std::vector<RoomRecord>& Rooms() const {
        return rooms_;
    }

private:
    bool sent_ = false;
    uint32_t lastSentMs_ = 0;
    std::vector<RoomRecord> rooms_;
};

}  // namespace gdx::net::open_rooms
