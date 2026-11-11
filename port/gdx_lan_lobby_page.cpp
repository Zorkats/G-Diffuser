// port/gdx_lan_lobby_page.cpp -- see gdx_lan_lobby_page.h.
//
// The page draws from a copy of the room state (gdx::lan::Snapshot) and sends the player's actions to the
// worker, so a slow frame never stalls the room. The host decides everything; a button here only asks.
// Players talk with preset messages, so no typed text leaves this page. Muting a player is local and lasts
// while they stay in the room.
//
// The lock-in panel (machine, colour, engine) is a stand-in: the game's own machine screen takes its place
// in the next slice. The race itself runs in the game: this page shows its table, while it runs and when
// it is over.

#include "gdx_lan_lobby_page.h"

#include "gdx_lan_lobby.h"
#include "gdx_net_room_view.h"
#include "gdx_profile.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

extern "C" const char* gMachineNames[];

namespace {

using gdx::lan::ActiveProfile;
using gdx::lan::Mode;
using gdx::lan::PortSetting;
using gdx::lan::View;
using namespace gdx::net::lobby;

const ImVec4 kRed = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
const ImVec4 kGreen = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
const ImVec4 kGold = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
const char* const kCupNames[4] = { "Jack", "Queen", "King", "Joker" };
constexpr int kCupTracks = 6;
constexpr int kNoChoice = -1;

char sAddress[64] = "";
std::string sHostError;
uint32_t sSeenSerial = 0;
int sPortEdit = -1; // the Port field while the player types; -1 until first drawn
int sCapEdit = -1;  // the race seats slider while the host drags it; -1 when it follows the room
// The lock-in stand-in's choices, reset from the roster when a new race opens the panel.
int sLockRace = -1;
int sLockMachine = 0;
int sLockSkin = 0;
int sLockEngine = 50;

ImVec4 Colour(uint32_t rgb) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

// The game stores its names in capitals: "BLUE FALCON" becomes "Blue Falcon".
std::string TitleCase(const char* text) {
    std::string label = text;
    bool start = true;
    for (char& c : label) {
        const unsigned char u = static_cast<unsigned char>(c);
        c = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
        start = c == ' ';
    }
    return label;
}

std::string MachineLabel(uint8_t machine) {
    if (machine >= GDX_PROFILE_ROSTER_COUNT || gMachineNames[machine] == nullptr) {
        return "No machine";
    }
    return TitleCase(gMachineNames[machine]);
}

// The same name the game's room screen shows.
std::string TrackLabel(uint8_t track) {
    char name[48];
    gdx_lan_room_track_name(track, name, static_cast<int>(sizeof(name)));
    return name;
}

const PlayerInfo* FindPlayer(const View& view, uint8_t id) {
    for (const PlayerInfo& p : view.roster) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::string PlayerName(const View& view, uint8_t id) {
    const PlayerInfo* p = FindPlayer(view, id);
    return p != nullptr ? p->name : std::string("A player who left");
}

size_t CountRacers(const View& view) {
    size_t n = 0;
    for (const PlayerInfo& p : view.roster) {
        n += p.watcher ? 0 : 1;
    }
    return n;
}

bool HasVoted(const View& view, uint8_t id) {
    for (const Ballot& b : view.votes) {
        if (b.id == id) {
            return true;
        }
    }
    return false;
}

// The phase timer as a bar that runs down. Nothing for a phase without one.
void DrawTimer(const RoomState& room) {
    if (room.phaseTotalMs == 0) {
        return;
    }
    const unsigned seconds = (room.phaseLeftMs + 999) / 1000;
    char text[32];
    if (seconds >= 60) {
        std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    } else {
        std::snprintf(text, sizeof(text), "%u s", seconds);
    }
    ImGui::ProgressBar(static_cast<float>(room.phaseLeftMs) / static_cast<float>(room.phaseTotalMs),
                       ImVec2(-1.0f, 0.0f), text);
}

const char* SeatText(const View& view, const PlayerInfo& p, ImVec4& colour) {
    colour = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (p.watcher) {
        return p.raceNext ? "Watching, races next" : "Watching";
    }
    switch (view.room.phase) {
    case Phase::Lobby:
        if (p.ready) {
            colour = kGreen;
            return "Ready";
        }
        return "Not ready";
    case Phase::Voting:
        if (HasVoted(view, p.id)) {
            colour = kGreen;
            return "Voted";
        }
        return "Voting";
    case Phase::Pick:
        if (p.id == view.room.picker) {
            colour = kGold;
            return "Picks the track";
        }
        return "Waiting";
    case Phase::Machine:
        if (!p.inRace) {
            return "Watches this race";
        }
        if (p.locked) {
            colour = kGreen;
            return "Locked in";
        }
        return "Choosing";
    case Phase::GridSync:
    case Phase::Countdown:
    case Phase::Racing:
        return p.inRace ? "Racing" : "Watches this race";
    case Phase::Lottery:
    case Phase::Results:
        break;
    }
    return "In the room";
}

void DrawRoster(const View& view, bool isHost) {
    // The mutes are the session's (gdx_lan_lobby.h), shared with the game's own room screen.
    gdx::lan::ForgetMutesOutside(view);
    // The series points get a column once somebody has some.
    bool showPoints = false;
    for (const PlayerInfo& p : view.roster) {
        showPoints = showPoints || p.points != 0;
    }
    const int columns = (isHost ? 6 : 5) + (showPoints ? 1 : 0);
    if (!ImGui::BeginTable("##lanroster", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Machine", ImGuiTableColumnFlags_WidthStretch);
    if (showPoints) {
        ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed);
    if (isHost) {
        ImGui::TableSetupColumn("##kick", ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableHeadersRow();
    for (const PlayerInfo& p : view.roster) {
        ImGui::PushID(p.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::ColorButton("##colour", Colour(p.colour), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                           ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        std::string name = p.name;
        if (p.host) {
            name += " (host)";
        }
        if (p.id == view.localId) {
            name += " (you)";
        }
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableNextColumn();
        ImVec4 colour;
        const char* seat = SeatText(view, p, colour);
        ImGui::TextColored(colour, "%s", seat);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.watcher ? "" : MachineLabel(p.machine).c_str());
        ImGui::TableNextColumn();
        if (showPoints) {
            ImGui::Text("%u", static_cast<unsigned>(p.points));
            ImGui::TableNextColumn();
        }
        if (p.host) {
            ImGui::TextDisabled("host");
        } else {
            ImGui::Text("%u ms", static_cast<unsigned>(p.pingMs));
        }
        ImGui::TableNextColumn();
        if (p.id != view.localId) {
            bool muted = gdx::lan::Muted(p.id);
            if (ImGui::Checkbox("##mute", &muted)) {
                gdx::lan::SetMuted(p.id, muted);
            }
            ImGui::SetItemTooltip("Hide this player's messages on your screen only.");
        }
        if (isHost) {
            ImGui::TableNextColumn();
            if (!p.host && ImGui::SmallButton("Remove")) {
                gdx::lan::Kick(p.id);
            }
            if (!p.host) {
                ImGui::SetItemTooltip("Remove this player from the room.");
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// The 24 tracks, one column per cup, and Random below. Returns the track the player pressed, kTrackRandom
// for Random, or kNoChoice. With showVotes each button carries the ballots cast for it so far.
int DrawTrackGrid(const View& view, bool enabled, bool showVotes) {
    int chosen = kNoChoice;
    const auto votesFor = [&view](uint8_t track) {
        int n = 0;
        for (const Ballot& b : view.votes) {
            n += b.track == track ? 1 : 0;
        }
        return n;
    };
    const auto label = [&](uint8_t track) {
        std::string text = TrackLabel(track);
        const int n = showVotes ? votesFor(track) : 0;
        if (n > 0) {
            text += " (" + std::to_string(n) + ")";
        }
        return text + "##track" + std::to_string(track);
    };
    if (ImGui::BeginTable("##lantracks", 4, ImGuiTableFlags_SizingStretchSame)) {
        for (const char* cup : kCupNames) {
            ImGui::TableSetupColumn(cup);
        }
        ImGui::TableHeadersRow();
        for (int row = 0; row < kCupTracks; row++) {
            ImGui::TableNextRow();
            for (int cup = 0; cup < 4; cup++) {
                ImGui::TableNextColumn();
                const uint8_t track = static_cast<uint8_t>(cup * kCupTracks + row);
                ImGui::BeginDisabled(!enabled || !TrackInPool(track, view.room.pool));
                if (ImGui::Button(label(track).c_str(), ImVec2(-1.0f, 0.0f))) {
                    chosen = track;
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button(label(kTrackRandom).c_str())) {
        chosen = kTrackRandom;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Let the room draw a track. It never draws the one just raced.");
    return chosen;
}

void DrawLobbyPhase(const View& view, const PlayerInfo& me, bool isHost) {
    if (me.watcher) {
        bool next = me.raceNext;
        if (ImGui::Checkbox("Race next##lanlobby", &next)) {
            gdx::lan::RaceNext(next);
        }
        ImGui::SetItemTooltip("Take a free race seat when the next race is set up. First come, first served.");
    } else {
        bool ready = me.ready;
        if (ImGui::Checkbox("Ready##lanlobby", &ready)) {
            gdx::lan::SetReady(ready);
        }
    }
    if (!isHost) {
        ImGui::TextDisabled("The races start when every racer is ready, or when the host starts them.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Start the races##lanlobby")) {
        gdx::lan::StartSeries();
    }
    ImGui::SetItemTooltip("Starts now, ready or not. The races also start on their own once two or more racers\n"
                          "are in and every one is ready.");
    if (sCapEdit < 0) {
        sCapEdit = view.room.racerCap;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::SliderInt("Race seats##lanlobby", &sCapEdit, static_cast<int>(kDefaultRacers), static_cast<int>(kMaxRacers));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        gdx::lan::SetRacerCap(static_cast<uint8_t>(sCapEdit));
        sCapEdit = -1;
    } else if (!ImGui::IsItemActive()) {
        sCapEdit = -1;
    }
    ImGui::SetItemTooltip("How many players may race, from %u to %u. %u more may watch.",
                          static_cast<unsigned>(kDefaultRacers), static_cast<unsigned>(kMaxRacers),
                          static_cast<unsigned>(kMaxWatchers));
}

void DrawVotingPhase(const View& view, const PlayerInfo& me) {
    const bool voted = HasVoted(view, me.id);
    if (me.watcher) {
        ImGui::TextDisabled("The racers vote for the next track.");
    } else if (voted) {
        ImGui::TextUnformatted("Your vote is in.");
    } else {
        ImGui::TextUnformatted("Vote for the next track. One of the votes cast is drawn, so every vote counts.");
    }
    DrawTimer(view.room);
    ImGui::Text("%u of %u voted", static_cast<unsigned>(view.votes.size()), static_cast<unsigned>(CountRacers(view)));
    const int chosen = DrawTrackGrid(view, !me.watcher && !voted, true);
    if (chosen != kNoChoice) {
        gdx::lan::Vote(static_cast<uint8_t>(chosen));
    }
}

void DrawLotteryPhase(const View& view, bool isHost) {
    const LotteryResult& lottery = view.lottery;
    if (lottery.winner == kNoWinner || lottery.winner >= lottery.ballots.size()) {
        ImGui::Text("Nobody voted. The room drew %s.", TrackLabel(view.room.track).c_str());
    } else {
        for (size_t i = 0; i < lottery.ballots.size(); i++) {
            const Ballot& b = lottery.ballots[i];
            const std::string line = PlayerName(view, b.id) + ": " + TrackLabel(b.track);
            if (i == lottery.winner) {
                ImGui::TextColored(kGold, "%s", line.c_str());
            } else {
                ImGui::TextDisabled("%s", line.c_str());
            }
        }
        ImGui::Text("%s's vote won. Next track: %s.", PlayerName(view, lottery.ballots[lottery.winner].id).c_str(),
                    TrackLabel(view.room.track).c_str());
    }
    if (!view.lotteryChecked) {
        ImGui::TextColored(kRed, "This copy drew a different result from the same votes. The host's result stands.");
    }
    DrawTimer(view.room);
    if (isHost && ImGui::Button("Skip##lanlottery")) {
        gdx::lan::SkipLottery();
    }
}

void DrawPickPhase(const View& view, const PlayerInfo& me) {
    const bool mine = me.id == view.room.picker;
    if (mine) {
        ImGui::TextUnformatted("You finished last, so you pick the next track.");
    } else {
        ImGui::Text("%s finished last and picks the next track.", PlayerName(view, view.room.picker).c_str());
    }
    DrawTimer(view.room);
    if (!mine) {
        return;
    }
    const int chosen = DrawTrackGrid(view, true, false);
    if (chosen != kNoChoice) {
        gdx::lan::Pick(static_cast<uint8_t>(chosen));
    }
}

void DrawMachinePhase(const View& view, const PlayerInfo& me) {
    ImGui::Text("Next track: %s", TrackLabel(view.room.track).c_str());
    DrawTimer(view.room);
    ImGui::Text("Locked in: %u / %u", static_cast<unsigned>(view.room.locked), static_cast<unsigned>(view.room.racers));
    if (!me.inRace) {
        ImGui::TextDisabled("You watch this race.");
        return;
    }
    if (me.locked) {
        ImGui::TextColored(kGreen, "Please wait");
        return;
    }
    if (sLockRace != view.room.raceIndex) {
        sLockRace = view.room.raceIndex;
        sLockMachine = me.machine < kRosterMachines ? me.machine : 0;
        sLockSkin = me.skin < kMachineSkins ? me.skin : 0;
    }
    // The main menu opens the game's own machine select for this state (gdx_net_machine_select.h) and puts
    // this menu away. The controls below are for a player who is on any other screen.
    ImGui::TextDisabled("From the main menu the game opens machine select. Anywhere else, lock in here.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Machine##lanlock", MachineLabel(static_cast<uint8_t>(sLockMachine)).c_str())) {
        for (int i = 0; i < static_cast<int>(kRosterMachines); i++) {
            const std::string item = MachineLabel(static_cast<uint8_t>(i)) + "##machine" + std::to_string(i);
            if (ImGui::Selectable(item.c_str(), i == sLockMachine)) {
                sLockMachine = i;
            }
        }
        ImGui::EndCombo();
    }
    int skin = sLockSkin + 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::SliderInt("Colour##lanlock", &skin, 1, static_cast<int>(kMachineSkins))) {
        sLockSkin = skin - 1;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::SliderInt("Engine##lanlock", &sLockEngine, 0, kEngineMax, "%d%%");
    ImGui::SetItemTooltip("The machine settings slider: acceleration at one end, top speed at the other.");
    if (ImGui::Button("Lock in##lanlock")) {
        LockRequest lock;
        lock.machine = static_cast<uint8_t>(sLockMachine);
        lock.skin = static_cast<uint8_t>(sLockSkin);
        lock.engine = static_cast<uint8_t>(sLockEngine);
        gdx::lan::Lock(lock);
    }
    ImGui::SetItemTooltip("Final for this race. A racer who has not locked in when the time runs out races the\n"
                          "machine shown in the player list.");
}

const char* PhaseTitle(Phase phase) {
    switch (phase) {
    case Phase::Lobby:
        return "Lobby";
    case Phase::Voting:
        return "Track vote";
    case Phase::Lottery:
        return "Vote result";
    case Phase::Pick:
        return "Track pick";
    case Phase::Machine:
        return "Machine select";
    case Phase::GridSync:
        return "Loading the track";
    case Phase::Countdown:
        return "Race start";
    case Phase::Racing:
        return "Race";
    case Phase::Results:
        return "Results";
    }
    return "Room";
}

// A race time the way the game writes one: 1'23"456.
std::string RaceTime(uint32_t ms) {
    char text[24];
    std::snprintf(text, sizeof(text), "%u'%02u\"%03u", static_cast<unsigned>(ms / 60000),
                  static_cast<unsigned>((ms / 1000) % 60), static_cast<unsigned>(ms % 1000));
    return text;
}

// The host's table of the race: who finished and when, who is out, who is still driving. While the race
// runs only a finisher has a place; once it is over everyone has one, and the points it earned.
void DrawResults(const View& view) {
    const RaceResults& table = view.results;
    if (!view.haveResults || table.raceIndex != view.room.raceIndex || table.entries.empty()) {
        if (view.room.phase == Phase::Racing) {
            ImGui::TextDisabled("Nobody has finished yet.");
        }
        return;
    }
    if (!ImGui::BeginTable("##lanresults", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Place", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Series", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    unsigned place = 0;
    for (const ResultEntry& entry : table.entries) {
        place++;
        const PlayerInfo* player = FindPlayer(view, entry.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (table.final || entry.kind == ResultKind::Finished) {
            ImGui::Text("%u", place);
        }
        ImGui::TableNextColumn();
        if (entry.id == view.localId) {
            ImGui::TextColored(kGold, "%s", PlayerName(view, entry.id).c_str());
        } else {
            ImGui::TextUnformatted(PlayerName(view, entry.id).c_str());
        }
        ImGui::TableNextColumn();
        switch (entry.kind) {
        case ResultKind::Finished:
            ImGui::TextUnformatted(RaceTime(entry.timeMs).c_str());
            break;
        case ResultKind::Unfinished:
            ImGui::TextDisabled("%s", table.final ? "Did not finish" : "Racing");
            break;
        case ResultKind::Retired:
            ImGui::TextColored(kRed, "Retired");
            break;
        }
        ImGui::TableNextColumn();
        if (table.final) {
            ImGui::Text("+%u", static_cast<unsigned>(entry.points));
        }
        ImGui::TableNextColumn();
        if (player != nullptr) {
            ImGui::Text("%u", static_cast<unsigned>(player->points));
        }
    }
    ImGui::EndTable();
}

void DrawPhase(const View& view, const PlayerInfo& me, bool isHost) {
    ImGui::SeparatorText(PhaseTitle(view.room.phase));
    switch (view.room.phase) {
    case Phase::Lobby:
        DrawLobbyPhase(view, me, isHost);
        break;
    case Phase::Voting:
        DrawVotingPhase(view, me);
        break;
    case Phase::Lottery:
        DrawLotteryPhase(view, isHost);
        break;
    case Phase::Pick:
        DrawPickPhase(view, me);
        break;
    case Phase::Machine:
        DrawMachinePhase(view, me);
        break;
    case Phase::GridSync:
        ImGui::Text("Track: %s", TrackLabel(view.room.track).c_str());
        DrawTimer(view.room);
        break;
    case Phase::Countdown:
        ImGui::Text("Track: %s", TrackLabel(view.room.track).c_str());
        DrawTimer(view.room);
        break;
    case Phase::Racing:
        ImGui::Text("Track: %s", TrackLabel(view.room.track).c_str());
        DrawTimer(view.room);
        DrawResults(view);
        break;
    case Phase::Results:
        ImGui::Text("The race on %s is over.", TrackLabel(view.room.track).c_str());
        DrawTimer(view.room);
        DrawResults(view);
        if (isHost) {
            if (ImGui::Button("Back to the lobby##lanlobby")) {
                gdx::lan::EndSeries();
            }
            ImGui::SetItemTooltip("Ends the series. Otherwise the next track choice starts when the time runs out.");
        }
        break;
    }
    // A watcher may ask for a race seat at any time; the lobby panel already draws the box.
    if (me.watcher && view.room.phase != Phase::Lobby) {
        bool next = me.raceNext;
        if (ImGui::Checkbox("Race next##lanphase", &next)) {
            gdx::lan::RaceNext(next);
        }
        ImGui::SetItemTooltip("Take a free race seat when the next race is set up. First come, first served.");
    }
}

void DrawMessages(const View& view) {
    ImGui::SeparatorText("Messages");
    const float height = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
    if (ImGui::BeginChild("##lanlog", ImVec2(-1.0f, height), ImGuiChildFlags_Borders)) {
        for (const ShownLine& line : view.lines) {
            if (line.from == kSystemId) {
                ImGui::TextDisabled("%s", line.text.c_str());
                continue;
            }
            if (gdx::lan::Muted(line.from)) {
                continue;
            }
            ImGui::TextColored(Colour(line.colour), "%s:", line.name.empty() ? "?" : line.name.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(line.text.c_str());
        }
        if (view.lineSerial != sSeenSerial) {
            sSeenSerial = view.lineSerial;
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    for (uint8_t emote = 0; emote < kEmoteCount; emote++) {
        if (emote % 4 != 0) {
            ImGui::SameLine();
        }
        const std::string label = std::string(EmoteText(emote)) + "##emote" + std::to_string(emote);
        if (ImGui::Button(label.c_str())) {
            gdx::lan::Emote(emote);
        }
    }
}

void DrawRoom(const View& view, bool isHost) {
    if (isHost) {
        ImGui::Text("Hosting a room on port %u. Players on your network find it under Find rooms.",
                    static_cast<unsigned>(view.port));
    } else if (view.mode == Mode::Joining) {
        ImGui::Text("Joining %s...", view.hostText.c_str());
    } else {
        ImGui::Text("In the room at %s.", view.hostText.c_str());
    }
    if (view.mode == Mode::Joining) {
        if (ImGui::Button("Cancel##lanlobby")) {
            gdx::lan::Stop();
        }
        return;
    }
    const size_t racers = CountRacers(view);
    ImGui::TextDisabled("%u of %u race seats, %u of %u watch seats", static_cast<unsigned>(racers),
                        static_cast<unsigned>(view.room.racerCap), static_cast<unsigned>(view.roster.size() - racers),
                        static_cast<unsigned>(kMaxWatchers));

    DrawRoster(view, isHost);
    // The welcome arrives a moment before the first roster.
    if (const PlayerInfo* me = FindPlayer(view, view.localId)) {
        DrawPhase(view, *me, isHost);
    }
    DrawMessages(view);
    ImGui::Spacing();
    if (ImGui::Button(isHost ? "Close the room##lanlobby" : "Leave the room##lanlobby")) {
        gdx::lan::Stop();
    }
    if (isHost) {
        ImGui::SetItemTooltip("Everyone in the room is sent back to the lobby page.");
    }
}

void DrawRooms(const View& view, const LocalProfile& me) {
    const std::string build = gdx::lan::BuildId();
    if (view.rooms.empty()) {
        ImGui::TextDisabled("Searching... Rooms hosted on this network show up here.");
        return;
    }
    if (!ImGui::BeginTable("##lanrooms", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Racers", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Watchers", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##join", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < view.rooms.size(); i++) {
        const auto& found = view.rooms[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(found.room.hostName.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u / %u", static_cast<unsigned>(found.room.racers), static_cast<unsigned>(found.room.racerCap));
        ImGui::TableNextColumn();
        ImGui::Text("%u / %u", static_cast<unsigned>(found.room.watchers), static_cast<unsigned>(kMaxWatchers));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(found.room.phase == Phase::Lobby ? "In the lobby" : "Racing");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(gdx::net::FormatAddress(found.address).c_str());
        ImGui::TableNextColumn();
        const bool sameBuild = found.room.build == build;
        ImGui::BeginDisabled(!sameBuild || !found.room.RaceSeatFree());
        if (ImGui::SmallButton("Join")) {
            gdx::lan::StartJoin(found.address, me, Seat::Race);
        }
        ImGui::EndDisabled();
        if (!sameBuild) {
            ImGui::SetItemTooltip("This room runs G-Diffuser %s. You have %s.", found.room.build.c_str(),
                                  build.c_str());
        } else if (!found.room.RaceSeatFree()) {
            ImGui::SetItemTooltip("Every race seat is taken.");
        } else if (found.room.phase != Phase::Lobby) {
            ImGui::SetItemTooltip("The room is mid-series. You race from the next race on.");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!sameBuild || !found.room.WatchSeatFree());
        if (ImGui::SmallButton("Watch")) {
            gdx::lan::StartJoin(found.address, me, Seat::Watch);
        }
        ImGui::EndDisabled();
        if (sameBuild && !found.room.WatchSeatFree()) {
            ImGui::SetItemTooltip("Both watch seats are taken.");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawStartPage(const View& view, const LocalProfile& me) {
    ImGui::TextWrapped("Hosting, searching or joining shares your profile name, colour and favourite machine, "
                       "and the preset messages you send, with the players in the room. Players on your network "
                       "can see your IP address. Nothing is sent until you press one of the buttons below.");
    ImGui::Spacing();

    ImGui::SeparatorText("Host a room");
    if (sPortEdit < 0) {
        sPortEdit = PortSetting();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::InputInt("Port##lanport", &sPortEdit, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (sPortEdit >= 1024 && sPortEdit <= 65535) {
            gdx::lan::SavePortSetting(static_cast<uint16_t>(sPortEdit));
        } else {
            sPortEdit = PortSetting();
        }
    }
    ImGui::SetItemTooltip("The port this copy listens on when it hosts, from 1024 to 65535. %u by default.\n"
                          "Players joining by address type it after the IP, as in 192.168.1.20:%u.",
                          static_cast<unsigned>(kDefaultPort), static_cast<unsigned>(kDefaultPort));
    ImGui::SameLine();
    if (ImGui::Button("Host a room##lanlobby")) {
        sHostError.clear();
        if (!gdx::lan::StartHost(PortSetting(), me, sHostError)) {
            gdx::lan::Dismiss();
        }
    }
    if (!sHostError.empty()) {
        ImGui::TextColored(kRed, "%s", sHostError.c_str());
    }

    ImGui::SeparatorText("Find rooms");
    if (view.mode == Mode::Browsing) {
        if (ImGui::Button("Stop searching##lanlobby")) {
            gdx::lan::Stop();
        }
        DrawRooms(view, me);
    } else if (ImGui::Button("Search the network##lanlobby")) {
        gdx::lan::StartBrowse(PortSetting());
    }

    ImGui::SeparatorText("Join by address");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##lanaddress", "192.168.1.20:24302", sAddress, sizeof(sAddress),
                             ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    std::string text = sAddress;
    if (!text.empty() && text.find(':') == std::string::npos) {
        text += ":" + std::to_string(kDefaultPort);
    }
    gdx::net::NetAddress address;
    const bool valid = gdx::net::ParseAddress(text.c_str(), address) && address.port != 0;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Join##lanaddress")) {
        gdx::lan::StartJoin(address, me, Seat::Race);
    }
    ImGui::SameLine();
    if (ImGui::Button("Watch##lanaddress")) {
        gdx::lan::StartJoin(address, me, Seat::Watch);
    }
    ImGui::EndDisabled();
    if (!text.empty() && !valid) {
        ImGui::TextDisabled("Type an IPv4 address, with :port when the host changed it.");
    }
}

} // namespace

void GdxLanLobbyPage_Draw() {
    LocalProfile me;
    const View view = gdx::lan::Snapshot();
    const bool inSession = view.mode == Mode::Hosting || view.mode == Mode::Joining || view.mode == Mode::Joined ||
                           view.mode == Mode::Closed;
    if (!inSession && !ActiveProfile(me)) {
        ImGui::TextWrapped("Make a profile in Online > Profile first. The room shows its name and colour.");
        return;
    }
    switch (view.mode) {
    case Mode::Hosting:
        DrawRoom(view, true);
        break;
    case Mode::Joining:
    case Mode::Joined:
        DrawRoom(view, false);
        break;
    case Mode::Closed:
        ImGui::TextColored(kRed, "%s", view.status.empty() ? "The room closed." : view.status.c_str());
        if (ImGui::Button("Back##lanlobby")) {
            gdx::lan::Dismiss();
        }
        break;
    case Mode::Idle:
    case Mode::Browsing:
        DrawStartPage(view, me);
        break;
    }
}
