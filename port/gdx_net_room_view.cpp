// port/gdx_net_room_view.cpp -- see gdx_net_room_view.h.
// Copyright (c) Zorkats
//
// The copy is filled under the session's lock without copying the session's own state first: the game
// reads it every frame and the state holds strings and a log of 200 lines. Everything the C side gets is
// bounded here, so a screen never has to ask how long a list is.

#include "gdx_net_room_view.h"

#include "gdx_lan_lobby.h"
#include "gdx_net_online.h"
#include "gdx_net_race_room.h"
#include "gdx_net_text.h"
#include "net/gdx_net_codes.h"
#include "net/gdx_net_directory.h"
#include "net/gdx_net_friends.h"
#include "net/gdx_net_track_pack.h"

#include "libultraship/bridge/consolevariablebridge.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

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
static_assert(static_cast<int>(Phase::Assets) == GDX_NET_ROOM_PHASE_ASSETS, "phase");
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
static_assert(kTrackCustom == GDX_NET_ROOM_TRACK_CUSTOM, "track");
static_assert(kTrackIdBytes == GDX_NET_ROOM_TRACK_ID_BYTES, "track id");
static_assert(kTrackTitleMax + 1 == GDX_NET_ROOM_TITLE_BYTES, "track title");
static_assert(kEmoteCount == GDX_NET_ROOM_MESSAGES, "messages");
static_assert(static_cast<int>(RoomType::Lan) == GDX_NET_ROOM_TYPE_LAN, "room type");
static_assert(static_cast<int>(RoomType::Code) == GDX_NET_ROOM_TYPE_CODE, "room type");
static_assert(static_cast<int>(RoomType::Open) == GDX_NET_ROOM_TYPE_OPEN, "room type");
static_assert(static_cast<int>(RoomType::Friends) == GDX_NET_ROOM_TYPE_FRIENDS, "room type");
static_assert(kRoomTypeCount == GDX_NET_ROOM_TYPES, "room type");
static_assert(static_cast<int>(gdx::online::ListState::Off) == GDX_NET_ROOM_LISTING_OFF, "listing");
static_assert(static_cast<int>(gdx::online::ListState::Gathering) == GDX_NET_ROOM_LISTING_GATHERING, "listing");
static_assert(static_cast<int>(gdx::online::ListState::Listing) == GDX_NET_ROOM_LISTING_LISTING, "listing");
static_assert(static_cast<int>(gdx::online::ListState::Listed) == GDX_NET_ROOM_LISTING_LISTED, "listing");
static_assert(static_cast<int>(gdx::online::ListState::Failed) == GDX_NET_ROOM_LISTING_FAILED, "listing");
static_assert(static_cast<int>(gdx::online::Lists::Status::Idle) == GDX_NET_ROOM_DIR_IDLE, "lists");
static_assert(static_cast<int>(gdx::online::Lists::Status::Asking) == GDX_NET_ROOM_DIR_ASKING, "lists");
static_assert(static_cast<int>(gdx::online::Lists::Status::Ready) == GDX_NET_ROOM_DIR_READY, "lists");
static_assert(static_cast<int>(gdx::online::Lists::Status::Failed) == GDX_NET_ROOM_DIR_FAILED, "lists");
static_assert(static_cast<int>(gdx::online::Ask::Open) == GDX_NET_ROOM_ASK_OPEN, "ask");
static_assert(static_cast<int>(gdx::online::Ask::Quick) == GDX_NET_ROOM_ASK_QUICK, "ask");
static_assert(static_cast<int>(gdx::online::Ask::Friend) == GDX_NET_ROOM_ASK_FRIEND, "ask");
static_assert(gdx::net::codes::kFriendCodeChars == GDX_NET_ROOM_FRIEND_CODE_CHARS, "friend code");
static_assert(gdx::net::codes::kFriendCodeChars + gdx::net::codes::kFriendCodeChars / 4 <=
                  GDX_NET_ROOM_FRIEND_CODE_BYTES,
              "friend code text");
static_assert(gdx::net::friends::kMaxFriends == GDX_NET_ROOM_FRIENDS_MAX, "friends");
static_assert(gdx::net::codes::kRoomCodeChars == GDX_NET_ROOM_CODE_CHARS, "code");

// The first-use notice the player last read. Raising it shows the notice again, to everyone, once. 2: a room
// gets the friend code of each member, and a host can ban it (O7.3).
constexpr const char* kNoticeCvar = "gEnhancements.Online.NoticeRead";
constexpr int kNoticeVersion = 2;

// The directory's room type in the session's numbering.
RoomType TypeOf(gdx::net::directory::RoomType type) {
    switch (type) {
        case gdx::net::directory::RoomType::Code:
            return RoomType::Code;
        case gdx::net::directory::RoomType::Friends:
            return RoomType::Friends;
        default:
            return RoomType::Open;
    }
}

void CopyListed(const gdx::net::directory::Record& from, GdxNetRoomListed& to) {
    uint32_t value = 0;
    const std::string code = gdx::net::codes::ParseRoomCode(from.code, value) ? gdx::online::CodeText(value) : "";
    std::snprintf(to.code, sizeof(to.code), "%s", code.c_str());
    to.type = static_cast<unsigned char>(TypeOf(from.draft.type));
    to.humans = static_cast<unsigned char>(std::min<uint32_t>(from.draft.humans, 255u));
    to.freeSeats = static_cast<unsigned char>(std::min<uint32_t>(from.draft.freeSeats, 255u));
    to.reachable = from.draft.reachable ? 1 : 0;
}

void FillLists(const gdx::online::Lists& lists, GdxNetRoomView& out) {
    out.dirStatus = static_cast<int>(lists.status);
    out.dirAsk = static_cast<int>(lists.ask);
    out.dirSerial = lists.serial;
    std::snprintf(out.dirText, sizeof(out.dirText), "%s", lists.text.c_str());
    out.listedCount = static_cast<int>(std::min<size_t>(lists.rooms.size(), GDX_NET_ROOM_LISTED));
    for (int i = 0; i < out.listedCount; i++) {
        CopyListed(lists.rooms[static_cast<size_t>(i)], out.listed[i]);
    }
    out.quickFound = lists.quickFound ? 1 : 0;
    if (lists.quickFound) {
        CopyListed(lists.quick, out.quick);
    }
}

uint32_t SteadyMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

const char* const kCupNames[4] = { "Jack", "Queen", "King", "Joker" };
constexpr size_t kCupTracks = 6;

template <size_t N> void Copy(char (&out)[N], const std::string& text) {
    std::snprintf(out, N, "%s", text.c_str());
}

void CopyBallot(const Ballot& from, GdxNetRoomBallot& to) {
    to.id = from.id;
    to.track = from.track;
    if (from.track == kTrackCustom) {
        std::copy(from.custom.id.begin(), from.custom.id.end(), to.custom);
        Copy(to.title, from.custom.title);
    }
}

void Fill(const View& view, GdxNetRoomView& out) {
    out.mode = static_cast<int>(view.mode);
    Copy(out.status, view.status);
    out.port = view.port;
    Copy(out.hostText, view.hostText);
    out.localId = view.localId;
    Copy(out.joinCode, view.joinCode);

    out.roomType = static_cast<int>(view.room.type);
    Copy(out.code, gdx::online::CodeText(view.room.code));
    out.listing = static_cast<int>(view.listing.state);
    Copy(out.listingText, view.listing.text);
    out.reachable = view.listing.reachable ? 1 : 0;
    Copy(out.publicAddress, view.listing.publicAddress);

    out.phase = static_cast<int>(view.room.phase);
    out.phaseLeftMs = view.room.phaseLeftMs;
    out.phaseTotalMs = view.room.phaseTotalMs;
    out.raceIndex = view.room.raceIndex;
    out.racerCap = view.room.racerCap;
    out.customTracks = view.room.customTracks ? 1 : 0;
    out.cpuFill = view.room.cpuFill ? 1 : 0;
    out.successor = view.room.successor < kMaxPlayers ? view.room.successor : GDX_NET_ROOM_NOBODY;
    out.pool = view.room.pool;
    out.track = view.room.track;
    if (view.room.track == kTrackCustom) {
        std::copy(view.room.custom.id.begin(), view.room.custom.id.end(), out.trackId);
        Copy(out.trackTitle, view.room.custom.title);
    }
    out.trackHeld = view.trackProgress.held ? 1 : 0;
    out.trackFailed = view.trackProgress.failed ? 1 : 0;
    out.trackChunks = static_cast<unsigned>(view.trackProgress.chunks);
    out.trackChunksOf = static_cast<unsigned>(view.trackProgress.of);
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
        CopyBallot(view.votes[static_cast<size_t>(i)], out.ballots[i]);
    }

    out.lotteryCount = static_cast<int>(std::min<size_t>(view.lottery.ballots.size(), GDX_NET_ROOM_RACERS));
    for (int i = 0; i < out.lotteryCount; i++) {
        CopyBallot(view.lottery.ballots[static_cast<size_t>(i)], out.lottery[i]);
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
        to.ip = from.address.Ipv4(); // browsing is IPv4 broadcast
        to.port = from.address.port;
        to.racers = from.room.racers;
        to.racerCap = from.room.racerCap;
        to.watchers = from.room.watchers;
        to.phase = static_cast<unsigned char>(from.room.phase);
        Copy(to.hostName, from.room.hostName);
    }
}

} // namespace

extern "C" void gdx_lan_room_view(GdxNetRoomView* out) {
    *out = GdxNetRoomView{};
    gdx::lan::Read([&](const View& view) { Fill(view, *out); });
    FillLists(gdx::online::ListsSnapshot(), *out);
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

extern "C" int gdx_lan_room_host(int type, char* error, int size) {
    LocalProfile me;
    std::string reason = "Make a profile first.";
    if (type < 0 || type >= GDX_NET_ROOM_TYPES) {
        type = GDX_NET_ROOM_TYPE_LAN;
    }
    if (gdx::lan::ActiveProfile(me)) {
        reason.clear();
        if (gdx::lan::StartHost(gdx::lan::PortSetting(), me, static_cast<RoomType>(type), reason)) {
            // The room carries the rules and CPU settings from the start, so a successor has them even if
            // the host is lost before the first grid (D28.1). The lock-in screen captures them again.
            gdx_lan_race_capture_rules();
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
    const gdx::net::NetAddress host = gdx::net::NetAddress::FromV4(ip, static_cast<uint16_t>(port));
    gdx::lan::StartJoin(host, me, watch != 0 ? Seat::Watch : Seat::Race);
    return 1;
}

extern "C" int gdx_lan_room_directory_ok(void) {
    return gdx::online::DirectoryConfigured() ? 1 : 0;
}

extern "C" int gdx_lan_room_join_code(const char* code, int watch) {
    LocalProfile me;
    uint32_t value = 0;
    if (code == nullptr || !gdx::net::codes::ParseRoomCode(code, value) || !gdx::online::DirectoryConfigured() ||
        !gdx::lan::ActiveProfile(me)) {
        return 0;
    }
    gdx::lan::StartJoinCode(value, me, watch != 0 ? Seat::Watch : Seat::Race);
    return 1;
}

extern "C" int gdx_lan_room_ask(int ask, int* waitSeconds) {
    uint32_t waitMs = 0;
    if (waitSeconds != nullptr) {
        *waitSeconds = 0;
    }
    if (ask != GDX_NET_ROOM_ASK_OPEN && ask != GDX_NET_ROOM_ASK_QUICK) {
        return 0;
    }
    if (gdx::online::AskDirectory(static_cast<gdx::online::Ask>(ask), std::string(), SteadyMs(), waitMs)) {
        return 1;
    }
    if (waitSeconds != nullptr) {
        *waitSeconds = static_cast<int>((waitMs + 999u) / 1000u);
    }
    return 0;
}

extern "C" int gdx_lan_room_join_listed(int index, int watch) {
    LocalProfile me;
    const gdx::online::Lists lists = gdx::online::ListsSnapshot();
    const gdx::net::directory::Record* record = nullptr;
    if (lists.status != gdx::online::Lists::Status::Ready) {
        return 0;
    }
    if (index < 0) {
        record = (lists.ask == gdx::online::Ask::Quick && lists.quickFound) ? &lists.quick : nullptr;
    } else if (lists.ask != gdx::online::Ask::Quick && static_cast<size_t>(index) < lists.rooms.size() &&
               index < GDX_NET_ROOM_LISTED) {
        record = &lists.rooms[static_cast<size_t>(index)];
    }
    if (record == nullptr || !gdx::lan::ActiveProfile(me)) {
        return 0;
    }
    gdx::lan::StartJoinRecord(*record, me, watch != 0 ? Seat::Watch : Seat::Race);
    return 1;
}

extern "C" int gdx_lan_room_notice_seen(void) {
    return CVarGetInteger(kNoticeCvar, 0) >= kNoticeVersion ? 1 : 0;
}

extern "C" void gdx_lan_room_notice_accept(void) {
    CVarSetInteger(kNoticeCvar, kNoticeVersion);
    CVarSave();
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

namespace {

// A track of this copy's as a ballot or a pick names it. false when the title could not travel.
bool OwnChoice(const unsigned char* id, const char* title, CustomTrack& out) {
    if (id == nullptr || title == nullptr) {
        return false;
    }
    std::copy(id, id + kTrackIdBytes, out.id.begin());
    out.title = title;
    return TrackTitleOk(out.title);
}

} // namespace

extern "C" void gdx_lan_room_vote_custom(const unsigned char* id, const char* title) {
    CustomTrack track;
    if (OwnChoice(id, title, track)) {
        gdx::lan::VoteCustom(track);
    }
}

extern "C" void gdx_lan_room_pick_custom(const unsigned char* id, const char* title) {
    CustomTrack track;
    if (OwnChoice(id, title, track)) {
        gdx::lan::PickCustom(track);
    }
}

extern "C" void gdx_lan_room_keep_track(const unsigned char* id, const unsigned char* bytes, unsigned long length) {
    if (id == nullptr || bytes == nullptr || length == 0 || length > gdx::net::track::kMaxBytes) {
        return;
    }
    gdx::net::track::TrackId key;
    std::copy(id, id + kTrackIdBytes, key.begin());
    gdx::lan::KeepOwnTrack(key, std::vector<uint8_t>(bytes, bytes + length));
}

extern "C" void gdx_lan_room_start(void) {
    gdx::lan::StartSeries();
}

extern "C" void gdx_lan_room_racer_cap(int cap) {
    if (cap >= GDX_NET_ROOM_SEATS_MIN && cap <= GDX_NET_ROOM_SEATS_MAX) {
        gdx::lan::SetRacerCap(static_cast<uint8_t>(cap));
    }
}

extern "C" void gdx_lan_room_custom_tracks(int on) {
    gdx::lan::SetCustomTracks(on != 0);
}

extern "C" void gdx_lan_room_cpu_fill(int on) {
    gdx::lan::SetCpuFill(on != 0);
}

extern "C" void gdx_lan_room_kick(int id) {
    if (id >= 0 && id < GDX_NET_ROOM_NOBODY) {
        gdx::lan::Kick(static_cast<uint8_t>(id));
    }
}

extern "C" void gdx_lan_room_ban(int id) {
    if (id >= 0 && id < GDX_NET_ROOM_NOBODY) {
        gdx::lan::Ban(static_cast<uint8_t>(id));
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

// ---- Friends (O7.4) ----

namespace {

int Put(char* out, int size, const std::string& text) {
    if (out == nullptr || size <= 0) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(size), "%s", text.c_str());
    return 1;
}

int AddResult(gdx::online::FriendAdd result) {
    switch (result) {
        case gdx::online::FriendAdd::Added:
            return GDX_NET_ROOM_FRIEND_ADDED;
        case gdx::online::FriendAdd::Updated:
            return GDX_NET_ROOM_FRIEND_RENAMED;
        case gdx::online::FriendAdd::Same:
            return GDX_NET_ROOM_FRIEND_LISTED;
        case gdx::online::FriendAdd::Full:
            return GDX_NET_ROOM_FRIEND_FULL;
        case gdx::online::FriendAdd::BadName:
            return GDX_NET_ROOM_FRIEND_BAD_NAME;
        case gdx::online::FriendAdd::Self:
            return GDX_NET_ROOM_FRIEND_SELF;
        default:
            return GDX_NET_ROOM_FRIEND_NOT_SAVED;
    }
}

bool FriendAt(int index, gdx::online::FriendEntry& out) {
    const std::vector<gdx::online::FriendEntry> friends = gdx::online::Friends();
    if (index < 0 || static_cast<size_t>(index) >= friends.size()) {
        return false;
    }
    out = friends[static_cast<size_t>(index)];
    return true;
}

bool MetAt(int index, gdx::lan::RoomMember& out) {
    const std::vector<gdx::lan::RoomMember> met = gdx::lan::LastRoom();
    if (index < 0 || index >= GDX_NET_ROOM_MET_MAX || static_cast<size_t>(index) >= met.size()) {
        return false;
    }
    out = met[static_cast<size_t>(index)];
    return true;
}

} // namespace

extern "C" int gdx_lan_room_my_friend_code(char* out, int size) {
    Bytes32 key{};
    if (!gdx::online::HostKeyBytes(key)) {
        Put(out, size, "");
        return 0;
    }
    return Put(out, size, gdx::online::FriendCodeOf(key));
}

extern "C" int gdx_lan_room_friend_count(void) {
    return static_cast<int>(gdx::online::Friends().size());
}

extern "C" int gdx_lan_room_friend_name(int index, char* out, int size) {
    gdx::online::FriendEntry entry;
    if (!FriendAt(index, entry)) {
        Put(out, size, "");
        return 0;
    }
    return Put(out, size, entry.name);
}

extern "C" int gdx_lan_room_friend_code(int index, char* out, int size) {
    gdx::online::FriendEntry entry;
    if (!FriendAt(index, entry)) {
        Put(out, size, "");
        return 0;
    }
    return Put(out, size, gdx::online::FriendCodeOf(entry.key));
}

extern "C" int gdx_lan_room_friend_add_code(const char* code) {
    Bytes32 key{};
    if (code == nullptr || !gdx::net::codes::ParseFriendCode(code, key.data())) {
        return GDX_NET_ROOM_FRIEND_BAD_CODE;
    }
    // A friend already listed keeps the name a room showed: a code carries none.
    if (gdx::online::IsFriend(key)) {
        return GDX_NET_ROOM_FRIEND_LISTED;
    }
    return AddResult(gdx::online::AddFriend(key, gdx::online::NewFriendName()));
}

extern "C" int gdx_lan_room_friend_remove(int index) {
    gdx::online::FriendEntry entry;
    return FriendAt(index, entry) && gdx::online::RemoveFriend(entry.key) ? 1 : 0;
}

extern "C" int gdx_lan_room_ask_friend(int index, int* waitSeconds) {
    gdx::online::FriendEntry entry;
    uint32_t waitMs = 0;
    if (waitSeconds != nullptr) {
        *waitSeconds = 0;
    }
    if (!FriendAt(index, entry)) {
        return 0;
    }
    const std::string hex = gdx::net::directory::SecretHex(entry.key);
    if (gdx::online::AskDirectory(gdx::online::Ask::Friend, hex, SteadyMs(), waitMs)) {
        return 1;
    }
    if (waitSeconds != nullptr) {
        *waitSeconds = static_cast<int>((waitMs + 999u) / 1000u);
    }
    return 0;
}

extern "C" int gdx_lan_room_met_count(void) {
    return static_cast<int>(std::min<size_t>(gdx::lan::LastRoom().size(), GDX_NET_ROOM_MET_MAX));
}

extern "C" int gdx_lan_room_met_name(int index, char* out, int size) {
    gdx::lan::RoomMember member;
    if (!MetAt(index, member)) {
        Put(out, size, "");
        return 0;
    }
    return Put(out, size, member.name);
}

extern "C" int gdx_lan_room_met_listed(int index) {
    gdx::lan::RoomMember member;
    if (!MetAt(index, member)) {
        return 0;
    }
    for (const gdx::online::FriendEntry& entry : gdx::online::Friends()) {
        if (entry.key == member.key) {
            return entry.name == member.name ? 1 : 0;
        }
    }
    return 0;
}

extern "C" int gdx_lan_room_met_add(int index) {
    gdx::lan::RoomMember member;
    if (!MetAt(index, member)) {
        return GDX_NET_ROOM_FRIEND_GONE;
    }
    return AddResult(gdx::online::AddFriend(member.key, member.name));
}

extern "C" int gdx_lan_room_clipboard_set(const char* text) {
    return text != nullptr && SDL_SetClipboardText(text) == 0 ? 1 : 0;
}

extern "C" int gdx_lan_room_clipboard_get(char* out, int size) {
    if (out == nullptr || size <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!SDL_HasClipboardText()) {
        return 0;
    }
    char* text = SDL_GetClipboardText();
    if (text == nullptr) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(size), "%s", text);
    SDL_free(text);
    return out[0] != '\0' ? 1 : 0;
}

