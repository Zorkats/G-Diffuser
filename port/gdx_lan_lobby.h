// port/gdx_lan_lobby.h -- the game's one LAN room session: a worker thread that owns the UDP socket and one
// of the room browser, the host or the joining client (net/gdx_net_lobby_session.h).
//
// The menu page calls these from the ImGui thread. Each call returns at once: the worker reads the
// socket and runs the timers every 10 ms, so the room stays alive while the menu is closed. The page reads
// a copy of the state with Snapshot(). Only one session runs at a time; starting one stops the last.
//
// Nothing here opens a socket until the player hosts, searches or joins.

#ifndef GDX_LAN_LOBBY_H
#define GDX_LAN_LOBBY_H

#include "gdx_net_online.h"
#include "net/gdx_net_directory.h"
#include "net/gdx_net_lobby_session.h"
#include "net/gdx_net_track_pack.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gdx::lan {

enum class Mode : uint8_t {
    Idle,      // no session, no socket
    Browsing,  // probing the network for rooms
    Hosting,   // this copy hosts a room
    Joining,   // waiting for the host to answer
    Joined,    // in someone else's room
    Closed,    // a session ended on its own; status says why
};

struct View {
    Mode mode = Mode::Idle;
    std::string status;   // why the last session ended, or why hosting failed
    uint16_t port = 0;    // the host's port while hosting, the room's port while joining
    std::string hostText; // "192.168.1.20:24302" while joining or joined
    uint8_t localId = net::lobby::kSystemId;
    std::vector<net::lobby::PlayerInfo> roster;
    net::lobby::RoomState room; // the phase timer is run down to the worker's last tick
    std::vector<net::lobby::Ballot> votes;
    net::lobby::LotteryResult lottery;
    bool lotteryChecked = true; // false when a client's own draw disagrees with the host's
    net::lobby::Picked picked;
    net::lobby::Grid grid; // the race being loaded or run; its race index says which
    bool haveGrid = false;
    net::lobby::StartSignal start; // when that race starts, on the host clock
    bool haveStart = false;
    net::lobby::RaceResults results; // the table of a race, being run or over; its race index says which
    bool haveResults = false;
    net::lobby::TrackProgress trackProgress; // this copy's hold on the room's custom track
    std::shared_ptr<const net::transfer::TrackBuffer> raceTrack; // that track, once verified; null for a stock track
    uint32_t clockOffsetMs = 0; // the host clock minus ours; 0 on the host
    bool clockReady = false;
    std::vector<net::lobby::ShownLine> lines;
    uint32_t lineSerial = 0; // grows with every new line, so the page knows when to scroll
    std::vector<net::lobby::LanBrowser::FoundRoom> rooms;
    // A directory room (O5.10): how its listing goes, on the host. The code itself is room.code.
    online::ListView listing;
    // Joining by code: the code asked for, "ABC-DEF", while the directory is asked and the room tried.
    std::string joinCode;
    // The friend keys of the room's members who proved one (O7.3), the host's included.
    std::vector<net::lobby::MemberKey> keys;
};

// A player of this copy's last room who proved a friend key (O7.4): what "Add from the last room" lists.
struct RoomMember {
    std::string name;
    net::lobby::Bytes32 key{};
};

// The build id peers must match: the version, and the commit when the build has one.
std::string BuildId();

// The active profile as a room shows it. false when there is none or its name is not one a room takes.
bool ActiveProfile(net::lobby::LocalProfile& out);
// The port this copy hosts on and searches: the saved setting, or the default when that is out of range.
uint16_t PortSetting();
// Saves it. port is 1024 to 65535.
void SavePortSetting(uint16_t port);

// Searches the network for rooms on port (and on the default port when port is another one).
void StartBrowse(uint16_t port);
// Binds port on every interface. false with a reason in error when the port is taken.
bool StartHost(uint16_t port, const net::lobby::LocalProfile& me, std::string& error);
// A room of a type (O6.1). Any type but Lan is listed in the room directory under a code, from the same socket.
bool StartHost(uint16_t port, const net::lobby::LocalProfile& me, net::lobby::RoomType type, std::string& error);
void StartJoin(const net::NetAddress& host, const net::lobby::LocalProfile& me, net::lobby::Seat seat);
// Asks the directory for the room with this code value, then tries its addresses in order (O5.10).
void StartJoinCode(uint32_t code, const net::lobby::LocalProfile& me, net::lobby::Seat seat);
// Joins a room the directory already gave: from the Open list, Quick race or a friend's rooms (O6.5, O7.4).
void StartJoinRecord(const net::directory::Record& record, const net::lobby::LocalProfile& me,
                     net::lobby::Seat seat);

// The player's own actions, as host or as client. The room decides; the answer shows in the next Snapshot.
void SetReady(bool ready);
void Vote(uint8_t track);
void Pick(uint8_t track);
// A ballot or a pick for a track of this copy's. Its bytes go to KeepOwnTrack first, so the room can fetch it.
void VoteCustom(const net::lobby::CustomTrack& track);
void PickCustom(const net::lobby::CustomTrack& track);
// Keeps the content bytes of a track of this copy's for the room's store, which looks there before the
// download cache. The last six stay: one Edit Cup.
void KeepOwnTrack(const net::track::TrackId& id, std::vector<uint8_t> bytes);
void Lock(const net::lobby::LockRequest& lock);
void Emote(uint8_t emote);
void RaceNext(bool raceNext);

// The host's room controls. They do nothing on a client.
void StartSeries();
void SkipLottery();
void EndSeries();
void SetRacerCap(uint8_t cap);
void SetCustomTracks(bool on);
// CPUs fill the grid slots the humans leave free (D2).
void SetCpuFill(bool on);
void Kick(uint8_t id);
// Removes the player and refuses their friend key for the rest of the room (O7.3). A player who joined without a
// key is only removed.
void Ban(uint8_t id);

// The players with a friend key met in this copy's last room, or in the room it is in, each once, under the name
// they last had. The first other player of a new room starts the list again; leaving a room, a refused join and
// a room nobody else enters keep it.
std::vector<RoomMember> LastRoom();

// Muted players. A mute is this copy's own: the player's messages are not shown here, and nothing about it
// travels. It ends when the player leaves, or when a handover gives the id to someone else (a newcomer, or the
// successor that becomes id 0), so whoever is given that id next starts unmuted. Game
// thread only. None of the three takes the session's lock, so they may be called inside a Read.
bool Muted(uint8_t id);
void SetMuted(uint8_t id, bool muted);
void ForgetMutesOutside(const View& view);

// Leaves or closes the room, or stops searching, and goes back to Idle.
void Stop();
// From Closed back to Idle, keeping nothing.
void Dismiss();

View Snapshot();
// Runs read on the state without copying it, for a caller that reads every frame. The worker waits
// meanwhile, so read must be short and must not call back into this module.
void Read(const std::function<void(const View&)>& read);

// Developer switch for scripted runs of two copies on one computer. Called every frame by the menu.
//   GDX_NET_DEV=host:<port>   hosts a room on that port as soon as the game is up, and marks itself ready
//   GDX_NET_DEV=join:<port>   joins the room on this computer at that port, and marks itself ready
// Two ready racers start a series, so the copies reach the lock-in screen with no hand on the menu.
void DevFrame();

// Stops the worker at exit, saying goodbye to the room.
void Shutdown();

} // namespace gdx::lan

#endif // GDX_LAN_LOBBY_H
