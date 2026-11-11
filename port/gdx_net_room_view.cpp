// port/gdx_net_room_view.cpp -- see gdx_net_room_view.h.
// Copyright (c) Zorkats
//
// The copy is filled under the session's lock without copying the session's own state first: the game
// reads it every frame and the state holds strings and a log of 200 lines. Everything the C side gets is
// bounded here, so a screen never has to ask how long a list is.

#include "gdx_net_room_view.h"

#include "gdx_lan_lobby.h"
#include "gdx_net_text.h"

#include <algorithm>
#include <cstdio>
#include <string>

extern "C" char* gTrackNames[];

namespace {

using gdx::lan::Mode;
using gdx::lan::View;
using namespace gdx::net::lobby;

// The C side's numbers are the session's, checked here once and not converted one by one.
static_assert(static_cast<int>(Mode::Idle) == GDX_NET_ROOM_MODE_IDLE, "mode");
static_assert(static_cast<int>(Mode::Browsing) == GDX_NET_ROOM_MODE_BROWSING, "mode");
static_assert(static_cast<int>(Mode::Hosting) == GDX_NET_ROOM_MODE_HOSTING, "mode");
static_assert(static_cast<int>(Mode::Joining) == GDX_NET_ROOM_MODE_JOINING, "mode");
static_assert(static_cast<int>(Mode::Joined) == GDX_NET_ROOM_MODE_JOINED, "mode");
static_assert(static_cast<int>(Mode::Closed) == GDX_NET_ROOM_MODE_CLOSED, "mode");
static_assert(static_cast<int>(Phase::Lobby) == GDX_NET_ROOM_PHASE_LOBBY, "phase");
static_assert(static_cast<int>(Phase::Voting) == GDX_NET_ROOM_PHASE_VOTING, "phase");
static_assert(static_cast<int>(Phase::Lottery) == GDX_NET_ROOM_PHASE_LOTTERY, "phase");
static_assert(static_cast<int>(Phase::Pick) == GDX_NET_ROOM_PHASE_PICK, "phase");
static_assert(static_cast<int>(Phase::Machine) == GDX_NET_ROOM_PHASE_MACHINE, "phase");
static_assert(static_cast<int>(Phase::GridSync) == GDX_NET_ROOM_PHASE_GRID_SYNC, "phase");
static_assert(static_cast<int>(Phase::Countdown) == GDX_NET_ROOM_PHASE_COUNTDOWN, "phase");
static_assert(static_cast<int>(Phase::Racing) == GDX_NET_ROOM_PHASE_RACING, "phase");
static_assert(static_cast<int>(Phase::Results) == GDX_NET_ROOM_PHASE_RESULTS, "phase");
static_assert(static_cast<int>(ResultKind::Finished) == GDX_NET_ROOM_RESULT_FINISHED, "result");
static_assert(static_cast<int>(ResultKind::Unfinished) == GDX_NET_ROOM_RESULT_UNFINISHED, "result");
static_assert(static_cast<int>(ResultKind::Retired) == GDX_NET_ROOM_RESULT_RETIRED, "result");
static_assert(kMaxPlayers == GDX_NET_ROOM_PLAYERS, "players");
static_assert(kMaxRacers == GDX_NET_ROOM_RACERS && kMaxRacers == GDX_NET_ROOM_SEATS_MAX, "racers");
static_assert(kDefaultRacers == GDX_NET_ROOM_SEATS_MIN, "racers");
static_assert(kMaxWatchers == GDX_NET_ROOM_WATCH_SEATS, "watchers");
static_assert(kNameMax + 1 == GDX_NET_ROOM_NAME_BYTES, "name");
static_assert(kDefaultPort == GDX_NET_ROOM_DEFAULT_PORT, "port");
static_assert(kSystemId == GDX_NET_ROOM_NOBODY, "nobody");
static_assert(kTrackRandom == GDX_NET_ROOM_TRACK_RANDOM && kTrackNone == GDX_NET_ROOM_TRACK_NONE, "track");
static_assert(kEmoteCount == GDX_NET_ROOM_MESSAGES, "messages");

const char* const kCupNames[4] = { "Jack", "Queen", "King", "Joker" };
constexpr size_t kCupTracks = 6;

template <size_t N> void Copy(char (&out)[N], const std::string& text) {
    std::snprintf(out, N, "%s", text.c_str());
}

void Fill(const View& view, const std::string& build, GdxNetRoomView& out) {
    out.mode = static_cast<int>(view.mode);
    Copy(out.status, view.status);
    out.port = view.port;
    Copy(out.hostText, view.hostText);
    out.localId = view.localId;

    out.phase = static_cast<int>(view.room.phase);
    out.phaseLeftMs = view.room.phaseLeftMs;
    out.phaseTotalMs = view.room.phaseTotalMs;
    out.raceIndex = view.room.raceIndex;
    out.racerCap = view.room.racerCap;
    out.pool = view.room.pool;
    out.track = view.room.track;
    out.lastTrack = view.room.lastTrack;
    out.picker = view.room.picker;
    out.lockedCount = view.room.locked;
    out.racers = view.room.racers;

    // A mute ends with the player's stay, so whoever is given that id next starts unmuted.
    gdx::lan::ForgetMutesOutside(view);

    out.playerCount = static_cast<int>(std::min<size_t>(view.roster.size(), GDX_NET_ROOM_PLAYERS));
    for (int i = 0; i < out.playerCount; i++) {
        const PlayerInfo& from = view.roster[static_cast<size_t>(i)];
        GdxNetRoomPlayer& to = out.players[i];
        to.id = from.id;
        to.host = from.host;
        to.ready = from.ready;
        to.watcher = from.watcher;
        to.raceNext = from.raceNext;
        to.inRace = from.inRace;
        to.locked = from.locked;
        to.voted = std::any_of(view.votes.begin(), view.votes.end(), [&](const Ballot& b) { return b.id == from.id; });
        to.muted = gdx::lan::Muted(from.id);
        to.machine = from.machine;
        to.colour = from.colour & 0xFFFFFFu;
        to.pingMs = from.pingMs;
        to.points = from.points;
        Copy(to.name, from.name);
    }

    out.ballotCount = static_cast<int>(std::min<size_t>(view.votes.size(), GDX_NET_ROOM_RACERS));
    for (int i = 0; i < out.ballotCount; i++) {
        out.ballots[i].id = view.votes[static_cast<size_t>(i)].id;
        out.ballots[i].track = view.votes[static_cast<size_t>(i)].track;
    }

    out.lotteryCount = static_cast<int>(std::min<size_t>(view.lottery.ballots.size(), GDX_NET_ROOM_RACERS));
    for (int i = 0; i < out.lotteryCount; i++) {
        out.lottery[i].id = view.lottery.ballots[static_cast<size_t>(i)].id;
        out.lottery[i].track = view.lottery.ballots[static_cast<size_t>(i)].track;
    }
    out.lotteryWinner = view.lottery.winner < out.lotteryCount ? view.lottery.winner : -1;
    out.lotteryTrack = view.lottery.track;
    out.lotteryChecked = view.lotteryChecked;

    out.pickedTrack = view.picked.track;
    out.pickedDrawn = view.picked.drawn;
    out.pickedBy = view.picked.by;

    out.haveResults = view.haveResults;
    out.resultsRace = view.results.raceIndex;
    out.resultsFinal = view.results.final;
    out.resultCount = static_cast<int>(std::min<size_t>(view.results.entries.size(), GDX_NET_ROOM_RACERS));
    for (int i = 0; i < out.resultCount; i++) {
        const ResultEntry& from = view.results.entries[static_cast<size_t>(i)];
        out.results[i].id = from.id;
        out.results[i].kind = static_cast<unsigned char>(from.kind);
        out.results[i].points = from.points;
        out.results[i].timeMs = from.timeMs;
    }

    // The newest lines this copy shows, found from the end of the log and then put oldest first.
    const ShownLine* shown[GDX_NET_ROOM_LINES];
    out.lineCount = 0;
    for (size_t i = view.lines.size(); i > 0 && out.lineCount < GDX_NET_ROOM_LINES; i--) {
        const ShownLine& line = view.lines[i - 1];
        if (line.from == kSystemId || !gdx::lan::Muted(line.from)) {
            shown[out.lineCount++] = &line;
        }
    }
    for (int i = 0; i < out.lineCount; i++) {
        const ShownLine& from = *shown[out.lineCount - 1 - i];
        out.lines[i].from = from.from;
        out.lines[i].colour = from.colour & 0xFFFFFFu;
        Copy(out.lines[i].name, from.name);
        Copy(out.lines[i].text, from.text);
    }

    out.foundCount = static_cast<int>(std::min<size_t>(view.rooms.size(), GDX_NET_ROOM_FOUND));
    for (int i = 0; i < out.foundCount; i++) {
        const LanBrowser::FoundRoom& from = view.rooms[static_cast<size_t>(i)];
        GdxNetRoomFound& to = out.found[i];
        to.ip = from.address.ip;
        to.port = from.address.port;
        to.racers = from.room.racers;
        to.racerCap = from.room.racerCap;
        to.watchers = from.room.watchers;
        to.phase = static_cast<unsigned char>(from.room.phase);
        to.sameBuild = from.room.build == build;
        Copy(to.hostName, from.room.hostName);
    }
}

} // namespace

extern "C" void gdx_lan_room_view(GdxNetRoomView* out) {
    static const std::string build = gdx::lan::BuildId();
    *out = GdxNetRoomView{};
    gdx::lan::Read([&](const View& view) { Fill(view, build, *out); });
}

extern "C" int gdx_lan_room_profile_ok(void) {
    LocalProfile me;
    return gdx::lan::ActiveProfile(me) ? 1 : 0;
}

extern "C" unsigned gdx_lan_room_port(void) {
    return gdx::lan::PortSetting();
}

extern "C" void gdx_lan_room_set_port(unsigned port) {
    if (port >= 1024u && port <= 65535u) {
        gdx::lan::SavePortSetting(static_cast<uint16_t>(port));
    }
}

extern "C" int gdx_lan_room_host(char* error, int size) {
    LocalProfile me;
    std::string reason = "Make a profile first.";
    if (gdx::lan::ActiveProfile(me)) {
        reason.clear();
        if (gdx::lan::StartHost(gdx::lan::PortSetting(), me, reason)) {
            return 1;
        }
        gdx::lan::Dismiss(); // the reason is shown by the caller, not as a closed session
    }
    if (size > 0) {
        std::snprintf(error, static_cast<size_t>(size), "%s", reason.c_str());
    }
    return 0;
}

extern "C" void gdx_lan_room_browse(void) {
    gdx::lan::StartBrowse(gdx::lan::PortSetting());
}

extern "C" int gdx_lan_room_join(unsigned ip, unsigned port, int watch) {
    LocalProfile me;
    if (ip == 0u || port == 0u || port > 65535u || !gdx::lan::ActiveProfile(me)) {
        return 0;
    }
    gdx::net::NetAddress host;
    host.ip = ip;
    host.port = static_cast<uint16_t>(port);
    gdx::lan::StartJoin(host, me, watch != 0 ? Seat::Watch : Seat::Race);
    return 1;
}

extern "C" void gdx_lan_room_stop(void) {
    gdx::lan::Stop();
}

extern "C" void gdx_lan_room_dismiss(void) {
    gdx::lan::Dismiss();
}

extern "C" void gdx_lan_room_ready(int ready) {
    gdx::lan::SetReady(ready != 0);
}

extern "C" void gdx_lan_room_race_next(int raceNext) {
    gdx::lan::RaceNext(raceNext != 0);
}

namespace {

// A track a ballot or a pick may name: a stock one or Random. The room checks it against its own pool.
bool ChoosableTrack(int track) {
    return track >= 0 && track <= 0xFF && TrackInPool(static_cast<uint8_t>(track), kPoolAll);
}

} // namespace

extern "C" void gdx_lan_room_vote(int track) {
    if (ChoosableTrack(track)) {
        gdx::lan::Vote(static_cast<uint8_t>(track));
    }
}

extern "C" void gdx_lan_room_pick(int track) {
    if (ChoosableTrack(track)) {
        gdx::lan::Pick(static_cast<uint8_t>(track));
    }
}

extern "C" void gdx_lan_room_start(void) {
    gdx::lan::StartSeries();
}

extern "C" void gdx_lan_room_racer_cap(int cap) {
    if (cap >= GDX_NET_ROOM_SEATS_MIN && cap <= GDX_NET_ROOM_SEATS_MAX) {
        gdx::lan::SetRacerCap(static_cast<uint8_t>(cap));
    }
}

extern "C" void gdx_lan_room_kick(int id) {
    if (id >= 0 && id < GDX_NET_ROOM_NOBODY) {
        gdx::lan::Kick(static_cast<uint8_t>(id));
    }
}

extern "C" void gdx_lan_room_skip_lottery(void) {
    gdx::lan::SkipLottery();
}

extern "C" void gdx_lan_room_end_series(void) {
    gdx::lan::EndSeries();
}

extern "C" void gdx_lan_room_message(int index) {
    if (index >= 0 && index < GDX_NET_ROOM_MESSAGES) {
        gdx::lan::Emote(static_cast<uint8_t>(index));
    }
}

extern "C" void gdx_lan_room_message_text(int index, char* out, int size) {
    if (size <= 0) {
        return;
    }
    const bool known = index >= 0 && index < GDX_NET_ROOM_MESSAGES;
    std::snprintf(out, static_cast<size_t>(size), "%s", known ? EmoteText(static_cast<uint8_t>(index)) : "");
}

extern "C" void gdx_lan_room_set_muted(int id, int muted) {
    if (id >= 0 && id < GDX_NET_ROOM_NOBODY) {
        gdx::lan::SetMuted(static_cast<uint8_t>(id), muted != 0);
    }
}

extern "C" void gdx_lan_room_track_name(int track, char* out, int size) {
    if (size <= 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(size);
    if (track == GDX_NET_ROOM_TRACK_RANDOM) {
        std::snprintf(out, bytes, "Random");
        return;
    }
    if (track < 0 || track >= static_cast<int>(kStockTracks)) {
        std::snprintf(out, bytes, "No track");
        return;
    }
    // The game fills the names at start-up; before that the cup and number stand in.
    const char* name = gTrackNames[track];
    if (name == nullptr || name[0] == '\0') {
        std::snprintf(out, bytes, "%s %d", kCupNames[static_cast<size_t>(track) / kCupTracks],
                      static_cast<int>(static_cast<size_t>(track) % kCupTracks) + 1);
        return;
    }
    gdx_net_text_track_name(name, out, size);
}
