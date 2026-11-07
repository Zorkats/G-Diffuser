// port/gdx_lan_lobby.h -- the game's one O2 LAN lobby session: a worker thread that owns the UDP socket
// and one of the room browser, the host or the joining client (net/gdx_net_lobby_session.h).
//
// The menu page calls these from the ImGui thread. Each call returns at once: the worker reads the
// socket and runs the timers every 10 ms, so the room stays alive while the menu is closed. The page reads
// a copy of the state with Snapshot(). Only one session runs at a time; starting one stops the last.
//
// Nothing here opens a socket until the player hosts, searches or joins.

#ifndef GDX_LAN_LOBBY_H
#define GDX_LAN_LOBBY_H

#include "net/gdx_net_lobby_session.h"

#include <cstdint>
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
    std::vector<net::lobby::ShownLine> chat;
    uint32_t chatSerial = 0; // grows with every new line, so the page knows when to scroll
    std::vector<net::lobby::LanBrowser::FoundRoom> rooms;
};

// The build id peers must match: the version, and the commit when the build has one.
std::string BuildId();

// Searches the network for rooms on port (and on the default port when port is another one).
void StartBrowse(uint16_t port);
// Binds port on every interface. false with a reason in error when the port is taken.
bool StartHost(uint16_t port, const net::lobby::LocalProfile& me, std::string& error);
void StartJoin(const net::NetAddress& host, const net::lobby::LocalProfile& me);

void SetReady(bool ready);
void Chat(const std::string& text);
void Kick(uint8_t id);
// Leaves or closes the room, or stops searching, and goes back to Idle.
void Stop();
// From Closed back to Idle, keeping nothing.
void Dismiss();

View Snapshot();

// Stops the worker at exit, saying goodbye to the room.
void Shutdown();

} // namespace gdx::lan

#endif // GDX_LAN_LOBBY_H
