// port/net/gdx_net_peer_links.cpp: direct links between the members of a room (gdx_net_peer_links.h).
// Copyright (c) Zorkats

#include "gdx_net_peer_links.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "monocypher.h"

namespace gdx::net::links {

using lobby::PeerEndpoint;
using lobby::PeerEntry;
using lobby::RaceState;
using lobby::StatePose;

namespace {

constexpr uint8_t kMagic[4] = { 'G', 'D', 'X', 'P' };

uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs; // unsigned: correct across the 49-day wrap
}

// now is at or past then, on a clock that wraps.
bool Reached(uint32_t nowMs, uint32_t thenMs) {
    return static_cast<int32_t>(nowMs - thenMs) >= 0;
}

Route RouteOf(PathKind kind) {
    return kind == PathKind::Relay ? Route::Relay : Route::Direct;
}

void Mac(const lobby::Bytes32& secret, const uint8_t* data, size_t len, uint8_t out[kMacBytes]) {
    crypto_blake2b_keyed(out, kMacBytes, secret.data(), secret.size(), data, len);
}

} // namespace

bool IsLinkDatagram(const uint8_t* data, size_t len) {
    return data != nullptr && len >= sizeof(kMagic) && std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
}

NetAddress AddressOf(const PeerEndpoint& endpoint) {
    NetAddress a;
    a.ip = endpoint.ip;
    a.port = endpoint.port;
    return a;
}

PeerEndpoint EndpointOf(const NetAddress& address) {
    PeerEndpoint e;
    e.ip = address.ip;
    e.port = address.port;
    return e;
}

std::vector<uint8_t> BuildDatagram(const lobby::Bytes32& secret, uint8_t kind, uint16_t raceIndex, uint8_t from,
                                   uint8_t to, const uint8_t* body, size_t bodyLen) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + bodyLen + kMacBytes);
    out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
    out.push_back(kind);
    out.push_back(static_cast<uint8_t>(raceIndex & 0xFF));
    out.push_back(static_cast<uint8_t>(raceIndex >> 8));
    out.push_back(from);
    out.push_back(to);
    if (body != nullptr && bodyLen > 0) {
        out.insert(out.end(), body, body + bodyLen);
    }
    uint8_t mac[kMacBytes];
    Mac(secret, out.data(), out.size(), mac);
    out.insert(out.end(), mac, mac + kMacBytes);
    return out;
}

PeerLinks::PeerLinks(LinkSend send) : send_(std::move(send)) {
}

void PeerLinks::SetLocal(uint8_t id, bool direct, bool relay) {
    if (id != localId_) {
        peers_.clear(); // the pair secrets were made for the old id
    }
    localId_ = id;
    direct_ = direct;
    relay_ = relay;
}

bool PeerLinks::Allowed(PathKind kind) const {
    return kind == PathKind::Relay ? relay_ : direct_;
}

bool PeerLinks::IsOpen(const Candidate& c, uint32_t nowMs) const {
    return c.answered && Allowed(c.kind) && Elapsed(nowMs, c.lastValidMs) < kSilenceMs;
}

PeerLinks::Candidate* PeerLinks::BestOpen(Peer& peer, uint32_t nowMs) {
    Candidate* best = nullptr;
    for (Candidate& c : peer.candidates) {
        if (IsOpen(c, nowMs) && (best == nullptr || c.kind < best->kind)) {
            best = &c;
        }
    }
    return best;
}

PeerLinks::Peer* PeerLinks::Find(uint8_t id) {
    for (Peer& p : peers_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

const PeerLinks::Peer* PeerLinks::Find(uint8_t id) const {
    for (const Peer& p : peers_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool PeerLinks::Known(uint8_t id) const {
    return Find(id) != nullptr;
}

std::vector<NetAddress> PeerLinks::RelayAddresses() const {
    std::vector<NetAddress> out;
    for (const Peer& p : peers_) {
        for (const Candidate& c : p.candidates) {
            if (c.kind == PathKind::Relay && std::find(out.begin(), out.end(), c.address) == out.end()) {
                out.push_back(c.address);
            }
        }
    }
    return out;
}

bool PeerLinks::Open(uint8_t id, uint32_t nowMs, PathKind& best) const {
    const Peer* peer = Find(id);
    if (peer == nullptr) {
        return false;
    }
    bool found = false;
    for (const Candidate& c : peer->candidates) {
        if (IsOpen(c, nowMs) && (!found || c.kind < best)) {
            best = c.kind;
            found = true;
        }
    }
    return found;
}

void PeerLinks::OpenWindow(Peer& peer, uint32_t nowMs) {
    peer.windowUntilMs = nowMs + kPunchWindowMs;
    peer.windowOpen = true;
    peer.nextPunchMs = nowMs;
}

void PeerLinks::Apply(const std::vector<PeerEntry>& entries, uint32_t nowMs) {
    for (const PeerEntry& entry : entries) {
        if (entry.id == localId_) {
            continue;
        }
        if (entry.Forget()) {
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer& p) { return p.id == entry.id; }),
                         peers_.end());
            continue;
        }
        std::vector<Candidate> fresh;
        const auto add = [&](PathKind kind, const PeerEndpoint& e) {
            if (e.None()) {
                return;
            }
            const NetAddress address = AddressOf(e);
            for (const Candidate& c : fresh) {
                if (c.address == address && c.kind == kind) {
                    return;
                }
            }
            Candidate c;
            c.kind = kind;
            c.address = address;
            fresh.push_back(c);
        };
        for (const PeerEndpoint& e : entry.lan) {
            add(PathKind::Lan, e);
        }
        add(PathKind::Public, entry.pub);
        add(PathKind::Relay, entry.relay);

        Peer* peer = Find(entry.id);
        if (peer == nullptr) {
            peers_.emplace_back();
            peer = &peers_.back();
            peer->id = entry.id;
        } else if (peer->secret == entry.secret) {
            // The same pair: keep what each endpoint already showed.
            bool added = false;
            for (Candidate& c : fresh) {
                const auto old = std::find_if(peer->candidates.begin(), peer->candidates.end(), [&](const Candidate& o) {
                    return o.address == c.address && o.kind == c.kind;
                });
                if (old != peer->candidates.end()) {
                    c = *old;
                } else {
                    added = true;
                }
            }
            peer->candidates = std::move(fresh);
            if (added) {
                OpenWindow(*peer, nowMs);
            }
            continue;
        }
        peer->secret = entry.secret;
        peer->candidates = std::move(fresh);
        peer->wasOpen = false;
        OpenWindow(*peer, nowMs);
    }
}

void PeerLinks::Clear() {
    peers_.clear();
    raceOn_ = false;
    slotIds_.clear();
}

void PeerLinks::Keep(const std::vector<uint8_t>& ids) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [&](const Peer& p) { return std::find(ids.begin(), ids.end(), p.id) == ids.end(); }),
                 peers_.end());
}

void PeerLinks::SetRace(uint16_t raceIndex, const std::vector<uint8_t>& slotIds, uint32_t nowMs) {
    raceOn_ = true;
    raceIndex_ = raceIndex;
    slotIds_ = slotIds;
    for (Peer& peer : peers_) {
        OpenWindow(peer, nowMs);
    }
}

bool PeerLinks::TakePunch(const NetAddress& address) {
    for (Unanswered& u : unanswered_) {
        if (u.address == address) {
            if (u.punches >= kUnansweredPunchCap) {
                return false;
            }
            u.punches++;
            return true;
        }
    }
    unanswered_.push_back({ address, 1 });
    if (unanswered_.size() > kUnansweredMemory) {
        unanswered_.erase(unanswered_.begin());
    }
    return true;
}

void PeerLinks::ForgetUnanswered(const NetAddress& address) {
    unanswered_.erase(std::remove_if(unanswered_.begin(), unanswered_.end(),
                                     [&](const Unanswered& u) { return u.address == address; }),
                      unanswered_.end());
}

void PeerLinks::SendTo(Peer& peer, Candidate& c, uint8_t kind, const uint8_t* body, size_t bodyLen, uint32_t nowMs) {
    const std::vector<uint8_t> bytes =
        BuildDatagram(peer.secret, kind, raceOn_ ? raceIndex_ : 0, localId_, peer.id, body, bodyLen);
    c.lastSentMs = nowMs;
    c.sentOnce = true;
    if (kind == kKindPunch) {
        stats_.punchesSent++;
    } else {
        stats_.posesSent++;
    }
    if (send_) {
        send_(RouteOf(c.kind), c.address, bytes.data(), bytes.size());
    }
}

void PeerLinks::Update(uint32_t nowMs) {
    if (localId_ == lobby::kSystemId) {
        return;
    }
    for (Peer& peer : peers_) {
        const bool open = BestOpen(peer, nowMs) != nullptr;
        if (peer.wasOpen && !open) {
            OpenWindow(peer, nowMs); // the link went silent: find it again
        }
        peer.wasOpen = open;
        if (peer.windowOpen && Reached(nowMs, peer.windowUntilMs)) {
            peer.windowOpen = false;
        }
        const bool punchDue = peer.windowOpen && Reached(nowMs, peer.nextPunchMs);
        for (Candidate& c : peer.candidates) {
            if (!Allowed(c.kind)) {
                continue;
            }
            if (IsOpen(c, nowMs)) {
                if (!c.sentOnce || Elapsed(nowMs, c.lastSentMs) >= kKeepaliveMs) {
                    SendTo(peer, c, kKindPunch, nullptr, 0, nowMs);
                }
            } else if (punchDue && (c.answered || TakePunch(c.address))) {
                SendTo(peer, c, kKindPunch, nullptr, 0, nowMs);
            }
        }
        if (punchDue) {
            peer.nextPunchMs = nowMs + kPunchIntervalMs;
        }
    }
}

void PeerLinks::SendPoses(uint16_t raceIndex, const std::vector<StatePose>& poses, uint32_t nowMs) {
    if (localId_ == lobby::kSystemId || !raceOn_ || raceIndex != raceIndex_ || poses.empty()) {
        return;
    }
    RaceState state;
    state.raceIndex = raceIndex;
    state.poses.assign(poses.begin(),
                       poses.begin() + static_cast<std::ptrdiff_t>(std::min(poses.size(), lobby::kStatePoses)));
    const std::vector<uint8_t> body = lobby::EncodeState(state);
    for (Peer& peer : peers_) {
        Candidate* best = BestOpen(peer, nowMs);
        if (best != nullptr) {
            SendTo(peer, *best, kKindPoses, body.data(), body.size(), nowMs);
        }
    }
}

bool PeerLinks::Receive(Route route, const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs,
                        std::vector<StatePose>& out) {
    const auto refuse = [&]() {
        stats_.refused++;
        return false;
    };
    if (!IsLinkDatagram(data, len) || len < kHeaderBytes + kMacBytes || localId_ == lobby::kSystemId) {
        return refuse();
    }
    const uint8_t kind = data[4];
    const uint16_t race = static_cast<uint16_t>(data[5] | (data[6] << 8));
    const uint8_t fromId = data[7];
    const uint8_t toId = data[8];
    if ((kind != kKindPunch && kind != kKindPoses) || toId != localId_ || fromId == localId_) {
        return refuse();
    }
    Peer* peer = Find(fromId);
    if (peer == nullptr) {
        return refuse();
    }
    uint8_t mac[kMacBytes];
    Mac(peer->secret, data, len - kMacBytes, mac);
    if (crypto_verify16(mac, data + len - kMacBytes) != 0) {
        return refuse();
    }
    Candidate* source = nullptr;
    for (Candidate& c : peer->candidates) {
        if (c.address == from && RouteOf(c.kind) == route && Allowed(c.kind)) {
            source = &c;
            break;
        }
    }
    if (source == nullptr) {
        return refuse(); // a sound datagram from an endpoint the host did not list
    }
    const uint8_t* body = data + kHeaderBytes;
    const size_t bodyLen = len - kHeaderBytes - kMacBytes;
    std::vector<StatePose> poses;
    if (kind == kKindPunch) {
        if (bodyLen != 0) {
            return refuse();
        }
    } else {
        RaceState state;
        if (!lobby::DecodeState(body, bodyLen, state) || state.raceIndex != race) {
            return refuse();
        }
        if (raceOn_ && race == raceIndex_) {
            const auto slot = std::find(slotIds_.begin(), slotIds_.end(), fromId);
            if (slot == slotIds_.end()) {
                return refuse(); // a watcher, or a member who is not in this race
            }
            const size_t own = static_cast<size_t>(slot - slotIds_.begin());
            for (const StatePose& pose : state.poses) {
                if (pose.slot != own) {
                    return refuse(); // a member speaks for its own machine only
                }
            }
            poses = std::move(state.poses);
        }
        // A pose of another race is nobody's fault: the link still answered.
    }
    source->answered = true;
    source->lastValidMs = nowMs;
    ForgetUnanswered(from);
    stats_.posesReceived[static_cast<size_t>(source->kind)] += static_cast<uint32_t>(poses.size());
    out.insert(out.end(), poses.begin(), poses.end());
    return true;
}

} // namespace gdx::net::links
