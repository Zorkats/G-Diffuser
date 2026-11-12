// port/net/gdx_net_peer_links.h: direct links between the members of a room (G-NET decisions 61 to 73).
// Copyright (c) Zorkats
//
// Pure, like the session classes: no socket and no clock. The socket owner hands it the GDXP datagrams that
// arrive and the time, and it says what to send through a callback. It holds the other members the host
// described in Peers, punches each one before it sends poses, and checks every datagram it is handed.
//
// A member's poses still go to the host, which still relays them (decision 61). These links are a second, shorter
// way for the same poses, and the race code keeps whichever copy of a pose arrives first.
//
// Datagram, outside the channel on the same socket ("GDXP" never starts a channel packet or a discovery datagram):
//
//   "GDXP" u8 kind (1 punch, 2 poses), u16 race index, u8 from id, u8 to id, body, 16 bytes MAC
//
// The MAC is keyed BLAKE2b (Monocypher) under the pair's secret over everything before it. A punch has no body. A
// poses body is a State payload (gdx_net_lobby.h message 23) whose race index matches the header's, and every pose
// in it is for the sender's own race slot.
//
// Each member has up to three kinds of endpoint: LAN addresses (only towards members behind the same public
// address, decision 68), the public address the host sees, and a TURN relay address. A copy with direct links off
// uses relay endpoints only, and the host gives nobody its public or LAN addresses (decision 67).
//
// Punch first (decision 64): until a valid datagram arrives from an endpoint, a copy sends that endpoint punches
// only, one every kPunchIntervalMs while the peer's punch window is open. The window opens for kPunchWindowMs when
// the entry arrives or changes, when a race starts, and when an open link goes silent. An endpoint that has never
// answered gets at most kUnansweredPunchCap punches over the links' life, so a host that lists a stranger's address
// cannot aim more than that at it. A valid datagram opens its endpoint; kSilenceMs without one closes it again.
// An open endpoint that nothing else was sent to for kKeepaliveMs gets a punch, which keeps NAT mappings alive
// between races.

#ifndef GDX_NET_PEER_LINKS_H
#define GDX_NET_PEER_LINKS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "gdx_net_lobby.h"
#include "gdx_net_socket.h"

namespace gdx::net::links {

constexpr size_t kHeaderBytes = 4 + 1 + 2 + 1 + 1;
constexpr size_t kMacBytes = 16;
constexpr uint8_t kKindPunch = 1;
constexpr uint8_t kKindPoses = 2;

constexpr uint32_t kPunchIntervalMs = 250;
constexpr uint32_t kPunchWindowMs = 15000;
constexpr uint32_t kSilenceMs = 3000;
constexpr uint32_t kKeepaliveMs = 1000;
// Four windows' worth: a pair that never opens stops trying after about four races.
constexpr uint32_t kUnansweredPunchCap = 4 * (kPunchWindowMs / kPunchIntervalMs);
// Never-answered endpoints remembered for the cap. Past it the oldest is forgotten.
constexpr size_t kUnansweredMemory = 64;

// True for a datagram that starts with "GDXP": the links' to check, whoever sent it.
bool IsLinkDatagram(const uint8_t* data, size_t len);

// How a datagram leaves this copy, or how it arrived. Direct: the game socket, to or from the address itself.
// Relay: this copy's TURN allocation, to or from the peer's relay address.
enum class Route : uint8_t { Direct = 0, Relay = 1 };

// The kinds of endpoint, best first. Poses go to a peer's best open endpoint only.
enum class PathKind : uint8_t { Lan = 0, Public = 1, Relay = 2 };
constexpr size_t kPathKinds = 3;

using LinkSend = std::function<void(Route route, const NetAddress& to, const uint8_t* data, size_t len)>;

struct LinkStats {
    uint32_t punchesSent = 0;
    uint32_t posesSent = 0;  // datagrams of kind 2
    uint32_t refused = 0;    // a GDXP datagram that failed a check
    std::array<uint32_t, kPathKinds> posesReceived{}; // poses accepted, by the kind of endpoint they came from
};

NetAddress AddressOf(const lobby::PeerEndpoint& endpoint);
lobby::PeerEndpoint EndpointOf(const NetAddress& address);

// One datagram, as a test or the session would build it. body may be null with bodyLen 0.
std::vector<uint8_t> BuildDatagram(const lobby::Bytes32& secret, uint8_t kind, uint16_t raceIndex, uint8_t from,
                                   uint8_t to, const uint8_t* body, size_t bodyLen);

class PeerLinks {
public:
    explicit PeerLinks(LinkSend send);

    // This copy's player id, whether it may send and take direct datagrams (gEnhancements.Online.DirectLinks), and
    // whether it has a relay allocation to send through. A change closes the endpoints it no longer allows.
    void SetLocal(uint8_t id, bool direct, bool relay);
    // A Peers message: each entry adds or replaces the link to that member, or forgets it.
    void Apply(const std::vector<lobby::PeerEntry>& entries, uint32_t nowMs);
    // Forget every peer: this copy left the room, or its host changed.
    void Clear();
    // Members still in the room, by id: a peer not listed is forgotten.
    void Keep(const std::vector<uint8_t>& ids);
    // The race being run, and the player id in each human race slot, from the front of the grid. Opens every
    // peer's punch window.
    void SetRace(uint16_t raceIndex, const std::vector<uint8_t>& slotIds, uint32_t nowMs);
    // Punches and keepalives due at nowMs.
    void Update(uint32_t nowMs);
    // This copy's own poses, to the best open endpoint of every peer. Poses for another race are not sent.
    void SendPoses(uint16_t raceIndex, const std::vector<lobby::StatePose>& poses, uint32_t nowMs);
    // A GDXP datagram that arrived by route from from. Appends the poses of a valid poses datagram for the race
    // being run to out, and returns true for any valid datagram. A datagram that fails a check changes nothing
    // and is counted in Stats().refused.
    bool Receive(Route route, const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs,
                 std::vector<lobby::StatePose>& out);

    size_t PeerCount() const {
        return peers_.size();
    }
    // Whether the peer has an open endpoint, and the best one's kind.
    bool Open(uint8_t id, uint32_t nowMs, PathKind& best) const;
    bool Known(uint8_t id) const;
    // The relay endpoints of the known peers, once each, in peer order: the addresses this copy's TURN allocation
    // must permit, whether or not it has one yet.
    std::vector<NetAddress> RelayAddresses() const;
    const LinkStats& Stats() const {
        return stats_;
    }

private:
    struct Candidate {
        PathKind kind = PathKind::Public;
        NetAddress address;
        bool answered = false;     // a valid datagram came from it once
        uint32_t lastValidMs = 0;  // when the last one did
        uint32_t lastSentMs = 0;   // when this copy last sent it anything
        bool sentOnce = false;
    };

    struct Peer {
        uint8_t id = 0;
        lobby::Bytes32 secret{};
        std::vector<Candidate> candidates;
        uint32_t windowUntilMs = 0;
        bool windowOpen = false;
        uint32_t nextPunchMs = 0;
        bool wasOpen = false;
    };

    struct Unanswered {
        NetAddress address;
        uint32_t punches = 0;
    };

    bool Allowed(PathKind kind) const;
    bool IsOpen(const Candidate& c, uint32_t nowMs) const;
    Candidate* BestOpen(Peer& peer, uint32_t nowMs);
    Peer* Find(uint8_t id);
    const Peer* Find(uint8_t id) const;
    void OpenWindow(Peer& peer, uint32_t nowMs);
    void SendTo(Peer& peer, Candidate& c, uint8_t kind, const uint8_t* body, size_t bodyLen, uint32_t nowMs);
    // false when this never-answered endpoint has had its share of punches; counts one more otherwise.
    bool TakePunch(const NetAddress& address);
    void ForgetUnanswered(const NetAddress& address);

    LinkSend send_;
    uint8_t localId_ = lobby::kSystemId;
    bool direct_ = true;
    bool relay_ = false;
    std::vector<Peer> peers_;
    bool raceOn_ = false;
    uint16_t raceIndex_ = 0;
    std::vector<uint8_t> slotIds_;
    std::vector<Unanswered> unanswered_;
    LinkStats stats_;
};

} // namespace gdx::net::links

#endif // GDX_NET_PEER_LINKS_H
