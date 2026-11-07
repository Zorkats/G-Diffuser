// port/net/gdx_net_lobby_session.h: the O2 LAN lobby host, the joining client and the LAN room browser.
//
// None of these classes opens a socket or reads a clock. The owner passes a send callback that takes an
// address, feeds every received datagram to Receive(), and calls Update() with the current time in
// milliseconds once per tick. That keeps them testable on a fake network and a fake clock, as the channel
// is. One thread owns each object and makes every call. The send callback must not call back in.
//
// LobbyHost answers discovery probes and keeps one server Channel per peer address (gdx_net_channel.h).
// A peer is a player only after its Join passes: same build, a valid profile and a free seat. The host
// is player 0 and owns the roster; every change goes out as a whole Roster message. Chat goes through the
// host, which sanitizes it, limits each player to kChatBurst lines per kChatWindowMs, and relays it.
//
// Racing is not part of O2: the lobby has no start message.

#ifndef GDX_NET_LOBBY_SESSION_H
#define GDX_NET_LOBBY_SESSION_H

#include "gdx_net_channel.h"
#include "gdx_net_lobby.h"
#include "gdx_net_socket.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gdx::net::lobby {

using DatagramSend = std::function<void(const NetAddress& to, const uint8_t* data, size_t len)>;

constexpr size_t kChatLogCap = 200;      // lines kept for the view
constexpr size_t kChatBurst = 5;         // lines one player may send per window
constexpr uint32_t kChatWindowMs = 5000;
constexpr size_t kPendingPeerCap = 16;   // addresses mid-handshake at once
constexpr uint32_t kPendingPeerMs = 5000; // a peer that never joins is dropped after this
constexpr uint32_t kRejectLingerMs = 400; // time for a Reject to reach the peer before the link drops
constexpr uint32_t kProbeEveryMs = 1500;
constexpr uint32_t kRoomExpireMs = 5000;
constexpr size_t kRoomListCap = 32;

struct LocalProfile {
    std::string name;
    uint32_t colour = 0;
    uint8_t machine = 0xFF;
};

// A chat line ready to show. from is a player id or kSystemId; name and colour are copied from the roster
// when the line arrives, so a line keeps its sender after the sender leaves.
struct ShownLine {
    uint8_t from = kSystemId;
    std::string name;
    uint32_t colour = 0;
    std::string text;
};

class LobbyHost {
public:
    LobbyHost(std::string build, LocalProfile me, uint16_t port, DatagramSend send);
    ~LobbyHost();

    LobbyHost(const LobbyHost&) = delete;
    LobbyHost& operator=(const LobbyHost&) = delete;

    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    void SetReady(bool ready);
    // Sanitized and relayed like a player's line. Returns false when nothing printable is left.
    bool Chat(const std::string& text);
    void Kick(uint8_t id, uint32_t nowMs);
    // Says goodbye to every peer. The host takes no more calls after this except the getters.
    void Close(uint32_t nowMs);

    const std::vector<PlayerInfo>& Roster() const {
        return roster_;
    }

    const std::deque<ShownLine>& ChatLog() const {
        return chat_;
    }

    // Counts every message the host refused or could not decode, for the view and the tests.
    uint32_t Refused() const {
        return refused_;
    }

    size_t PeerCount() const {
        return peers_.size();
    }

private:
    struct Peer;

    void HandleMessage(Peer& peer, const Message& msg, uint32_t nowMs);
    void HandleJoin(Peer& peer, const Message& msg, uint32_t nowMs);
    void Reject(Peer& peer, RejectReason reason, uint32_t nowMs);
    void Relay(const ChatLine& line);
    void Notice(const std::string& text);
    void BroadcastRoster();
    void SendToPlayers(uint8_t type, const std::vector<uint8_t>& payload);
    void RemovePlayer(uint8_t id);
    uint8_t FreeId() const;
    PlayerInfo* FindPlayer(uint8_t id);
    void Log(const ChatLine& line);

    std::string build_;
    LocalProfile me_;
    uint16_t port_;
    DatagramSend send_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<PlayerInfo> roster_;
    std::deque<ShownLine> chat_;
    uint32_t refused_ = 0;
    bool closed_ = false;
};

class LobbyClient {
public:
    enum class State : uint8_t { Connecting, Joined, Closed };

    // nonce: a fresh random value for this attempt (ChannelConfig::connectionNonce).
    LobbyClient(std::string build, LocalProfile me, NetAddress host, uint32_t nonce, DatagramSend send);

    void Start(uint32_t nowMs);
    // Datagrams from any address other than the host are ignored.
    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    void SetReady(bool ready);
    bool Chat(const std::string& text);
    void Leave(uint32_t nowMs);

    State GetState() const {
        return state_;
    }

    // Why the client is Closed, in words for the player. Empty while open or after Leave().
    const std::string& CloseText() const {
        return closeText_;
    }

    uint8_t LocalId() const {
        return localId_;
    }

    const NetAddress& Host() const {
        return host_;
    }

    const std::vector<PlayerInfo>& Roster() const {
        return roster_;
    }

    const std::deque<ShownLine>& ChatLog() const {
        return chat_;
    }

    uint32_t Refused() const {
        return refused_;
    }

private:
    void HandleMessage(const Message& msg);
    void CloseWith(const std::string& text);

    std::string build_;
    LocalProfile me_;
    NetAddress host_;
    Channel channel_;
    State state_ = State::Connecting;
    bool joinSent_ = false;
    bool everConnected_ = false;
    bool leaving_ = false;
    uint8_t localId_ = kSystemId;
    std::vector<PlayerInfo> roster_;
    std::deque<ShownLine> chat_;
    std::string closeText_;
    uint32_t refused_ = 0;
};

// Sends a probe to every target each kProbeEveryMs and keeps the rooms that answer, dropped after
// kRoomExpireMs of silence. Targets are broadcast and loopback addresses on the lobby port.
class LanBrowser {
public:
    struct FoundRoom {
        NetAddress address; // the reply's source ip with the room's announced port
        RoomInfo room;
        uint32_t lastSeenMs = 0;
    };

    LanBrowser(std::vector<NetAddress> targets, uint32_t nonce, DatagramSend send);

    void Update(uint32_t nowMs);
    // true when the datagram was a room reply for this browser, whether or not it was kept.
    bool Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);

    const std::vector<FoundRoom>& Rooms() const {
        return rooms_;
    }

private:
    std::vector<NetAddress> targets_;
    uint32_t nonce_;
    DatagramSend send_;
    bool probed_ = false;
    uint32_t lastProbeMs_ = 0;
    std::vector<FoundRoom> rooms_;
};

} // namespace gdx::net::lobby

#endif // GDX_NET_LOBBY_SESSION_H
