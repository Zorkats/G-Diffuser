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
//
// A custom track moves through a HostTrackFlow and a ClientTrackFlow (gdx_net_track_flow.h). Each class runs
// its flow every Update and passes it the transfer messages; the host tells its room who holds the track.
// RaceTrack() is the verified track a copy races on, from memory.
//
// When the host is lost, each client's NextHost() says what comes next (HandoverPlan): the successor hosts the
// room from its own mirror, and the others come back to it with Rejoin and the token the old host gave them.
//
// Friend keys (O7.3, decision 42). The host sends each new link a Nonce: the room id, its own friend key and 32
// fresh bytes. The client signs that with its JoinSigner and sends the key and the proof in Join. The host checks
// the proof, refuses a banned key, and in a Friends room refuses any key that is not on its friend list. A copy with
// no signer joins with no key, which only a Friends room refuses. Every member's key goes to everyone in Keys, so a
// member can add another as a friend, and the host can ban by key. The proof binds the room and the host's key: a
// proof made for one room does not open another. It cannot stop a host from forwarding another host's Nonce, so a
// client that knows the key it expects (a listed room, a friend's room) checks the Nonce against it. That key is
// only as good as its source. The directory accepts a write only when the key it names signed it (decision 53), so
// a record's key belongs to whoever listed the room.

#ifndef GDX_NET_LOBBY_SESSION_H
#define GDX_NET_LOBBY_SESSION_H

#include "gdx_net_channel.h"
#include "gdx_net_lobby.h"
#include "gdx_net_room.h"
#include "gdx_net_socket.h"
#include "gdx_net_track_flow.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gdx::net::lobby {

using DatagramSend = std::function<void(const NetAddress& to, const uint8_t* data, size_t len)>;

constexpr size_t kLogCap = 200;           // lines kept for the view
constexpr size_t kPendingPeerCap = 16;    // addresses mid-handshake at once
// Of those, the most one source may hold: one IPv4 address or one IPv6 /64, so a single machine sending hellos
// from many ports cannot fill every slot and keep real joiners out. Loopback has no cap (local tests).
constexpr size_t kPendingPerSourceCap = 4;
constexpr uint32_t kPendingPeerMs = 5000; // a peer that never joins is dropped after this
constexpr uint32_t kRejectLingerMs = 400; // time for a Reject to reach the peer before the link drops
// A hello cookie (decision 34) is good in the bucket it was made in and the next one: 8 to 16 s, far longer than a
// handshake, and short enough that a cookie someone once saw stops working.
constexpr uint32_t kCookieBucketMs = 8192;
// Silence before a link counts as lost: a player's seat is freed, a client leaves the room. The channel
// uses the same value for a handshake nobody answers.
constexpr uint32_t kLinkTimeoutMs = 3000;
// After the host is lost (decision 16): how long a member keeps trying the successor, and how long a promoted
// successor holds the seats of members who have not come back. A successor nobody came back to closes.
constexpr uint32_t kSuccessorWaitMs = 10000;
// A host that leaves (D28.5) waits this long at most for its Handover to be acknowledged before it goes.
constexpr uint32_t kHandoverLingerMs = 1500;
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
    // The ROM hack this copy runs: empty for the stock game. A host lets in only a copy with the same hack.
    HackIdentity hack;
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

// What a member does once its host is gone (D28.4, decision 22): the host fell silent for kLinkTimeoutMs, or
// sent Handover. Promote: this copy is the successor, and hosts the room from its mirror (RoomMachine::Promote,
// the second LobbyHost constructor) on the socket the old host knew it by. Rejoin: this copy connects to the
// successor and comes back under its old id with its token (LobbyClient::SetRejoin). None: the room is over
// for this copy, because nobody takes over or this copy holds no token.
struct HandoverPlan {
    enum class Kind : uint8_t { None, Promote, Rejoin };
    Kind kind = Kind::None;
    uint8_t successor = kSystemId; // its id under the old host
    NetAddress address;            // where the successor hosts
    uint8_t id = kSystemId;        // this copy's id under the old host
    uint64_t token = 0;            // Rejoin only
    uint32_t sinceMs = 0;          // when this copy lost the host, on its own clock
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
    LobbyHost(std::string build, LocalProfile me, uint16_t port, const RoomConfig& config, DatagramSend send,
              TrackStore store = TrackStore());
    // The successor's host (D28.3): the room RoomMachine::Promote built from this copy's mirror, and the token
    // table this copy got as successor, by the old ids. The members come back with Rejoin (D28.4); until then
    // they hold their seats without a link.
    LobbyHost(std::string build, LocalProfile me, uint16_t port, RoomMachine machine,
              const std::vector<HandoverTokenEntry>& tokens, uint8_t promotedFrom, DatagramSend send,
              TrackStore store = TrackStore());
    ~LobbyHost();

    LobbyHost(const LobbyHost&) = delete;
    LobbyHost& operator=(const LobbyHost&) = delete;

    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    // The host player's own actions. Each returns what the RoomMachine call of the same name returns.
    bool SetReady(bool ready, uint32_t nowMs);
    bool Vote(uint8_t track, uint32_t nowMs);
    bool Vote(const TrackChoice& choice, uint32_t nowMs);
    bool Pick(uint8_t track, uint32_t nowMs);
    bool Pick(const TrackChoice& choice, uint32_t nowMs);
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
    bool SetCustomTracks(bool on, uint32_t nowMs);
    // The CPU settings a successor needs (decision 24). Refused once a grid has been sent.
    bool SetCpuOptions(const CpuOptions& cpu, uint32_t nowMs);
    const CpuOptions& Cpu() const {
        return machine_.Cpu();
    }
    void Kick(uint8_t id, uint32_t nowMs);
    // Removes the player and refuses its friend key and its address (an IPv6 one by its /64; never loopback) for
    // as long as the room lasts: the bans go to the successor with the token table (decision 52). At most kBanMax
    // of each; past that the oldest goes. false when no such player is in the room.
    bool Ban(uint8_t id, uint32_t nowMs);
    const std::vector<Bytes32>& Banned() const {
        return banned_;
    }
    BanList Bans() const {
        return BanList{banned_, bannedIps_};
    }
    // A promoted host takes the old host's bans (LobbyClient::Bans), so a player banned before the handover stays
    // out. Lists longer than kBanMax keep their newest entries.
    void SetBans(const BanList& bans);
    // This host's friend key, sent in each Nonce. All zero until set: then a client that expects a key refuses.
    void SetIdentity(const Bytes32& key) {
        identity_ = key;
    }
    // The keys a Friends room admits: the host's friend list. Other room types ignore it.
    void SetFriends(std::vector<Bytes32> keys) {
        friends_ = std::move(keys);
    }
    // Every member's friend key, the host's own included, by id and ascending. Members with no key are left out.
    std::vector<MemberKey> Keys() const;
    // A promoted host learns the members' keys from its mirror (LobbyClient::Keys), by their ids under the old
    // host; the remap of decision 20 is done here.
    void SetMirrorKeys(const std::vector<MemberKey>& oldKeys);
    // Says goodbye to every peer. The host takes no more calls after this except the getters.
    void Close(uint32_t nowMs);
    // The host player leaves and the room goes on (D28.5, decision 17). During a race the host keeps relaying
    // until the race is over; the host copy of the game reports its own machine (Finish, HostLeftRace) before
    // it leaves. Then, or at once outside a race, every member is sent Handover naming the successor. Once
    // that is acknowledged, or after kHandoverLingerMs, the host closes without a goodbye, so a member that
    // missed the message still finds the successor through the silence. With no successor linked, this is
    // Close().
    void Leave(uint32_t nowMs);
    bool Leaving() const {
        return leaving_;
    }
    // Whether the Handover went out: the room goes on under the successor, so its directory record stays.
    bool HandedOver() const {
        return handedOver_;
    }
    // The member the room would go to now, or kSystemId.
    uint8_t Successor() const {
        return machine_.Successor();
    }

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

    // The custom track of the race being set up, once the host's copy verified. Null for a stock track.
    std::shared_ptr<const transfer::TrackBuffer> RaceTrack() const {
        return flow_.Held();
    }

    TrackProgress Track() const {
        return flow_.Progress();
    }

    bool HoldsTrack(uint8_t id) const {
        return machine_.HoldsTrack(id);
    }

    const transfer::Counters& TrackDrops() const {
        return flow_.Counts();
    }

    const std::deque<ShownLine>& Lines() const {
        return lines_;
    }

    // Counts every message the host refused or could not decode, for the view and the tests.
    uint32_t Refused() const {
        return refused_;
    }

    // The nonce the host sent the link from this address, for the tests. All zero before one.
    Bytes32 NonceFor(const NetAddress& addr) const;
    // Hellos answered with a cookie challenge instead of a channel (decision 34), for the tests.
    uint32_t Challenged() const {
        return challenged_;
    }

    size_t PeerCount() const {
        return peers_.size();
    }

    // The host's upload (L2, decision 46): the ChannelStats byte and datagram counters of every link this host
    // had, closed links included, and how many links are open now.
    struct UploadTotals {
        uint64_t bytes = 0;
        uint64_t datagrams = 0;
        size_t links = 0;
    };
    UploadTotals Upload() const;

    // A directory room (O5.10): the takeover secret goes to the successor with the token table, and again
    // whenever it changes.
    void SetTakeoverSecret(const Bytes32& secret) {
        takeoverSecret_ = secret;
        tableTo_ = kSystemId;
    }
    // True once after the member who was sent the takeover secret stopped being the successor, by leaving or
    // by the order changing. The listing then replaces the secret (RotateSecret).
    bool TakeSecretExposed() {
        const bool exposed = secretExposed_;
        secretExposed_ = false;
        return exposed;
    }
    // The code the directory lists the room under; members see it in the next `room`.
    bool SetRoomCode(uint32_t code) {
        return machine_.SetCode(code);
    }

    // Handover tokens (decision 21). Each member gets its own over its link, and a new one whenever a member
    // leaves or is removed, or the successor changes while the old one stays. The successor alone gets the
    // table. Tests replace the source; by default it is std::random_device.
    void SetTokenSource(std::function<uint64_t()> source);
    // The token the member holds now; 0 for none.
    uint64_t TokenOf(uint8_t id) const {
        return id < kMaxPlayers ? tokens_[id] : 0;
    }
    // After a promotion, the token a member must come back with, by its id in this room; 0 for none.
    uint64_t RejoinTokenOf(uint8_t id) const {
        return id < kMaxPlayers ? rejoinTokens_[id] : 0;
    }
    // The id the successor had under the old host, or kSystemId for a room this copy opened.
    uint8_t PromotedFrom() const {
        return promotedFrom_;
    }
    // Members a promoted host took back with Rejoin.
    size_t RejoinCount() const {
        return rejoined_;
    }

    // True once the host closed itself: a promoted host nobody came back to within kSuccessorWaitMs, or a host
    // that left and handed the room over. Close() sets it too. CloseText says why, for the player; empty after
    // Close().
    bool Closed() const {
        return closed_;
    }
    const std::string& CloseText() const {
        return closeText_;
    }

private:
    struct Peer;

    void HandleMessage(Peer& peer, const Message& msg, uint32_t nowMs);
    void HandleJoin(Peer& peer, const Message& msg, uint32_t nowMs);
    void HandleRejoin(Peer& peer, const Message& msg, uint32_t nowMs);
    // A promoted host's rejoin window: starts at the first Update, and at its end frees the seats nobody came
    // back to, or closes the room when nobody came back at all. false when the room closed.
    bool RunRejoinWindow(uint32_t nowMs);
    // Sends Handover to every member, or closes the room when nobody can take it.
    void HandOver(uint32_t nowMs);
    // After Handover: keeps the links up until the message is acknowledged, then goes without a goodbye.
    void LingerAfterHandover(uint32_t nowMs);
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
    void SyncTrack(uint32_t nowMs);
    void ApplyNews(const RoomNews& news, uint32_t nowMs, std::vector<Outgoing>& out);
    Peer* JoinedPeer(uint8_t id) const;
    uint64_t NewToken();
    void SyncTokens();
    void Gate(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    uint64_t CookieFor(const NetAddress& from, uint32_t nonce, uint32_t bucket) const;
    // Sends the link its Nonce, once, when it first comes up.
    void SendNonce(Peer& peer);
    // The O7.3 checks of a Join: the proof, the ban list, and a Friends room's list. Ok, or why not.
    bool AdmitKey(const Peer& peer, const JoinRequest& join, uint32_t nowMs, RejectReason& why) const;
    void SendKeys();

    std::string build_;
    LocalProfile me_;
    uint16_t port_;
    DatagramSend send_;
    RoomMachine machine_;
    HostTrackFlow flow_;
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
    uint32_t challenged_ = 0;
    UploadTotals uploadGone_; // the counters of links already dropped
    std::array<uint64_t, 2> cookieKey_{}; // the SipHash key of the hello cookie, random per host
    bool closed_ = false;
    std::function<uint64_t()> tokenSource_;
    std::array<uint64_t, kMaxPlayers> tokens_{};       // each linked member's token, by id
    std::array<uint64_t, kMaxPlayers> rejoinTokens_{}; // after a promotion, by the id in this room
    bool tokensStale_ = false;                         // a member left: every token is reissued
    bool bansChanged_ = false;                         // a ban since the table last went out
    bool secretExposed_ = false;                       // the secret's holder is no longer the successor
    uint8_t tableTo_ = kSystemId;                      // who holds the table that was last sent
    Bytes32 takeoverSecret_{};                         // sent with the table (decision 32)
    uint8_t promotedFrom_ = kSystemId;
    bool windowStarted_ = false;
    bool windowDone_ = false;
    uint32_t windowStartMs_ = 0;
    size_t rejoined_ = 0;
    std::string closeText_;
    bool leaving_ = false;     // Leave was called
    bool handedOver_ = false;  // Handover went out
    uint32_t handoverMs_ = 0;
    uint8_t handedTo_ = kSystemId;
    Bytes32 identity_{};
    std::vector<Bytes32> friends_;
    std::vector<Bytes32> banned_;
    std::vector<IpBytes> bannedIps_; // BanPrefix of each banned member's address
    std::array<Bytes32, kMaxPlayers> keys_{}; // each member's friend key by id; all zero for none
};

// Signs a host's Nonce for the client (friends::JoinProofSign with the profile's seed): fills this copy's friend
// key and the proof. false when the profile has no key; the client then joins without one.
using JoinSigner = std::function<bool(const JoinNonce& nonce, Bytes32& key, std::array<uint8_t, 64>& proof)>;

class LobbyClient {
public:
    enum class State : uint8_t { Connecting, Joined, Closed };

    // seat: the kind of seat to ask for. nonce: a fresh random value for this attempt
    // (ChannelConfig::connectionNonce).
    LobbyClient(std::string build, LocalProfile me, Seat seat, NetAddress host, uint32_t nonce, DatagramSend send,
                TrackStore store = TrackStore());

    void Start(uint32_t nowMs);
    // Datagrams from any address other than the host are ignored.
    void Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs);
    void Update(uint32_t nowMs);

    // Requests. The host decides; what it decided comes back in the roster and the room state.
    void SetReady(bool ready);
    void Vote(uint8_t track);
    void Vote(const TrackChoice& choice);
    void Pick(uint8_t track);
    void Pick(const TrackChoice& choice);
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

    // The custom track of the race being set up, once this copy holds it verified. Null for a stock track.
    std::shared_ptr<const transfer::TrackBuffer> RaceTrack() const {
        return flow_.Held();
    }

    TrackProgress Track() const {
        return flow_.Progress();
    }

    const transfer::Counters& TrackDrops() const {
        return flow_.Counts();
    }

    const std::deque<ShownLine>& Lines() const {
        return lines_;
    }

    uint32_t Refused() const {
        return refused_;
    }

    // What this copy holds of the room, for RoomMachine::Promote (D28.3).
    RoomMirror Mirror(uint32_t nowMs) const;
    // This member's handover token, 0 before the host sent one (decision 21).
    uint64_t HandoverTokenOwn() const {
        return ownToken_;
    }
    // Every member's token, sent only while this copy is the successor. Empty before.
    const std::vector<HandoverTokenEntry>& HandoverTable() const {
        return tokenTable_;
    }
    // The directory takeover secret the host sent with the table (decision 32), all zero outside a directory
    // room or before this copy was the successor.
    const Bytes32& TakeoverSecret() const {
        return takeoverSecret_;
    }
    // The room's bans, sent with the table (decision 52). Empty before this copy was the successor.
    const BanList& Bans() const {
        return bans_;
    }
    // Whether this copy can take inbound connections (decision 51). Before Start it rides in Join; once joined it
    // goes as Reach when it changes, and again after a rejoin, since a promoted host starts with nobody reachable.
    void SetReachable(bool reachable);
    // true once the host answered. A join by code tries the room's next address after a close without it.
    bool EverConnected() const {
        return everConnected_;
    }
    // Once Closed: what to do next. Kind None unless this copy was in the room when the host went.
    const HandoverPlan& NextHost() const {
        return plan_;
    }
    // Makes this client come back to a promoted successor instead of joining: call it before Start, on a
    // client built for plan.address. It sends Rejoin in place of Join, and keeps trying a successor that does
    // not answer until kSuccessorWaitMs after plan.sinceMs. false, changing nothing, for any other plan.
    bool SetRejoin(const HandoverPlan& plan);

    // O7.3. Without a signer the client joins with no key. Call both before Start.
    void SetSigner(JoinSigner signer) {
        signer_ = std::move(signer);
    }
    // The friend key the host must show in its Nonce: the key a directory listed, or a friend's. A Nonce with any
    // other key closes the client before it joins.
    void ExpectHostKey(const Bytes32& key) {
        expectKey_ = key;
        expectingKey_ = true;
    }
    // The members' friend keys from the host's last Keys, by id. Members with no key are left out.
    const std::vector<MemberKey>& Keys() const {
        return keys_;
    }
    // Decision 60: in a directory room, a handover sends this client only to a successor address DialableIp
    // allows. allow lets loopback through, for a local test directory. A LAN room's successor is not checked.
    void AllowLoopback(bool allow) {
        allowLoopback_ = allow;
    }

private:
    // Fills plan_ from the successor the host named, as the host is lost at nowMs. false when the successor's
    // address is not one this client sends to (decision 60): the plan stays None.
    bool PlanHandover(uint8_t successor, const IpBytes& ip, uint16_t port, uint32_t nowMs);
    void HandleMessage(const Message& msg, uint32_t nowMs);
    void HandleState(const Message& msg);
    // Our race slot in the last grid, or -1.
    int OwnSlot() const;
    void TakeRoster(std::vector<PlayerInfo> roster);
    void Send(uint8_t type, const std::vector<uint8_t>& payload);
    void Note(const std::string& text);
    void CloseWith(const std::string& text);
    // Sends Reach when joined and the host was last told otherwise.
    void SyncReach();
    // This copy named the room's custom track, by the winning ballot or the pick.
    bool OwnsTrack() const;
    void SyncTrack(uint32_t nowMs);

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
    ClientTrackFlow flow_;
    uint64_t ownToken_ = 0;
    std::vector<HandoverTokenEntry> tokenTable_;
    Bytes32 takeoverSecret_{};
    BanList bans_;
    bool reachable_ = false;
    bool hostReachable_ = false; // what the host was last told, by Join or Reach
    HandoverPlan plan_;
    bool rejoin_ = false; // SetRejoin: this client comes back to a successor
    bool allowLoopback_ = false;
    uint8_t rejoinId_ = kSystemId;
    uint64_t rejoinToken_ = 0;
    uint32_t rejoinSinceMs_ = 0;
    JoinSigner signer_;
    Bytes32 expectKey_{};
    bool expectingKey_ = false;
    std::vector<MemberKey> keys_;
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
