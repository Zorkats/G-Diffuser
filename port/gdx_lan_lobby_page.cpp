// port/gdx_lan_lobby_page.cpp -- see gdx_lan_lobby_page.h.
//
// The page draws from a copy of the lobby state (gdx::lan::Snapshot) and sends the player's actions to the
// worker, so a slow frame never stalls the room. The chat filter runs here, on the receiving side, so each
// player chooses it for themselves; the host only cleans and rate-limits lines. Muting a player is local and
// lasts while they stay in the room.

#include "gdx_lan_lobby_page.h"

#include "gdx_lan_lobby.h"
#include "gdx_profile_state.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

extern "C" const char* gMachineNames[];

namespace {

using gdx::lan::Mode;
using namespace gdx::net::lobby;

const ImVec4 kRed = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
const ImVec4 kGreen = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
const char* const kPortCvar = "gEnhancements.Online.LanPort";
const char* const kFilterCvar = "gEnhancements.Online.ChatFilter";
const char* const kNoRacing = "Online racing comes in a later update.";

char sAddress[64] = "";
char sChat[kChatMax + 1] = "";
std::string sHostError;
std::set<uint8_t> sMuted;
uint32_t sSeenSerial = 0;
bool sReady = false;
int sPortEdit = -1; // the Port field while the player types; -1 until first drawn
bool sFocusChat = false;
bool sFilterLoaded = false;
std::vector<std::string> sFilterWords;

ImVec4 Colour(uint32_t rgb) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

std::string MachineLabel(uint8_t machine) {
    if (machine >= GDX_PROFILE_ROSTER_COUNT || gMachineNames[machine] == nullptr) {
        return "No machine";
    }
    std::string label = gMachineNames[machine];
    bool start = true;
    for (char& c : label) {
        if (start) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        start = c == ' ';
    }
    return label;
}

// The player's own blocked words: chat-filter.txt in the data folder, one per line, '#' starts a comment.
void LoadFilterWords() {
    if (sFilterLoaded) {
        return;
    }
    sFilterLoaded = true;
    const std::filesystem::path path = std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "chat-filter.txt";
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line) && sFilterWords.size() < 1000) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        sFilterWords.push_back(line.substr(first));
    }
}

bool ActiveProfile(LocalProfile& out) {
    const GdxProfileStore* store = gdx_profile_state_store();
    if (store->count <= 0 || store->active < 0 || store->active >= store->count) {
        return false;
    }
    const GdxProfile& p = store->profiles[store->active];
    out.name = p.name;
    out.colour = p.colour & 0xFFFFFFu;
    out.machine = (p.preferredMachine >= 0 && p.preferredMachine < GDX_PROFILE_ROSTER_COUNT) ? static_cast<uint8_t>(p.preferredMachine)
                                                                       : static_cast<uint8_t>(0xFF);
    return NameOk(out.name);
}

uint16_t PortSetting() {
    const int port = CVarGetInteger(kPortCvar, kDefaultPort);
    return static_cast<uint16_t>((port >= 1024 && port <= 65535) ? port : kDefaultPort);
}

void DrawStart(const gdx::lan::View& view) {
    ImGui::BeginDisabled(true);
    ImGui::Button("Start race##lanlobby");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", kNoRacing);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kNoRacing);
    (void) view;
}

void DrawRoster(const gdx::lan::View& view, bool isHost) {
    // Forget mutes of players who left, so a newcomer with the same id starts unmuted.
    for (auto it = sMuted.begin(); it != sMuted.end();) {
        bool present = false;
        for (const PlayerInfo& p : view.roster) {
            present = present || p.id == *it;
        }
        it = present ? std::next(it) : sMuted.erase(it);
    }
    const int columns = isHost ? 5 : 4;
    if (!ImGui::BeginTable("##lanroster", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Machine", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthFixed);
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
        ImGui::TextUnformatted(MachineLabel(p.machine).c_str());
        ImGui::TableNextColumn();
        if (p.ready) {
            ImGui::TextColored(kGreen, "Ready");
        } else {
            ImGui::TextDisabled("Not ready");
        }
        ImGui::TableNextColumn();
        if (p.id != view.localId) {
            bool muted = sMuted.count(p.id) != 0;
            if (ImGui::Checkbox("##mute", &muted)) {
                if (muted) {
                    sMuted.insert(p.id);
                } else {
                    sMuted.erase(p.id);
                }
            }
            ImGui::SetItemTooltip("Hide this player's chat on your screen only.");
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

void DrawChat(const gdx::lan::View& view) {
    LoadFilterWords();
    bool filter = CVarGetInteger(kFilterCvar, 1) != 0;
    if (ImGui::Checkbox("Filter bad words##lanchat", &filter)) {
        CVarSetInteger(kFilterCvar, filter ? 1 : 0);
        CVarSave();
    }
    ImGui::SetItemTooltip("Masks common swear words and slurs in chat you receive. Add your own words to\n"
                          "chat-filter.txt in the G-Diffuser data folder, one per line, then restart.");

    const float height = ImGui::GetTextLineHeightWithSpacing() * 9.0f;
    if (ImGui::BeginChild("##lanchatlog", ImVec2(-1.0f, height), ImGuiChildFlags_Borders)) {
        for (const ShownLine& line : view.chat) {
            if (line.from != kSystemId && sMuted.count(line.from) != 0) {
                continue;
            }
            const std::string text = filter ? FilterChat(line.text, sFilterWords) : line.text;
            if (line.from == kSystemId) {
                ImGui::TextDisabled("%s", text.c_str());
                continue;
            }
            ImGui::TextColored(Colour(line.colour), "%s:", line.name.empty() ? "?" : line.name.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", text.c_str());
        }
        if (view.chatSerial != sSeenSerial) {
            sSeenSerial = view.chatSerial;
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (sFocusChat) {
        ImGui::SetKeyboardFocusHere();
        sFocusChat = false;
    }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Send").x - ImGui::GetStyle().FramePadding.x * 2.0f -
                            ImGui::GetStyle().ItemSpacing.x);
    const bool entered =
        ImGui::InputTextWithHint("##lanchatinput", "Say something", sChat, sizeof(sChat),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool clicked = ImGui::Button("Send##lanchat");
    if ((entered || clicked) && !SanitizeChat(sChat).empty()) {
        gdx::lan::Chat(sChat);
        sChat[0] = '\0';
        sFocusChat = entered;
    }
}

void DrawRoom(const gdx::lan::View& view, bool isHost) {
    if (isHost) {
        ImGui::Text("Hosting a room on port %u. Players on your network find it under Find rooms.",
                    static_cast<unsigned>(view.port));
    } else if (view.mode == Mode::Joining) {
        ImGui::Text("Joining %s...", view.hostText.c_str());
    } else {
        ImGui::Text("In the room at %s.", view.hostText.c_str());
    }
    ImGui::TextDisabled("%u of %u players", static_cast<unsigned>(view.roster.size()),
                        static_cast<unsigned>(kMaxPlayers));
    if (view.mode == Mode::Joining) {
        if (ImGui::Button("Cancel##lanlobby")) {
            gdx::lan::Stop();
        }
        return;
    }

    DrawRoster(view, isHost);
    if (ImGui::Checkbox("Ready##lanlobby", &sReady)) {
        gdx::lan::SetReady(sReady);
    }
    ImGui::SameLine();
    DrawStart(view);
    ImGui::Spacing();
    DrawChat(view);
    ImGui::Spacing();
    if (ImGui::Button(isHost ? "Close the room##lanlobby" : "Leave the room##lanlobby")) {
        gdx::lan::Stop();
    }
    if (isHost) {
        ImGui::SetItemTooltip("Everyone in the room is sent back to the lobby page.");
    }
}

void DrawRooms(const gdx::lan::View& view, const LocalProfile& me) {
    const std::string build = gdx::lan::BuildId();
    if (view.rooms.empty()) {
        ImGui::TextDisabled("Searching... Rooms hosted on this network show up here.");
        return;
    }
    if (!ImGui::BeginTable("##lanrooms", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed);
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
        ImGui::Text("%u / %u", static_cast<unsigned>(found.room.players), static_cast<unsigned>(found.room.cap));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(gdx::net::FormatAddress(found.address).c_str());
        ImGui::TableNextColumn();
        const bool sameBuild = found.room.build == build;
        ImGui::BeginDisabled(!sameBuild || !found.room.open);
        if (ImGui::SmallButton("Join")) {
            gdx::lan::StartJoin(found.address, me);
        }
        ImGui::EndDisabled();
        if (!sameBuild) {
            ImGui::SetItemTooltip("This room runs G-Diffuser %s. You have %s.", found.room.build.c_str(),
                                  build.c_str());
        } else if (!found.room.open) {
            ImGui::SetItemTooltip("This room is full.");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawStartPage(const gdx::lan::View& view, const LocalProfile& me) {
    ImGui::TextWrapped("Hosting, searching or joining shares your profile name, colour and favourite machine, "
                       "and what you write in chat, with the players in the room. Players on your network can "
                       "see your IP address. Nothing is sent until you press one of the buttons below.");
    ImGui::Spacing();

    ImGui::SeparatorText("Host a room");
    if (sPortEdit < 0) {
        sPortEdit = PortSetting();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::InputInt("Port##lanport", &sPortEdit, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (sPortEdit >= 1024 && sPortEdit <= 65535) {
            CVarSetInteger(kPortCvar, sPortEdit);
            CVarSave();
        } else {
            sPortEdit = PortSetting();
        }
    }
    ImGui::SetItemTooltip("The port this copy listens on when it hosts, from 1024 to 65535. %u by default.\n"
                          "Players joining by address type it after the IP, as in 192.168.1.20:%u.",
                          static_cast<unsigned>(kDefaultPort), static_cast<unsigned>(kDefaultPort));
    ImGui::SameLine();
    if (ImGui::Button("Host a room##lanlobby")) {
        sReady = false;
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
        sReady = false;
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
        sReady = false;
        gdx::lan::StartJoin(address, me);
    }
    ImGui::EndDisabled();
    if (!text.empty() && !valid) {
        ImGui::TextDisabled("Type an IPv4 address, with :port when the host changed it.");
    }
}

} // namespace

void GdxLanLobbyPage_Draw() {
    LocalProfile me;
    const gdx::lan::View view = gdx::lan::Snapshot();
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
