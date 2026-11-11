// port/net/gdx_net_lobby_session.h: the LAN room host, the joining client and the LAN room browser.
//
// None of these classes opens a socket or reads a clock. The owner passes a send callback that takes an
// address, feeds every received datagram to Receive(), and calls Update() with the current time in
// milliseconds once per tick. That keeps them testable on a fake network and a fake clock, as the channel
// is. One thread owns each object and makes every call. The send callback must not call back in.
//
// LobbyHost answers discovery probes and keeps one server Channel per peer address (gdx_net_channel.h).
// A peer is a player only after its Join passes: same build, a valid profile and a free seat of the kind
// it asked for. The host is player 0. What the room does is decided by a RoomMachine (gdx_net_room.h);
// LobbyHost decodes each message into a call on it and sends what it returns.
//
// LobbyClient mirrors the room from the host's messages: the roster, the room state with its timer, the
// ballots, the lottery and the pick. It checks the host's lottery with RunLottery.
//
// The race start is here: the grid, each racer's "track loaded" answer, the start time on the host clock,
// and the client's estimate of that clock (ClockSync).
//
// So are the poses of a race (State, unreliable). Every copy hands in the poses of the machines it drives
// and takes out the ones that arrived; neither class looks inside a pose beyond the decoder's limits. A
// client may speak only for its own race slot. The host speaks for its own and for the CPUs, and passes
// each player's poses on to the others at its next Update, so a pose a player sent reaches the rest one
// host tick later.
//
// And how a race ends: each copy reports its own finish or retirement (Finish), the host's room checks it
// against the laps that player's poses named and against its own clock, and everyone gets the table
// (Results). The host copy also hands in its game's order of the humans, which places whoever is still
// driving when the race is called.

#ifndef GDX_NET_LOBBY_SESSION_H
#define GDX_NET_LOBBY_SESSION_H

#include "gdx_net_channel.h"
#include "gdx_net_lobby.h"
#include "gdx_net_room.h"
#include "gdx_net_socket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gdx::net::lobby {

using DatagramSend = std::function<void(const NetAddress& to, const uint8_t* data, size_t len)>;

constexpr size_t kLogCap = 200;           // lines kept for the view
constexpr size_t kPendingPeerCap = 16;    // addresses mid-handshake at once
constexpr uint32_t kPendingPeerMs = 5000; // a peer that never joins is dropped after this
constexpr uint32_t kRejectLingerMs = 400; // time for a Reject to reach the peer before the link drops
// Silence before a link counts as lost: a player's seat is freed, a client leaves the room. The channel
// uses the same value for a handshake nobody answers.
constexpr uint32_t kLinkTimeoutMs = 3000;
constexpr uint32_t kPingEveryMs = 2000; // how often the roster's ping column is refreshed
constexpr uint32_t kProbeEveryMs = 1500;
constexpr uint32_t kRoomExpireMs = 5000;
constexpr size_t kRoomListCap = 32;
// Clock pings: quick until the first samples are in, then slow.
constexpr uint32_t kClockPingFastMs = 100;
constexpr uint32_t kClockPingSlowMs = 500;
constexpr size_t kClockWarmSamples = 8;
constexpr size_t kClockWindow = 16;      // samples the offset is chosen from
constexpr uint32_t kClockStaleMs = 2000; // an answer older than this is not a sample
// Poses. One pose leaves each copy every 33 ms and the owner ticks every 10 ms, so more than a few from
// one player in one Update is a flood, or the tail of a stall: the rest are dropped.
constexpr size_t kPosesPerPeerPerUpdate = 4;
constexpr size_t kPoseBoxCap = 256; // poses waiting for the game, or for the next Update

struct LocalProfile {
    std::string name;
    uint32_t colour = 0;
    uint8_t machine = 0xFF;
};

// A log line ready to show: a preset message, or a room notice ("Ana joined the room."). from is a player
// id or kSystemId; name and colour are copied from the roster when the line arrives, so a line keeps its
// sender after the sender leaves. The words are produced on each machine: no text travels.
struct ShownLine {
    uint8_t from = kSystemId;
    std::string name;
    uint32_t colour = 0;
    std::string text;
};

// The host clock as a client sees it. Each ping comes back with the host's time; half the round trip
// later is "now" over there. The trips are not equal and both ends answer on a 10 ms tick, so the sample
// with the shortest round trip of the last kClockWindow is the one used: it had the least room to be wrong.
class ClockSync {
public:
    // An answer arrived at local time nowMs for the ping stamped clientMs. false when it is too old.
    bool Sample(uint32_t clientMs, uint32_t hostMs, uint32_t nowMs);

    bool Ready() const {
        return count_ > 0;
    }

    // Host clock minus local clock, modulo 2^32. 0 before the first sample.
    uint32_t OffsetMs() const;
    // The round trip of the sample in use.
    uint32_t RttMs() const;

    size_t Samples() const {
        return total_;
    }

private:
    struct Entry {
        uint32_t rttMs = 0;
        uint32_t offsetMs = 0;
    };

    const Entry* Best() const;

    std::array<Entry, kClockWindow> ring_{};
    size_t count_ = 0;
    size_t next_ = 0;
    size_t total_ = 0;
};

class LobbyHost {
public:
    LobbyHost(std::string build, LocalProfile me, uint16_t port, const RoomConfig& config, DatagramSend send);
    ~LobbyHost();

    LobbyHost(const LobbyHost&) = delete;
    LobbyHost& operator=(const LobbyHost&) = delete;

    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    // The host player's own actions. Each returns what the RoomMachine call of the same name returns.
    bool SetReady(bool ready, uint32_t nowMs);
    bool Vote(uint8_t track, uint32_t nowMs);
    bool Pick(uint8_t track, uint32_t nowMs);
    bool Lock(const LockRequest& lock, uint32_t nowMs);
    // false, with a notice in the log, when the host player is over the rate limit.
    bool Emote(uint8_t emote, uint32_t nowMs);
    // The host copy of the game has loaded the track.
    bool GridReady(const ReadyToRace& ready, uint32_t nowMs);
    // The machines the host copy drives in the race raceIndex: its own and the CPUs. They leave with the
    // next Update. A pose for another race, or for a slot a player drives, is dropped.
    void SendPoses(uint16_t raceIndex, const std::vector<StatePose>& poses);
    // The players' poses that arrived since the last call, for the host copy of the game, and the race
    // they belong to.
    void TakePoses(uint16_t& raceIndex, std::vector<StatePose>& out);
    // The host player's own race ended. Held to the same rules as anyone's.
    bool Finish(const FinishReport& report, uint32_t nowMs);
    // The host game's order of the humans of the race being run, first to last, by player id.
    void SetStanding(const std::vector<uint8_t>& order);
    // The host copy of the game left the race being run: its CPU machines are out of it, and every copy
    // hears so. The race goes on for the others. false outside a race, and the second time.
    bool HostLeftRace(uint32_t nowMs);

    // The host's room controls.
    bool SetRules(const RaceRules& rules);
    bool Start(uint32_t nowMs);
    bool SkipLottery(uint32_t nowMs);
    bool EndSeries(uint32_t nowMs);
    bool SetRacerCap(uint8_t cap, uint32_t nowMs);
    void Kick(uint8_t id, uint32_t nowMs);
    // Says goodbye to every peer. The host takes no more calls after this except the getters.
    void Close(uint32_t nowMs);

    const std::vector<PlayerInfo>& Roster() const {
        return machine_.Roster();
    }

    RoomState Room(uint32_t nowMs) const {
        return machine_.State(nowMs);
    }

    const std::vector<Ballot>& Votes() const {
        return machine_.Votes();
    }

    const LotteryResult& Lottery() const {
        return machine_.Lottery();
    }

    const Picked& LastPicked() const {
        return machine_.LastPicked();
    }

    const std::vector<uint8_t>& LastOrder() const {
        return machine_.LastOrder();
    }

    // The table last sent. Its race index says which race, and final whether that race is over.
    const RaceResults& Results() const {
        return machine_.Results();
    }

    // The grid of the race being loaded or run. Its race index says which race.
    const Grid& RaceGrid() const {
        return machine_.LastGrid();
    }

    // The last start the room sent, on this (the host) clock. false before the first one.
    bool StartSent(StartSignal& out) const {
        out = start_;
        return haveStart_;
    }

    uint16_t LoadMs(uint8_t id) const {
        return machine_.LoadMs(id);
    }

    const std::deque<ShownLine>& Lines() const {
        return lines_;
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
    void HandleState(Peer& peer, const Message& msg);
    // True while a race is being loaded, counted in or run.
    bool RaceOn() const;
    // The race slot the player drives in that race, or -1.
    int RaceSlotOf(uint8_t id) const;
    void FlushPoses();
    void Reject(Peer& peer, RejectReason reason, uint32_t nowMs);
    void RemovePlayer(uint8_t id, uint32_t nowMs);
    void Dispatch(std::vector<Outgoing>& out);
    void SendToPlayers(uint8_t type, const std::vector<uint8_t>& payload);
    std::string NameOf(uint8_t id) const;
    void Note(const std::string& text);

    std::string build_;
    LocalProfile me_;
    uint16_t port_;
    DatagramSend send_;
    RoomMachine machine_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::deque<ShownLine> lines_;
    uint32_t lastPingMs_ = 0;
    bool pinged_ = false;
    StartSignal start_;
    bool haveStart_ = false;
    std::vector<StatePose> posesOut_; // for everyone, at the next Update
    std::deque<StatePose> posesIn_;   // for the host copy of the game
    uint16_t posesRace_ = 0;          // the race both lists belong to
    uint32_t refused_ = 0;
    bool closed_ = false;
};

class LobbyClient {
public:
    enum class State : uint8_t { Connecting, Joined, Closed };

    // seat: the kind of seat to ask for. nonce: a fresh random value for this attempt
    // (ChannelConfig::connectionNonce).
    LobbyClient(std::string build, LocalProfile me, Seat seat, NetAddress host, uint32_t nonce, DatagramSend send);

    void Start(uint32_t nowMs);
    // Datagrams from any address other than the host are ignored.
    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    // Requests. The host decides; what it decided comes back in the roster and the room state.
    void SetReady(bool ready);
    void Vote(uint8_t track);
    void Pick(uint8_t track);
    void Lock(const LockRequest& lock);
    // false, with a notice in the log, when this player is over the rate limit.
    bool Emote(uint8_t emote, uint32_t nowMs);
    // A watcher asks for a race seat at the next lock-in screen, or takes the request back.
    void RaceNext(bool raceNext);
    // This copy of the game has loaded the track.
    void GridReady(const ReadyToRace& ready);
    // This copy's own machine in the race raceIndex, for the host to pass on. A pose for another race, or
    // for a slot that is not ours, is dropped.
    void SendPoses(uint16_t raceIndex, const std::vector<StatePose>& poses);
    // The other machines' poses that arrived since the last call, and the race they belong to.
    void TakePoses(uint16_t& raceIndex, std::vector<StatePose>& out);
    // Our own race ended. The host decides whether it counts; the answer is the next table.
    void Finish(const FinishReport& report);
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

    // The room as the host last described it, with the phase timer run down to nowMs.
    RoomState Room(uint32_t nowMs) const;

    const std::vector<Ballot>& Votes() const {
        return votes_;
    }

    const LotteryResult& Lottery() const {
        return lottery_;
    }

    // false when the last lottery the host sent is not the one RunLottery gives for the same ballots.
    bool LotteryChecked() const {
        return lotteryChecked_;
    }

    const Picked& LastPicked() const {
        return picked_;
    }

    // The last grid the host sent. false before the first one.
    bool RaceGrid(Grid& out) const {
        if (haveGrid_) {
            out = grid_;
        }
        return haveGrid_;
    }

    // The last start the host sent, on the host clock. false before the first one.
    bool StartSent(StartSignal& out) const {
        out = start_;
        return haveStart_;
    }

    // The last table the host sent. false before the first one.
    bool Results(RaceResults& out) const {
        if (haveResults_) {
            out = results_;
        }
        return haveResults_;
    }

    const ClockSync& Clock() const {
        return clock_;
    }

    const std::deque<ShownLine>& Lines() const {
        return lines_;
    }

    uint32_t Refused() const {
        return refused_;
    }

private:
    void HandleMessage(const Message& msg, uint32_t nowMs);
    void HandleState(const Message& msg);
    // Our race slot in the last grid, or -1.
    int OwnSlot() const;
    void TakeRoster(std::vector<PlayerInfo> roster);
    void Send(uint8_t type, const std::vector<uint8_t>& payload);
    void Note(const std::string& text);
    void CloseWith(const std::string& text);

    std::string build_;
    LocalProfile me_;
    Seat seat_;
    NetAddress host_;
    Channel channel_;
    State state_ = State::Connecting;
    bool joinSent_ = false;
    bool everConnected_ = false;
    uint8_t localId_ = kSystemId;
    std::vector<PlayerInfo> roster_;
    bool haveRoster_ = false;
    RoomState room_;
    uint32_t roomAtMs_ = 0;
    Grid grid_;
    bool haveGrid_ = false;
    StartSignal start_;
    bool haveStart_ = false;
    RaceResults results_;
    bool haveResults_ = false;
    ClockSync clock_;
    uint32_t lastClockPingMs_ = 0;
    bool clockPinged_ = false;
    std::deque<StatePose> posesIn_; // for this copy of the game
    uint16_t posesRace_ = 0;        // the race they belong to
    std::vector<Ballot> votes_;
    LotteryResult lottery_;
    bool lotteryChecked_ = true;
    Picked picked_;
    std::deque<ShownLine> lines_;
    std::deque<uint32_t> emoteTimes_;
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
