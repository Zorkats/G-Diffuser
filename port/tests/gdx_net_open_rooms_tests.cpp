// port/tests/gdx_net_open_rooms_tests.cpp: the Open room list, the request cooldown and Quick race ties (G-NET O6.2).
// Copyright (c) Zorkats
//
// Usage: gdx_net_open_rooms_tests
// Prints one line per failed check, then "<N> checks, <M> failed". Exit 0 only when M is 0.

#include "gdx_net_open_rooms.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using gdx::net::open_rooms::Action;
using gdx::net::open_rooms::Intent;
using gdx::net::open_rooms::Model;
using gdx::net::open_rooms::PickQuickRace;
using gdx::net::open_rooms::QuickRaceChoice;
using gdx::net::open_rooms::RoomRecord;
using gdx::net::open_rooms::RoomType;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

RoomRecord Room(const std::string& code, uint8_t humans, uint8_t seats, bool reachable,
                RoomType type = RoomType::Open) {
    RoomRecord room;
    room.code = code;
    room.humans = humans;
    room.freeRaceSeats = seats;
    room.reachable = reachable;
    room.roomType = type;
    return room;
}

// R00, R01 ... so that list order and code order agree in every list the tests build.
std::string Name(int i) {
    return (i < 10 ? "R0" : "R") + std::to_string(i);
}

void CheckCooldown() {
    // The first request is always sent. Then 30 s must pass, exactly.
    Model model;
    const Action first = model.Press(Intent::Refresh, 0);
    Check(first.send && first.intent == Intent::Refresh, "the first refresh is sent at once");

    const Action at29999 = model.Press(Intent::Refresh, 29999);
    Check(!at29999.send && at29999.waitMs == 1, "a refresh at 29999 ms is refused with 1 ms left");
    const Action at30000 = model.Press(Intent::Refresh, 30000);
    Check(at30000.send, "a refresh at 30000 ms is sent");

    // Both kinds of request share one cooldown: each sent request starts it.
    Model shared;
    Check(shared.Press(Intent::QuickRace, 0).send, "a Quick race request is sent when none was");
    const Action refresh10000 = shared.Press(Intent::Refresh, 10000);
    Check(!refresh10000.send && refresh10000.waitMs == 20000, "a refresh 10 s after a Quick race waits 20 s");
    Check(refresh10000.intent == Intent::Refresh, "a refused action still names its intent");
    const Action quick29999 = shared.Press(Intent::QuickRace, 29999);
    Check(!quick29999.send && quick29999.waitMs == 1, "a Quick race at 29999 ms is refused with 1 ms left");
    Check(shared.Press(Intent::QuickRace, 30000).send, "a Quick race at 30000 ms is sent");

    // A refused press records nothing: the refusals at 15 s and 29.999 s do not push the next send back.
    Model refused;
    Check(refused.Press(Intent::Refresh, 0).send, "the press that starts the cooldown is sent");
    const Action at15000 = refused.Press(Intent::Refresh, 15000);
    Check(!at15000.send && at15000.waitMs == 15000, "a press at 15000 ms is refused with 15 s left");
    Check(!refused.Press(Intent::Refresh, 29999).send, "a press at 29999 ms is still refused");
    Check(refused.Press(Intent::Refresh, 30000).send, "the refusals did not move the 30 s point");

    // The clock may wrap at 2^32 ms. The elapsed time is still counted correctly across the wrap.
    Model wrap;
    const uint32_t start = 0xFFFFFFF0u;
    Check(wrap.Press(Intent::Refresh, start).send, "a refresh is sent just before the clock wraps");
    const Action wrapped29999 = wrap.Press(Intent::Refresh, start + 29999u);
    Check(!wrapped29999.send && wrapped29999.waitMs == 1, "29999 ms across the wrap is still refused");
    Check(wrap.Press(Intent::Refresh, start + 30000u).send, "30000 ms across the wrap is sent");
}

void CheckListCap() {
    std::vector<RoomRecord> eleven;
    for (int i = 0; i < 11; i++) {
        eleven.push_back(Room(Name(i), 2, 1, true));
    }
    Model model;
    model.OnReply(eleven);
    Check(model.Rooms().size() == 10, "eleven rooms in a reply keep ten");
    bool inOrder = model.Rooms().size() == 10;
    for (size_t i = 0; inOrder && i < model.Rooms().size(); i++) {
        inOrder = model.Rooms()[i].code == Name(static_cast<int>(i));
    }
    Check(inOrder, "the first ten rooms are kept in the order the reply gave them");

    std::vector<RoomRecord> twentyFive;
    for (int i = 0; i < 25; i++) {
        twentyFive.push_back(Room(Name(i), 1, 1, true));
    }
    model.OnReply(twentyFive);
    Check(model.Rooms().size() == 10, "a reply of 25 rooms keeps ten");
    Check(model.Rooms().back().code == Name(9), "the tenth kept room is the tenth in the reply");

    std::vector<RoomRecord> ten(twentyFive.begin(), twentyFive.begin() + 10);
    model.OnReply(ten);
    Check(model.Rooms().size() == 10, "a reply of exactly ten rooms keeps all ten");

    std::vector<RoomRecord> three(twentyFive.begin(), twentyFive.begin() + 3);
    model.OnReply(three);
    Check(model.Rooms().size() == 3, "a reply of three rooms keeps three");

    model.OnReply({});
    Check(model.Rooms().empty(), "an empty reply empties the list");

    // A reply replaces the list. It does not add to it.
    model.OnReply(three);
    model.OnReply(std::vector<RoomRecord>(three.begin(), three.begin() + 2));
    Check(model.Rooms().size() == 2, "a second reply replaces the first instead of adding to it");

    // A press changes no room in the list.
    model.Press(Intent::Refresh, 0);
    model.Press(Intent::QuickRace, 1);
    Check(model.Rooms().size() == 2, "presses leave the list as the reply left it");
}

void CheckQuickRaceChoice() {
    // Nothing to join: an empty list, and no room that fits.
    Check(!PickQuickRace({}, 0).found, "an empty list offers to host an Open room");
    const std::vector<RoomRecord> none = {
        Room("A", 6, 2, true, RoomType::Code),
        Room("B", 5, 2, true, RoomType::FriendsOnly),
        Room("C", 4, 2, true, RoomType::Lan),
        Room("D", 7, 0, true),
        Room("E", 8, 3, false),
    };
    Check(!PickQuickRace(none, 0).found, "no eligible room offers to host an Open room");

    // The most humans wins among the eligible rooms. Bigger rooms that fail a test do not count.
    const std::vector<RoomRecord> most = {
        Room("A", 2, 1, true),
        Room("B", 5, 1, true),
        Room("C", 9, 4, true, RoomType::Code),
        Room("D", 8, 0, true),
        Room("E", 7, 3, false),
        Room("F", 4, 2, true),
    };
    const QuickRaceChoice chosen = PickQuickRace(most, 12345);
    Check(chosen.found && chosen.index == 1, "the Open room with the most humans wins (B), not the most seats");

    // A single eligible room with one free seat is enough. The seat count only has to be above zero.
    const std::vector<RoomRecord> oneSeat = {Room("X", 3, 1, true)};
    const QuickRaceChoice single = PickQuickRace(oneSeat, 0);
    Check(single.found && single.index == 0, "one free seat is enough to be eligible");

    // Ties: the random index picks among the tied rooms, by position in the list.
    const std::vector<RoomRecord> tie = {
        Room("T0", 4, 1, true),
        Room("X", 4, 1, true, RoomType::Lan),  // tied on humans, wrong type
        Room("T1", 4, 2, true),
        Room("Y", 4, 0, true),                 // tied on humans, no free seat
        Room("T2", 4, 3, true),
        Room("Z", 2, 3, true),                 // fewer humans
    };
    Check(PickQuickRace(tie, 0).index == 0, "tie: index 0 picks the first tied room (T0)");
    Check(PickQuickRace(tie, 1).index == 2, "tie: index 1 picks the second tied room (T1)");
    Check(PickQuickRace(tie, 2).index == 4, "tie: index 2 picks the third tied room (T2)");
    Check(PickQuickRace(tie, 3).index == 0, "tie: index 3 wraps to T0");
    Check(PickQuickRace(tie, 7).index == 2, "tie: index 7 picks T1");
    Check(PickQuickRace(tie, 0xFFFFFFFFu).index == 0, "tie: the largest index wraps to T0");
    Check(PickQuickRace(tie, 0xFFFFFFFEu).index == 4, "tie: the next largest index picks T2");
    Check(PickQuickRace(tie, 5).index == 4, "tie: index 5 picks T2, and the same index picks it every time");
    Check(PickQuickRace(tie, 5).index == 4, "tie: index 5 picks T2 on a second call");

    // A single tied room ignores the random index.
    const std::vector<RoomRecord> lone = {Room("S", 2, 1, true)};
    Check(PickQuickRace(lone, 0).index == 0 && PickQuickRace(lone, 999).index == 0, "a lone candidate ignores the index");

    // The model's list is the list Quick race picks from, and the cap applies first.
    Model model;
    model.OnReply(tie);
    Check(PickQuickRace(model.Rooms(), 1).index == 2, "Quick race on the model's list gives the same pick");

    std::vector<RoomRecord> beyondCap;
    for (int i = 0; i < 10; i++) {
        beyondCap.push_back(Room(Name(i), 1, 1, true));
    }
    beyondCap.push_back(Room("BEST", 9, 4, true));
    model.OnReply(beyondCap);
    bool bestKept = false;
    for (const RoomRecord& room : model.Rooms()) {
        bestKept = bestKept || room.code == "BEST";
    }
    Check(model.Rooms().size() == 10 && !bestKept, "a room beyond the tenth in a reply is not in the list Quick race picks from");
}

}  // namespace

int main() {
    CheckCooldown();
    CheckListCap();
    CheckQuickRaceChoice();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
