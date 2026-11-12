// port/net/gdx_net_lobby_session.cpp: LAN room host, client and room browser (gdx_net_lobby_session.h).

#include "gdx_net_lobby_session.h"
#include "gdx_net_friends.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>

namespace gdx::net::lobby {

namespace {

const char* const kTooFastText = "You are sending messages too fast. Wait a moment.";
const char* const kNoSuccessorText = "The host left, and the next host did not answer. The room is closed.";
const char* const kBadSuccessorText = "The host left, and named a next host at an address this copy does not "
                                      "send to. The room is closed.";

uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs; // unsigned: correct across the 49-day wrap
}

// The part of an address a ban holds: an IPv4 address whole, an IPv6 one by its /64, since a host's privacy
// addresses change inside it. A loopback address is never banned: every copy on one PC shares it.
bool BanPrefix(const IpBytes& ip, IpBytes& out) {
    if (IsMappedV4(ip)) {
        out = ip;
        return ip[12] != 127;
    }
    IpBytes loopback{};
    loopback[15] = 1;
    out = ip;
    std::fill(out.begin() + 8, out.end(), static_cast<uint8_t>(0));
    return ip != loopback;
}

// Whether a probe could have come from this network: loopback, private, shared (100.64/10) and link-local
// ranges, and IPv6 unique-local and link-local. The game probes only the broadcast and loopback addresses.
bool LocalSource(const IpBytes& ip) {
    if (IsMappedV4(ip)) {
        const uint32_t v4 = V4Of(ip);
        return (v4 >> 24) == 127 || (v4 >> 24) == 10 || (v4 >> 20) == 0xAC1u || (v4 >> 16) == 0xC0A8u ||
               (v4 >> 22) == 0x191u || (v4 >> 16) == 0xA9FEu;
    }
    IpBytes loopback{};
    loopback[15] = 1;
    return ip == loopback || (ip[0] & 0xFE) == 0xFC || (ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80);
}

uint64_t Rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

void SipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1;
    v1 = Rotl(v1, 13);
    v1 ^= v0;
    v0 = Rotl(v0, 32);
    v2 += v3;
    v3 = Rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = Rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = Rotl(v1, 17);
    v1 ^= v2;
    v2 = Rotl(v2, 32);
}

// SipHash-2-4 (Aumasson and Bernstein): a keyed hash made for short inputs such as this cookie, where a
// stranger must not be able to compute the output for an address it cannot receive on.
// The per-datagram header the byte counters add for a link to this address (O5.3): an IPv6 header is 20 bytes
// longer than an IPv4 one.
uint32_t HeaderBytesFor(const NetAddress& addr) {
    return addr.IsV4() ? kUdpIpv4HeaderBytes : kUdpIpv6HeaderBytes;
}

uint64_t SipHash24(const std::array<uint64_t, 2>& key, const uint8_t* data, size_t len) {
    uint64_t v0 = 0x736f6d6570736575ull ^ key[0];
    uint64_t v1 = 0x646f72616e646f6dull ^ key[1];
    uint64_t v2 = 0x6c7967656e657261ull ^ key[0];
    uint64_t v3 = 0x7465646279746573ull ^ key[1];
    const size_t whole = len - len % 8;
    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m = 0;
        for (int b = 0; b < 8; b++) {
            m |= static_cast<uint64_t>(data[i + b]) << (8 * b);
        }
        v3 ^= m;
        SipRound(v0, v1, v2, v3);
        SipRound(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t last = static_cast<uint64_t>(len & 0xFF) << 56;
    for (size_t b = 0; b < len % 8; b++) {
        last |= static_cast<uint64_t>(data[whole + b]) << (8 * b);
    }
    v3 ^= last;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xFF;
    for (int i = 0; i < 4; i++) {
        SipRound(v0, v1, v2, v3);
    }
    return v0 ^ v1 ^ v2 ^ v3;
}

const PlayerInfo* FindIn(const std::vector<PlayerInfo>& roster, uint8_t id) {
    for (const PlayerInfo& p : roster) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

ShownLine EmoteShown(const EmoteLine& line, const std::vector<PlayerInfo>& roster) {
    ShownLine shown;
    shown.from = line.from;
    shown.text = EmoteText(line.emote);
    if (const PlayerInfo* p = FindIn(roster, line.from)) {
        shown.name = p->name;
        shown.colour = p->colour;
    }
    return shown;
}

std::string JoinedText(const std::string& name, bool watcher) {
    return name + (watcher ? " joined to watch." : " joined the room.");
}

std::string LeftText(const std::string& name) {
    return name + " left the room.";
}

void Push(std::deque<ShownLine>& log, ShownLine line) {
    log.push_back(std::move(line));
    while (log.size() > kLogCap) {
        log.pop_front();
    }
}

// A pose for the game to take. A full box loses its oldest: the newer pose is worth more.
void PushPose(std::deque<StatePose>& box, const StatePose& pose) {
    box.push_back(pose);
    while (box.size() > kPoseBoxCap) {
        box.pop_front();
    }
}

void TakeBox(std::deque<StatePose>& box, std::vector<StatePose>& out) {
    out.assign(box.begin(), box.end());
    box.clear();
}

// Sends the poses in as many State messages as they need, each whole on its own.
void SendState(Channel& channel, uint16_t raceIndex, const std::vector<StatePose>& poses) {
    RaceState state;
    state.raceIndex = raceIndex;
    for (size_t at = 0; at < poses.size(); at += kStatePoses) {
        const size_t end = std::min(poses.size(), at + kStatePoses);
        state.poses.assign(poses.begin() + static_cast<std::ptrdiff_t>(at),
                           poses.begin() + static_cast<std::ptrdiff_t>(end));
        const std::vector<uint8_t> bytes = EncodeState(state);
        channel.SendUnreliable(kMsgState, bytes.data(), bytes.size());
    }
}

void PushNote(std::deque<ShownLine>& log, const std::string& text) {
    ShownLine line;
    line.text = text;
    Push(log, std::move(line));
}

PlayerInfo HostPlayer(const LocalProfile& me) {
    PlayerInfo self;
    self.colour = me.colour;
    self.machine = me.machine;
    self.name = me.name;
    return self;
}

// std::random_device reads the OS source on the toolchains we ship (see gdx_profile_state.cpp).
uint64_t OsToken() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) | static_cast<uint64_t>(device());
}

} // namespace

bool ClockSync::Sample(uint32_t clientMs, uint32_t hostMs, uint32_t nowMs) {
    const uint32_t rtt = Elapsed(nowMs, clientMs);
    if (rtt > kClockStaleMs) {
        return false;
    }
    Entry entry;
    entry.rttMs = rtt;
    entry.offsetMs = hostMs + (rtt / 2) - nowMs; // unsigned: correct across the wrap
    ring_[next_] = entry;
    next_ = (next_ + 1) % ring_.size();
    count_ = std::min(count_ + 1, ring_.size());
    total_++;
    return true;
}

const ClockSync::Entry* ClockSync::Best() const {
    const Entry* best = nullptr;
    for (size_t i = 0; i < count_; i++) {
        if (best == nullptr || ring_[i].rttMs < best->rttMs) {
            best = &ring_[i];
        }
    }
    return best;
}

uint32_t ClockSync::OffsetMs() const {
    const Entry* best = Best();
    return best != nullptr ? best->offsetMs : 0;
}

uint32_t ClockSync::RttMs() const {
    const Entry* best = Best();
    return best != nullptr ? best->rttMs : 0;
}

struct LobbyHost::Peer {
    NetAddress addr;
    std::unique_ptr<Channel> channel;
    uint32_t createdMs = 0;
    bool everConnected = false;
    bool joined = false;
    uint8_t id = kSystemId;
    bool rejecting = false;
    uint32_t rejectMs = 0;
    bool dead = false;
    size_t posesThisUpdate = 0;
    bool nonceSent = false; // O7.3: the Nonce this link's Join must sign
    uint32_t nonceRoom = 0;
    Bytes32 nonce{};
    // Direct member links: this link's serial once joined, the Paths it sent, and the Peers entries it was told.
    uint64_t serial = 0;
    PathsInfo paths;
    bool pathsKnown = false;
    std::vector<PeerEntry> told; // ascending by id
};

LobbyHost::LobbyHost(std::string build, LocalProfile me, uint16_t port, const RoomConfig& config, DatagramSend send,
                     TrackStore store)
    : build_(std::move(build)), me_(std::move(me)), port_(port), send_(std::move(send)),
      machine_(config, HostPlayer(me_)), flow_(std::move(store)), cookieKey_{ OsToken(), OsToken() },
      tokenSource_(OsToken) {
}

LobbyHost::LobbyHost(std::string build, LocalProfile me, uint16_t port, RoomMachine machine,
                     const std::vector<HandoverTokenEntry>& tokens, uint8_t promotedFrom, DatagramSend send,
                     TrackStore store)
    : build_(std::move(build)), me_(std::move(me)), port_(port), send_(std::move(send)), machine_(std::move(machine)),
      flow_(std::move(store)), cookieKey_{ OsToken(), OsToken() }, tokenSource_(OsToken), promotedFrom_(promotedFrom) {
    for (const HandoverTokenEntry& entry : tokens) {
        const uint8_t id = HandoverId(entry.id, promotedFrom);
        if (id != kSystemId && id != kHostId && id < kMaxPlayers && entry.token != 0) {
            rejoinTokens_[id] = entry.token;
        }
    }
}

void LobbyHost::SetTokenSource(std::function<uint64_t()> source) {
    tokenSource_ = source ? std::move(source) : std::function<uint64_t()>(OsToken);
}

uint64_t LobbyHost::NewToken() {
    uint64_t token = 0;
    for (int i = 0; i < 8 && token == 0; i++) {
        token = tokenSource_();
    }
    return token != 0 ? token : 1; // 0 means "none" on the wire; only a broken source gets here
}

LobbyHost::Peer* LobbyHost::JoinedPeer(uint8_t id) const {
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting && !peer->dead && peer->id == id) {
            return peer.get();
        }
    }
    return nullptr;
}

// Each linked member holds a token of its own; the successor holds the table. A member who left could have
// seen the table as an earlier successor, so a departure reissues every token, and so does a successor that
// changes while the old one stays: it knows the table it was sent.
void LobbyHost::SyncTokens() {
    const uint8_t successor = machine_.Successor();
    if (tableTo_ != kSystemId && tableTo_ != successor && JoinedPeer(tableTo_) != nullptr) {
        tokensStale_ = true;
    }
    if (tableTo_ != kSystemId && tableTo_ != successor && !IsZero32(takeoverSecret_)) {
        secretExposed_ = true;
    }
    bool changed = false;
    for (uint8_t id = 1; id < kMaxPlayers; id++) {
        Peer* peer = JoinedPeer(id);
        if (peer == nullptr) {
            changed = changed || tokens_[id] != 0;
            tokens_[id] = 0;
            continue;
        }
        if (tokensStale_ || tokens_[id] == 0) {
            tokens_[id] = NewToken();
            HandoverToken token;
            token.token = tokens_[id];
            const std::vector<uint8_t> bytes = EncodeHandoverToken(token);
            peer->channel->SendReliable(kMsgHandoverToken, bytes.data(), bytes.size());
            changed = true;
        }
    }
    tokensStale_ = false;
    Peer* next = successor != kSystemId ? JoinedPeer(successor) : nullptr;
    if (next == nullptr) {
        tableTo_ = kSystemId;
        return;
    }
    if (!changed && !bansChanged_ && tableTo_ == successor) {
        return;
    }
    std::vector<HandoverTokenEntry> table;
    for (uint8_t id = 1; id < kMaxPlayers; id++) {
        if (id != successor && tokens_[id] != 0) {
            HandoverTokenEntry entry;
            entry.id = id;
            entry.token = tokens_[id];
            table.push_back(entry);
        }
    }
    const std::vector<uint8_t> bytes = EncodeHandoverTokens(table, takeoverSecret_, Bans());
    next->channel->SendReliable(kMsgHandoverTokens, bytes.data(), bytes.size());
    tableTo_ = successor;
    bansChanged_ = false;
}

LobbyHost::~LobbyHost() = default;

void LobbyHost::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (closed_ || data == nullptr) {
        return;
    }
    if (IsDiscovery(data, len)) {
        if (handedOver_) {
            return; // the room is the successor's now
        }
        uint32_t nonce = 0;
        if (!DecodeProbe(data, len, nonce)) {
            refused_++;
            return;
        }
        const RoomState state = machine_.State(nowMs);
        if (state.type != RoomType::Lan && !LocalSource(from.ip)) {
            refused_++; // a directory room tells its name and build to this network only, never to a scanner
            return;
        }
        RoomInfo room;
        room.port = port_;
        room.racers = static_cast<uint8_t>(machine_.Racers());
        room.racerCap = state.racerCap;
        room.watchers = static_cast<uint8_t>(machine_.Watchers());
        room.phase = state.phase;
        room.build = build_;
        room.hostName = me_.name;
        const std::vector<uint8_t> reply = EncodeRoom(nonce, room);
        send_(from, reply.data(), reply.size());
        return;
    }
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->addr == from) {
            peer->channel->Receive(data, len, nowMs);
            return;
        }
    }
    if (handedOver_) {
        return;
    }
    Gate(from, data, len, nowMs);
}

uint64_t LobbyHost::CookieFor(const NetAddress& from, uint32_t nonce, uint32_t bucket) const {
    uint8_t bytes[26];
    size_t n = 0;
    for (uint8_t b : from.ip) {
        bytes[n++] = b;
    }
    bytes[n++] = static_cast<uint8_t>(from.port);
    bytes[n++] = static_cast<uint8_t>(from.port >> 8);
    for (int b = 0; b < 4; b++) {
        bytes[n++] = static_cast<uint8_t>(nonce >> (8 * b));
    }
    for (int b = 0; b < 4; b++) {
        bytes[n++] = static_cast<uint8_t>(bucket >> (8 * b));
    }
    return SipHash24(cookieKey_, bytes, n);
}

// A datagram from an address with no channel (decision 34). Only a hello may open one, and only once it echoes
// the cookie this host gave that address. Until then the host answers without keeping anything, so a flood from
// spoofed addresses costs one reply each, no larger than the hello, and no pending slot.
void LobbyHost::Gate(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    uint32_t nonce = 0;
    uint16_t version = 0;
    uint64_t cookie = 0;
    if (!ParseHello(data, len, nonce, version, cookie)) {
        refused_++;
        return;
    }
    if (version != kProtocolVersion) {
        const std::vector<uint8_t> refuse = EncodeVersionRefuse(nonce, kProtocolVersion);
        send_(from, refuse.data(), refuse.size());
        refused_++;
        return;
    }
    const uint32_t bucket = nowMs / kCookieBucketMs;
    if (cookie != CookieFor(from, nonce, bucket) && cookie != CookieFor(from, nonce, bucket - 1)) {
        const std::vector<uint8_t> challenge = EncodeChallenge(nonce, kProtocolVersion, CookieFor(from, nonce, bucket));
        send_(from, challenge.data(), challenge.size());
        challenged_++;
        return;
    }
    IpBytes source{};
    const bool capped = BanPrefix(from.ip, source);
    size_t pending = 0;
    size_t fromSource = 0;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined) {
            continue;
        }
        pending++;
        IpBytes other{};
        if (capped && BanPrefix(peer->addr.ip, other) && other == source) {
            fromSource++;
        }
    }
    if (pending >= kPendingPeerCap || fromSource >= kPendingPerSourceCap) {
        refused_++;
        return;
    }
    auto peer = std::make_unique<Peer>();
    peer->addr = from;
    peer->createdMs = nowMs;
    ChannelConfig config;
    config.role = Role::Server;
    config.timeoutMs = kLinkTimeoutMs;
    const NetAddress to = from;
    peer->channel =
        std::make_unique<Channel>(config, [this, to](const uint8_t* bytes, size_t n) { send_(to, bytes, n); });
    peer->channel->SetHeaderEstimateBytes(HeaderBytesFor(from));
    peer->channel->Receive(data, len, nowMs);
    peers_.push_back(std::move(peer));
}

void LobbyHost::Update(uint32_t nowMs) {
    if (closed_) {
        return;
    }
    if (handedOver_) {
        LingerAfterHandover(nowMs);
        return;
    }
    // First, so the channel updates below send them in this tick.
    FlushPoses();
    // Index loop: HandleMessage never adds or removes peers, but Reject may mark one.
    for (size_t i = 0; i < peers_.size(); i++) {
        Peer& peer = *peers_[i];
        peer.posesThisUpdate = 0;
        peer.channel->Update(nowMs);
        if (peer.channel->State() == LinkState::Connected) {
            peer.everConnected = true;
            if (!peer.nonceSent && !peer.rejecting) {
                SendNonce(peer);
            }
        }
        Message msg;
        while (peer.channel->PopMessage(msg)) {
            if (!peer.rejecting) {
                HandleMessage(peer, msg, nowMs);
            }
        }
        if (peer.rejecting && Elapsed(nowMs, peer.rejectMs) >= kRejectLingerMs) {
            peer.channel->Disconnect(nowMs);
            peer.dead = true;
        }
        if (peer.channel->State() == LinkState::Disconnected && (peer.everConnected || peer.rejecting)) {
            peer.dead = true;
        }
        if (!peer.joined && !peer.rejecting && Elapsed(nowMs, peer.createdMs) >= kPendingPeerMs) {
            peer.channel->Disconnect(nowMs);
            peer.dead = true;
        }
    }
    std::vector<uint8_t> gone;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->dead && peer->joined) {
            gone.push_back(peer->id);
        }
    }
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->dead) {
            uploadGone_.bytes += peer->channel->Stats().bytesSent;
            uploadGone_.datagrams += peer->channel->Stats().datagramsSent;
        }
    }
    // Drop the dead links first, so the roster that follows goes only to players still here.
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [](const std::unique_ptr<Peer>& p) { return p->dead; }),
                 peers_.end());
    for (uint8_t id : gone) {
        RemovePlayer(id, nowMs);
    }
    if (!RunRejoinWindow(nowMs)) {
        return;
    }

    if (!pinged_ || Elapsed(nowMs, lastPingMs_) >= kPingEveryMs) {
        pinged_ = true;
        lastPingMs_ = nowMs;
        for (const std::unique_ptr<Peer>& peer : peers_) {
            if (peer->joined && !peer->rejecting) {
                const uint32_t rtt = std::min<uint32_t>(peer->channel->Stats().rttMs, 0xFFFF);
                machine_.SetPing(peer->id, static_cast<uint16_t>(rtt));
            }
        }
    }

    std::vector<Outgoing> out;
    machine_.Update(nowMs, out);
    Dispatch(out);
    SyncTrack(nowMs);
    SyncTokens();
    SyncPeers(nowMs);
    if (leaving_ && !closed_ && !RaceOn()) {
        HandOver(nowMs);
    }
}

const Bytes32& LobbyHost::PairSecret(uint64_t a, uint64_t b) {
    const uint64_t low = std::min(a, b);
    const uint64_t high = std::max(a, b);
    for (const PairKey& key : pairSecrets_) {
        if (key.low == low && key.high == high) {
            return key.secret;
        }
    }
    // From the OS source, not the token source: a test that scripts its tokens keeps the same sequence.
    PairKey key;
    key.low = low;
    key.high = high;
    for (size_t i = 0; i < key.secret.size(); i += 8) {
        const uint64_t word = OsToken();
        for (size_t b8 = 0; b8 < 8; b8++) {
            key.secret[i + b8] = static_cast<uint8_t>(word >> (8 * b8));
        }
    }
    if (IsZero32(key.secret)) {
        key.secret[0] = 1; // all zero means forget on the wire
    }
    pairSecrets_.push_back(key);
    return pairSecrets_.back().secret;
}

// Decisions 63, 67 and 68. A member hears of another only once both sent Paths, so a copy with direct links off
// never has its public address told before the host knows it is off. The public address goes only between two
// copies with direct links on; LAN addresses only between two of those behind the same public address; a relay
// address only between two copies that both have one.
void LobbyHost::SyncPeers(uint32_t nowMs) {
    (void)nowMs;
    std::vector<Peer*> linked;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting && !peer->dead && peer->id != kHostId && peer->id < kMaxPlayers) {
            if (peer->serial == 0) {
                peer->serial = nextSerial_++;
                peersDirty_ = true;
            }
            if (peer->pathsKnown) {
                linked.push_back(peer.get());
            }
        }
    }
    std::sort(linked.begin(), linked.end(), [](const Peer* a, const Peer* b) { return a->id < b->id; });
    std::vector<uint64_t> seen;
    for (const Peer* peer : linked) {
        seen.push_back(peer->serial);
    }
    if (!peersDirty_ && seen == peersSeen_) {
        return;
    }
    peersSeen_ = seen;
    peersDirty_ = false;

    for (Peer* x : linked) {
        std::vector<PeerEntry> want;
        for (const Peer* y : linked) {
            if (y == x) {
                continue;
            }
            const bool direct = x->paths.direct && y->paths.direct;
            PeerEntry entry;
            entry.id = y->id;
            if (direct && DialableIp(y->addr.ip, y->addr.port, true)) {
                entry.pub = links::EndpointOf(y->addr);
            }
            if (direct && x->addr.ip == y->addr.ip) {
                entry.lan = y->paths.lan;
            }
            if (!x->paths.relay.None() && !y->paths.relay.None()) {
                entry.relay = y->paths.relay;
            }
            if (entry.pub.None() && entry.relay.None() && entry.lan.empty()) {
                continue;
            }
            entry.secret = PairSecret(x->serial, y->serial);
            want.push_back(entry);
        }
        // What changed: a new or different entry, or a forget for a member still here that lost every path. A
        // member who left is not forgotten here: the roster tells every copy that.
        std::vector<PeerEntry> send;
        for (const PeerEntry& entry : want) {
            const auto was = std::find_if(x->told.begin(), x->told.end(),
                                          [&](const PeerEntry& e) { return e.id == entry.id; });
            if (was == x->told.end() || !(*was == entry)) {
                send.push_back(entry);
            }
        }
        for (const PeerEntry& entry : x->told) {
            const bool wanted = std::any_of(want.begin(), want.end(), [&](const PeerEntry& e) { return e.id == entry.id; });
            const bool here = std::any_of(linked.begin(), linked.end(), [&](const Peer* p) { return p->id == entry.id; });
            if (!wanted && here) {
                PeerEntry forget;
                forget.id = entry.id;
                send.push_back(forget);
            }
        }
        std::sort(send.begin(), send.end(), [](const PeerEntry& a, const PeerEntry& b) { return a.id < b.id; });
        bool sent = true;
        for (size_t at = 0; at < send.size(); at += kPeersPerMessage) {
            const size_t end = std::min(send.size(), at + kPeersPerMessage);
            const std::vector<PeerEntry> part(send.begin() + static_cast<std::ptrdiff_t>(at),
                                              send.begin() + static_cast<std::ptrdiff_t>(end));
            const std::vector<uint8_t> bytes = EncodePeers(part);
            if (x->channel->SendReliable(kMsgPeers, bytes.data(), bytes.size()) != QueueResult::Ok) {
                sent = false;
                break;
            }
        }
        if (sent) {
            x->told = want;
        } else {
            peersDirty_ = true; // a full send queue: the whole diff again next tick
        }
    }

    // A pair whose either side left keeps no secret.
    pairSecrets_.erase(std::remove_if(pairSecrets_.begin(), pairSecrets_.end(),
                                      [&](const PairKey& key) {
                                          const bool low = std::find(seen.begin(), seen.end(), key.low) != seen.end();
                                          const bool high = std::find(seen.begin(), seen.end(), key.high) != seen.end();
                                          return !low || !high;
                                      }),
                       pairSecrets_.end());
}

// A successor that lost only its own link to a living host promotes all the same, and nobody comes back to
// it: the others still hear the old host (decision 22). Closing is how that copy leaves the split.
bool LobbyHost::RunRejoinWindow(uint32_t nowMs) {
    if (promotedFrom_ == kSystemId || windowDone_) {
        return true;
    }
    if (!windowStarted_) {
        windowStarted_ = true;
        windowStartMs_ = nowMs;
        Note("The host left. You host the room now.");
        return true;
    }
    if (Elapsed(nowMs, windowStartMs_) < kSuccessorWaitMs) {
        return true;
    }
    windowDone_ = true;
    rejoinTokens_.fill(0);
    if (rejoined_ == 0) {
        Close(nowMs);
        closeText_ = "Lost the connection to the room.";
        return false;
    }
    std::vector<uint8_t> missing;
    for (const PlayerInfo& p : machine_.Roster()) {
        if (p.id != kHostId && JoinedPeer(p.id) == nullptr) {
            missing.push_back(p.id);
        }
    }
    for (uint8_t id : missing) {
        RemovePlayer(id, nowMs);
    }
    return true;
}

void LobbyHost::ApplyNews(const RoomNews& news, uint32_t nowMs, std::vector<Outgoing>& out) {
    for (uint8_t id : news.held) {
        machine_.TrackHeld(id, nowMs, out);
    }
    if (news.failed) {
        machine_.TrackFailed(nowMs, out);
    }
}

// The flow learns the room's track, asks, offers and reports; then each guest's stream gets what its channel
// has room for. A track the room just dropped is cancelled at the next call.
void LobbyHost::SyncTrack(uint32_t nowMs) {
    const Phase phase = machine_.GetPhase();
    const bool open = phase == Phase::Machine || phase == Phase::Assets;
    std::vector<uint8_t> guests;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting && !peer->dead) {
            guests.push_back(peer->id);
        }
    }
    std::vector<Outgoing> out;
    RoomNews news;
    flow_.Sync(machine_.CurrentTrack(), machine_.TrackOwner(), machine_.State(nowMs).raceIndex, open, guests, nowMs,
               out, news);
    ApplyNews(news, nowMs, out);
    Dispatch(out);
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (!peer->joined || peer->rejecting || peer->dead) {
            continue;
        }
        Channel& channel = *peer->channel;
        flow_.Pump(peer->id, channel.ReliableOutstanding(), nowMs, [&channel](const std::vector<uint8_t>& payload) {
            return channel.SendReliable(kMsgTrackChunk, payload.data(), payload.size()) == QueueResult::Ok;
        });
    }
}

void LobbyHost::HandleMessage(Peer& peer, const Message& msg, uint32_t nowMs) {
    if (!msg.reliable) {
        // The clock ping is the one message that is better lost than late.
        uint32_t clientMs = 0;
        if (msg.type == kMsgClockPing && peer.joined &&
            DecodeClockPing(msg.payload.data(), msg.payload.size(), clientMs)) {
            ClockPong pong;
            pong.clientMs = clientMs;
            pong.hostMs = nowMs;
            const std::vector<uint8_t> bytes = EncodeClockPong(pong);
            peer.channel->SendUnreliable(kMsgClockPong, bytes.data(), bytes.size());
            return;
        }
        if (msg.type == kMsgState && peer.joined) {
            HandleState(peer, msg);
            return;
        }
        refused_++;
        return;
    }
    if (msg.type == kMsgJoin) {
        HandleJoin(peer, msg, nowMs);
        return;
    }
    if (msg.type == kMsgRejoin) {
        HandleRejoin(peer, msg, nowMs);
        return;
    }
    if (!peer.joined) {
        refused_++;
        return;
    }
    const uint8_t* data = msg.payload.data();
    const size_t len = msg.payload.size();
    const uint8_t id = peer.id;
    std::vector<Outgoing> out;
    bool ok = false;
    switch (msg.type) {
    case kMsgReady: {
        bool ready = false;
        ok = DecodeReady(data, len, ready) && machine_.SetReady(id, ready, nowMs, out);
        break;
    }
    case kMsgReach: {
        bool reachable = false;
        ok = DecodeReach(data, len, reachable);
        if (ok) {
            machine_.SetReachable(id, reachable);
        }
        break;
    }
    case kMsgPaths: {
        PathsInfo paths;
        ok = DecodePaths(data, len, paths);
        if (ok && (!peer.pathsKnown || !(peer.paths == paths))) {
            peer.paths = std::move(paths);
            peer.pathsKnown = true;
            peersDirty_ = true;
        }
        break;
    }
    case kMsgVote: {
        TrackChoice choice;
        ok = DecodeVote(data, len, choice) && machine_.Vote(id, choice, nowMs, out);
        break;
    }
    case kMsgPick: {
        TrackChoice choice;
        ok = DecodePick(data, len, choice) && machine_.Pick(id, choice, nowMs, out);
        break;
    }
    case kMsgTrackOffer:
    case kMsgTrackChunk:
    case kMsgTrackHave:
    case kMsgTrackNeed:
    case kMsgTrackAbort: {
        RoomNews news;
        ok = flow_.Receive(id, msg.type, data, len, nowMs, out, news);
        ApplyNews(news, nowMs, out);
        break;
    }
    case kMsgLock: {
        LockRequest lock;
        ok = DecodeLock(data, len, lock) && machine_.Lock(id, lock, nowMs, out);
        break;
    }
    case kMsgEmote: {
        uint8_t emote = 0;
        ok = DecodeEmote(data, len, emote) && machine_.Emote(id, emote, nowMs, out);
        break;
    }
    case kMsgSeatNext: {
        bool raceNext = false;
        ok = DecodeSeatNext(data, len, raceNext) && machine_.SeatNext(id, raceNext, nowMs, out);
        break;
    }
    case kMsgReadyToRace: {
        ReadyToRace ready;
        ok = DecodeReadyToRace(data, len, ready) && machine_.GridReady(id, ready, nowMs, out);
        break;
    }
    case kMsgFinish: {
        FinishReport report;
        ok = DecodeFinish(data, len, report) && machine_.ReportFinish(id, report, nowMs, out);
        break;
    }
    default:
        break;
    }
    if (!ok) {
        refused_++;
    }
    Dispatch(out);
}

void LobbyHost::HandleJoin(Peer& peer, const Message& msg, uint32_t nowMs) {
    if (peer.joined) {
        refused_++;
        return;
    }
    JoinRequest join;
    if (!DecodeJoin(msg.payload.data(), msg.payload.size(), join)) {
        refused_++;
        Reject(peer, RejectReason::BadProfile, nowMs);
        return;
    }
    // No build check since O5.1 (decision 25): the channel's hello already matched the protocol version, and a
    // build that changes any message or the track encoding bumps it. The build string travels for the logs only.
    if (join.hack != me_.hack) {
        Reject(peer, RejectReason::Hack, nowMs);
        return;
    }
    uint8_t id = kSystemId;
    RejectReason why = RejectReason::Full;
    if (!AdmitKey(peer, join, nowMs, why)) {
        Reject(peer, why, nowMs);
        return;
    }
    std::vector<Outgoing> out;
    if (!machine_.Join(join, nowMs, id, why, out)) {
        Reject(peer, why, nowMs);
        return;
    }
    peer.joined = true;
    peer.id = id;
    keys_[id] = join.key;
    machine_.SetAddress(id, peer.addr.ip, peer.addr.port);
    // The welcome goes first: the roster and the room that follow mean nothing without the player's own id.
    const std::vector<uint8_t> welcome = EncodeWelcome(id);
    peer.channel->SendReliable(kMsgWelcome, welcome.data(), welcome.size());
    Dispatch(out);
    SendKeys();
    Note(JoinedText(join.name, join.seat == Seat::Watch));
}

void LobbyHost::SendNonce(Peer& peer) {
    JoinNonce nonce;
    nonce.roomId = machine_.State(0).roomId;
    nonce.hostKey = identity_;
    for (size_t i = 0; i < nonce.nonce.size(); i += 8) {
        const uint64_t word = tokenSource_();
        for (size_t b = 0; b < 8; b++) {
            nonce.nonce[i + b] = static_cast<uint8_t>(word >> (8 * b));
        }
    }
    peer.nonceSent = true;
    peer.nonceRoom = nonce.roomId;
    peer.nonce = nonce.nonce;
    const std::vector<uint8_t> bytes = EncodeNonce(nonce);
    peer.channel->SendReliable(kMsgNonce, bytes.data(), bytes.size());
}

// A key comes with a proof over this link's Nonce, or the join is refused: anyone can claim a key, and only its
// owner can sign with it. The other checks then run on a key the joiner proved it holds. Without a key, only a
// Friends room says no.
bool LobbyHost::AdmitKey(const Peer& peer, const JoinRequest& join, uint32_t nowMs, RejectReason& why) const {
    const bool friendsRoom = machine_.State(nowMs).type == RoomType::Friends;
    IpBytes prefix{};
    if (BanPrefix(peer.addr.ip, prefix) &&
        std::find(bannedIps_.begin(), bannedIps_.end(), prefix) != bannedIps_.end()) {
        why = RejectReason::Banned;
        return false;
    }
    if (IsZero32(join.key)) {
        why = RejectReason::NotFriend;
        return !friendsRoom;
    }
    friends::Key key{};
    friends::Key host{};
    friends::Nonce nonce{};
    friends::Signature proof{};
    std::memcpy(key.data(), join.key.data(), key.size());
    std::memcpy(host.data(), identity_.data(), host.size());
    std::memcpy(nonce.data(), peer.nonce.data(), nonce.size());
    std::memcpy(proof.data(), join.proof.data(), proof.size());
    if (!peer.nonceSent || !friends::JoinProofCheck(key, peer.nonceRoom, host, nonce, proof)) {
        why = RejectReason::BadProof;
        return false;
    }
    if (std::find(banned_.begin(), banned_.end(), join.key) != banned_.end()) {
        why = RejectReason::Banned;
        return false;
    }
    if (friendsRoom && std::find(friends_.begin(), friends_.end(), join.key) == friends_.end()) {
        why = RejectReason::NotFriend;
        return false;
    }
    return true;
}

std::vector<MemberKey> LobbyHost::Keys() const {
    std::vector<MemberKey> keys;
    if (!IsZero32(identity_)) {
        keys.push_back(MemberKey{ kHostId, identity_ });
    }
    for (size_t i = 0; i < kMaxPlayers; i++) {
        const uint8_t id = static_cast<uint8_t>(i);
        if (id != kHostId && !IsZero32(keys_[i]) && FindIn(machine_.Roster(), id) != nullptr) {
            keys.push_back(MemberKey{ id, keys_[i] });
        }
    }
    return keys;
}

void LobbyHost::SendKeys() {
    SendToPlayers(kMsgKeys, EncodeKeys(Keys()));
}

void LobbyHost::SetMirrorKeys(const std::vector<MemberKey>& oldKeys) {
    for (const MemberKey& entry : oldKeys) {
        const uint8_t id = HandoverId(entry.id, promotedFrom_);
        if (id != kSystemId && id != kHostId && id < kMaxPlayers) {
            keys_[id] = entry.key;
        }
    }
}

Bytes32 LobbyHost::NonceFor(const NetAddress& addr) const {
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->addr == addr && peer->nonceSent) {
            return peer->nonce;
        }
    }
    return Bytes32{};
}

bool LobbyHost::Ban(uint8_t id, uint32_t nowMs) {
    if (closed_ || id == kHostId || id >= kMaxPlayers) {
        return false;
    }
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && peer->id == id && !peer->rejecting) {
            const Bytes32 key = keys_[id];
            if (!IsZero32(key) && std::find(banned_.begin(), banned_.end(), key) == banned_.end()) {
                if (banned_.size() >= kBanMax) {
                    banned_.erase(banned_.begin());
                }
                banned_.push_back(key);
                bansChanged_ = true;
            }
            // The address too: a key is dropped by joining with none, which every room but a Friends room admits.
            IpBytes prefix{};
            if (BanPrefix(peer->addr.ip, prefix) &&
                std::find(bannedIps_.begin(), bannedIps_.end(), prefix) == bannedIps_.end()) {
                if (bannedIps_.size() >= kBanMax) {
                    bannedIps_.erase(bannedIps_.begin());
                }
                bannedIps_.push_back(prefix);
                bansChanged_ = true;
            }
            Reject(*peer, RejectReason::Banned, nowMs);
            return true;
        }
    }
    return false;
}

void LobbyHost::SetBans(const BanList& bans) {
    const size_t keyFrom = bans.keys.size() > kBanMax ? bans.keys.size() - kBanMax : 0;
    banned_.assign(bans.keys.begin() + static_cast<std::ptrdiff_t>(keyFrom), bans.keys.end());
    const size_t prefixFrom = bans.prefixes.size() > kBanMax ? bans.prefixes.size() - kBanMax : 0;
    bannedIps_.assign(bans.prefixes.begin() + static_cast<std::ptrdiff_t>(prefixFrom), bans.prefixes.end());
    bansChanged_ = true;
}

// A member of the old room comes back to this promoted host (D28.4). It names the id it had there and the
// token the old host gave it; the table this copy got as successor says which token each seat waits for. A
// token is good once, and only while the window runs: RunRejoinWindow clears the rest when it ends.
void LobbyHost::HandleRejoin(Peer& peer, const Message& msg, uint32_t nowMs) {
    if (peer.joined) {
        refused_++;
        return;
    }
    Rejoin rejoin;
    if (!DecodeRejoin(msg.payload.data(), msg.payload.size(), rejoin)) {
        refused_++;
        Reject(peer, RejectReason::Rejoin, nowMs);
        return;
    }
    const uint8_t id = promotedFrom_ != kSystemId ? HandoverId(rejoin.id, promotedFrom_) : kSystemId;
    const bool seated = id != kHostId && id < kMaxPlayers && FindIn(machine_.Roster(), id) != nullptr;
    if (!seated || rejoinTokens_[id] == 0 || rejoinTokens_[id] != rejoin.token || JoinedPeer(id) != nullptr) {
        Reject(peer, RejectReason::Rejoin, nowMs);
        return;
    }
    rejoinTokens_[id] = 0;
    peer.joined = true;
    peer.id = id;
    machine_.SetAddress(id, peer.addr.ip, peer.addr.port);
    const std::vector<uint8_t> welcome = EncodeWelcome(id);
    peer.channel->SendReliable(kMsgWelcome, welcome.data(), welcome.size());
    std::vector<Outgoing> out;
    machine_.Rejoined(id, nowMs, out);
    Dispatch(out);
    SendKeys();
    rejoined_++;
    Note(NameOf(id) + " is back in the room.");
}

void LobbyHost::Reject(Peer& peer, RejectReason reason, uint32_t nowMs) {
    const std::vector<uint8_t> bytes = EncodeReject(reason);
    peer.channel->SendReliable(kMsgReject, bytes.data(), bytes.size());
    peer.rejecting = true;
    peer.rejectMs = nowMs;
    if (peer.joined) {
        peer.joined = false;
        RemovePlayer(peer.id, nowMs);
    }
}

void LobbyHost::RemovePlayer(uint8_t id, uint32_t nowMs) {
    const std::string name = NameOf(id);
    if (id < kMaxPlayers) {
        keys_[id] = Bytes32{};
    }
    std::vector<Outgoing> out;
    machine_.Leave(id, nowMs, out);
    Dispatch(out);
    SendKeys();
    Note(LeftText(name));
    tokensStale_ = true;
}

bool LobbyHost::SetReady(bool ready, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.SetReady(kHostId, ready, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Vote(uint8_t track, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Vote(kHostId, track, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Vote(const TrackChoice& choice, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Vote(kHostId, choice, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Pick(uint8_t track, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Pick(kHostId, track, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Pick(const TrackChoice& choice, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Pick(kHostId, choice, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Lock(const LockRequest& lock, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Lock(kHostId, lock, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::Emote(uint8_t emote, uint32_t nowMs) {
    if (closed_ || emote >= kEmoteCount) {
        return false;
    }
    std::vector<Outgoing> out;
    const bool ok = machine_.Emote(kHostId, emote, nowMs, out);
    Dispatch(out);
    if (!ok) {
        Note(kTooFastText);
    }
    return ok;
}

bool LobbyHost::GridReady(const ReadyToRace& ready, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.GridReady(kHostId, ready, nowMs, out);
    Dispatch(out);
    return ok;
}

LobbyHost::UploadTotals LobbyHost::Upload() const {
    UploadTotals totals = uploadGone_;
    totals.links = peers_.size();
    for (const std::unique_ptr<Peer>& peer : peers_) {
        totals.bytes += peer->channel->Stats().bytesSent;
        totals.datagrams += peer->channel->Stats().datagramsSent;
    }
    return totals;
}

bool LobbyHost::RaceOn() const {
    const Phase phase = machine_.GetPhase();
    return !machine_.LastGrid().humans.empty() &&
           (phase == Phase::GridSync || phase == Phase::Countdown || phase == Phase::Racing);
}

int LobbyHost::RaceSlotOf(uint8_t id) const {
    const PlayerInfo* player = FindIn(machine_.Roster(), id);
    if (player == nullptr || !player->inRace || player->watcher) {
        return -1;
    }
    const std::vector<GridEntry>& humans = machine_.LastGrid().humans;
    for (size_t i = 0; i < humans.size(); i++) {
        if (humans[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LobbyHost::HandleState(Peer& peer, const Message& msg) {
    RaceState state;
    if (!DecodeState(msg.payload.data(), msg.payload.size(), state)) {
        refused_++;
        return;
    }
    if (!RaceOn() || state.raceIndex != machine_.LastGrid().raceIndex) {
        return; // a pose of a race that is over, still on its way: nobody's fault
    }
    const int slot = RaceSlotOf(peer.id);
    if (slot < 0) {
        refused_++; // a watcher, or a racer who is out of this race
        return;
    }
    for (const StatePose& pose : state.poses) {
        if (pose.slot != slot) {
            refused_++; // a player speaks for their own machine only
            return;
        }
    }
    if (posesRace_ != state.raceIndex) {
        posesOut_.clear();
        posesIn_.clear();
        posesRace_ = state.raceIndex;
    }
    // The lap is taken from every pose, passed on or not: a finish is held to it.
    machine_.NoteLap(peer.id, state.poses.back().lap);
    for (const StatePose& pose : state.poses) {
        if (peer.posesThisUpdate >= kPosesPerPeerPerUpdate || posesOut_.size() >= kPoseBoxCap) {
            return;
        }
        peer.posesThisUpdate++;
        posesOut_.push_back(pose);
        PushPose(posesIn_, pose);
    }
}

void LobbyHost::SendPoses(uint16_t raceIndex, const std::vector<StatePose>& poses) {
    if (closed_ || !RaceOn() || raceIndex != machine_.LastGrid().raceIndex) {
        return;
    }
    const Grid& grid = machine_.LastGrid();
    const int own = RaceSlotOf(kHostId);
    if (posesRace_ != raceIndex) {
        posesOut_.clear();
        posesIn_.clear();
        posesRace_ = raceIndex;
    }
    for (const StatePose& pose : poses) {
        const size_t slot = pose.slot;
        const bool cpu = slot >= grid.humans.size() && slot < grid.humans.size() + grid.cpus;
        if ((pose.slot == own || cpu) && StatePoseOk(pose) && posesOut_.size() < kPoseBoxCap) {
            posesOut_.push_back(pose);
        }
        if (pose.slot == own && StatePoseOk(pose)) {
            machine_.NoteLap(kHostId, pose.lap);
        }
    }
}

bool LobbyHost::Finish(const FinishReport& report, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.ReportFinish(kHostId, report, nowMs, out);
    Dispatch(out);
    return ok;
}

void LobbyHost::SetStanding(const std::vector<uint8_t>& order) {
    if (!closed_) {
        machine_.SetStanding(order);
    }
}

bool LobbyHost::HostLeftRace(uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.HostLeftRace(nowMs, out);
    Dispatch(out);
    return ok;
}

void LobbyHost::TakePoses(uint16_t& raceIndex, std::vector<StatePose>& out) {
    raceIndex = posesRace_;
    TakeBox(posesIn_, out);
}

// Everyone gets every pose but their own machine's. Watchers too: they follow the race from the room.
void LobbyHost::FlushPoses() {
    if (posesOut_.empty()) {
        return;
    }
    if (RaceOn() && posesRace_ == machine_.LastGrid().raceIndex) {
        std::vector<StatePose> forPeer;
        for (const std::unique_ptr<Peer>& peer : peers_) {
            if (!peer->joined || peer->rejecting) {
                continue;
            }
            const int own = RaceSlotOf(peer->id);
            forPeer.clear();
            for (const StatePose& pose : posesOut_) {
                if (pose.slot != own) {
                    forPeer.push_back(pose);
                }
            }
            SendState(*peer->channel, posesRace_, forPeer);
        }
    }
    posesOut_.clear();
}

bool LobbyHost::SetRules(const RaceRules& rules) {
    return !closed_ && machine_.SetRules(rules);
}

bool LobbyHost::Start(uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Start(nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::SkipLottery(uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.SkipLottery(nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::EndSeries(uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.EndSeries(nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::SetRacerCap(uint8_t cap, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.SetRacerCap(cap, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::SetCustomTracks(bool on, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.SetCustomTracks(on, nowMs, out);
    Dispatch(out);
    return ok;
}

bool LobbyHost::SetCpuOptions(const CpuOptions& cpu, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.SetCpuOptions(cpu, nowMs, out);
    Dispatch(out);
    return ok;
}

void LobbyHost::Kick(uint8_t id, uint32_t nowMs) {
    if (closed_ || id == kHostId) {
        return;
    }
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && peer->id == id && !peer->rejecting) {
            Reject(*peer, RejectReason::Kicked, nowMs);
            return;
        }
    }
}

void LobbyHost::Close(uint32_t nowMs) {
    if (closed_) {
        return;
    }
    for (const std::unique_ptr<Peer>& peer : peers_) {
        peer->channel->Disconnect(nowMs);
    }
    peers_.clear();
    closed_ = true;
    closeText_.clear();
}

void LobbyHost::Leave(uint32_t nowMs) {
    if (closed_ || leaving_) {
        return;
    }
    leaving_ = true;
    Note("You left the room.");
    if (!RaceOn()) {
        HandOver(nowMs);
    }
}

void LobbyHost::HandOver(uint32_t nowMs) {
    const uint8_t successor = machine_.Successor();
    Peer* next = successor != kSystemId ? JoinedPeer(successor) : nullptr;
    if (next == nullptr) {
        Close(nowMs);
        closeText_ = "You closed the room. Nobody was left to host it.";
        return;
    }
    Handover handover;
    handover.successor = successor;
    handover.ip = next->addr.ip;
    handover.port = next->addr.port;
    const std::vector<uint8_t> bytes = EncodeHandover(handover);
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting) {
            peer->channel->SendReliable(kMsgHandover, bytes.data(), bytes.size());
        }
    }
    handedOver_ = true;
    handoverMs_ = nowMs;
    handedTo_ = successor;
}

// The members follow the Handover at once and stop answering, so most acks never come: the linger is the
// usual way out. No disconnect goes to anyone. A member that missed the Handover hears silence, and silence
// already leads it to the successor the room named.
void LobbyHost::LingerAfterHandover(uint32_t nowMs) {
    bool settled = true;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        peer->channel->Update(nowMs);
        Message msg;
        while (peer->channel->PopMessage(msg)) {
            // The room is no longer this copy's to run.
        }
        if (peer->joined && peer->channel->State() != LinkState::Disconnected &&
            peer->channel->ReliableOutstanding() > 0) {
            settled = false;
        }
    }
    if (!settled && Elapsed(nowMs, handoverMs_) < kHandoverLingerMs) {
        return;
    }
    const std::string next = NameOf(handedTo_);
    peers_.clear();
    closed_ = true;
    closeText_ = "You left the room. " + next + " hosts it now.";
}

void LobbyHost::Dispatch(std::vector<Outgoing>& out) {
    for (const Outgoing& msg : out) {
        if (msg.type == kMsgEmoteLine) {
            EmoteLine line;
            if (DecodeEmoteLine(msg.payload.data(), msg.payload.size(), line)) {
                Push(lines_, EmoteShown(line, machine_.Roster()));
            }
        }
        if (msg.type == kMsgStart) {
            // The host copy of the game starts on the same message as everyone else.
            haveStart_ = DecodeStart(msg.payload.data(), msg.payload.size(), start_) || haveStart_;
        }
        if (msg.to == kEveryone) {
            SendToPlayers(msg.type, msg.payload);
            continue;
        }
        for (const std::unique_ptr<Peer>& peer : peers_) {
            if (peer->joined && !peer->rejecting && peer->id == msg.to) {
                peer->channel->SendReliable(msg.type, msg.payload.data(), msg.payload.size());
            }
        }
    }
    out.clear();
}

void LobbyHost::SendToPlayers(uint8_t type, const std::vector<uint8_t>& payload) {
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting) {
            peer->channel->SendReliable(type, payload.data(), payload.size());
        }
    }
}

std::string LobbyHost::NameOf(uint8_t id) const {
    const PlayerInfo* p = FindIn(machine_.Roster(), id);
    return p != nullptr ? p->name : std::string();
}

void LobbyHost::Note(const std::string& text) {
    PushNote(lines_, text);
}

namespace {

ChannelConfig ClientConfig(uint32_t nonce) {
    ChannelConfig config;
    config.role = Role::Client;
    config.connectionNonce = nonce;
    config.timeoutMs = kLinkTimeoutMs;
    return config;
}

} // namespace

LobbyClient::LobbyClient(std::string build, LocalProfile me, Seat seat, NetAddress host, uint32_t nonce,
                         DatagramSend send, TrackStore store)
    : build_(std::move(build)), me_(std::move(me)), seat_(seat), host_(host),
      channel_(ClientConfig(nonce), [send, host](const uint8_t* bytes, size_t n) { send(host, bytes, n); }),
      flow_(std::move(store)), relaySend_(std::make_shared<RelaySend>()),
      links_([send, relay = relaySend_](links::Route route, const NetAddress& to, const uint8_t* bytes, size_t n) {
          if (route == links::Route::Direct) {
              send(to, bytes, n);
          } else if (*relay) {
              (*relay)(to, bytes, n);
          }
      }) {
    channel_.SetHeaderEstimateBytes(HeaderBytesFor(host));
}

void LobbyClient::Start(uint32_t nowMs) {
    if (!channel_.Connect(nowMs)) {
        CloseWith("Could not start the connection.");
    }
}

namespace {

// Endpoints the host's decoder takes: a dialable address, or none.
PeerEndpoint Usable(const PeerEndpoint& endpoint) {
    return !endpoint.None() && DialableIp(endpoint.ip, endpoint.port, true) ? endpoint : PeerEndpoint();
}

// The phases the host relays poses in (LobbyHost::RaceOn). The links keep to them as this copy last heard them.
bool PosePhase(Phase phase) {
    return phase == Phase::GridSync || phase == Phase::Countdown || phase == Phase::Racing;
}

} // namespace

void LobbyClient::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (state_ == State::Closed) {
        return;
    }
    if (links::IsLinkDatagram(data, len)) {
        TakeLinkDatagram(links::Route::Direct, from, data, len, nowMs);
        return;
    }
    if (from != host_ || IsDiscovery(data, len)) {
        return;
    }
    channel_.Receive(data, len, nowMs);
}

void LobbyClient::ReceiveRelay(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (state_ != State::Closed && links::IsLinkDatagram(data, len)) {
        TakeLinkDatagram(links::Route::Relay, from, data, len, nowMs);
    }
}

void LobbyClient::TakeLinkDatagram(links::Route route, const NetAddress& from, const uint8_t* data, size_t len,
                                   uint32_t nowMs) {
    if (state_ != State::Joined) {
        return;
    }
    std::vector<StatePose> poses;
    // The links checked the MAC, the race and that every pose is the sender's own slot in it.
    if (!links_.Receive(route, from, data, len, nowMs, poses) || poses.empty() || !haveGrid_ ||
        !PosePhase(room_.phase)) {
        return;
    }
    const int own = OwnSlot();
    const PosePath path = route == links::Route::Relay ? kPathRelay : kPathDirect;
    for (const StatePose& pose : poses) {
        if (pose.slot != own) {
            TakeIn(pose, path, nowMs);
        }
    }
}

// Only a pose newer than the newest one of its slot goes to the game, so the copy that came second by the
// other path is dropped here, where it can still be counted.
void LobbyClient::TakeIn(const StatePose& pose, PosePath path, uint32_t nowMs) {
    if (posesRace_ != grid_.raceIndex) {
        posesIn_.clear();
        posesRace_ = grid_.raceIndex;
    }
    if (pathStats_.raceIndex != grid_.raceIndex) {
        pathStats_ = PosePathStats();
        pathStats_.raceIndex = grid_.raceIndex;
        haveTick_.fill(false);
    }
    const size_t slot = pose.slot;
    if (slot >= kGridSlots) {
        return;
    }
    if (haveTick_[slot] && static_cast<int32_t>(pose.tick - newestTick_[slot]) <= 0) {
        return;
    }
    haveTick_[slot] = true;
    newestTick_[slot] = pose.tick;
    PushPose(posesIn_, pose);
    pathStats_.first[slot][path]++;
    if (clock_.Ready()) {
        const int32_t age = static_cast<int32_t>(nowMs + clock_.OffsetMs() - pose.timeMs);
        pathStats_.ageSumMs[slot][path] += static_cast<uint64_t>(std::max<int32_t>(age, 0));
        pathStats_.aged[slot][path]++;
    }
}

void LobbyClient::SetPaths(const PathsInfo& paths) {
    paths_ = PathsInfo();
    paths_.direct = paths.direct;
    paths_.relay = Usable(paths.relay);
    for (const PeerEndpoint& lan : paths.lan) {
        const PeerEndpoint ok = Usable(lan);
        if (!ok.None() && paths_.lan.size() < kPeerLanMax &&
            std::find(paths_.lan.begin(), paths_.lan.end(), ok) == paths_.lan.end()) {
            paths_.lan.push_back(ok);
        }
    }
    ApplyLocalPaths();
    SyncPaths();
}

void LobbyClient::SetRelaySend(RelaySend send) {
    *relaySend_ = std::move(send);
    ApplyLocalPaths();
}

void LobbyClient::ApplyLocalPaths() {
    if (state_ == State::Joined) {
        links_.SetLocal(localId_, paths_.direct, static_cast<bool>(*relaySend_) && !paths_.relay.None());
    }
}

void LobbyClient::SyncPaths() {
    if (state_ != State::Joined || (pathsSent_ && pathsTold_ == paths_)) {
        return;
    }
    const std::vector<uint8_t> bytes = EncodePaths(paths_);
    if (channel_.SendReliable(kMsgPaths, bytes.data(), bytes.size()) == QueueResult::Ok) {
        pathsSent_ = true;
        pathsTold_ = paths_;
    }
}

void LobbyClient::Update(uint32_t nowMs) {
    if (state_ == State::Closed) {
        return;
    }
    lastNowMs_ = nowMs;
    channel_.Update(nowMs);
    if (rejoin_ && state_ == State::Connecting && Elapsed(nowMs, rejoinSinceMs_) >= kSuccessorWaitMs) {
        channel_.Disconnect(nowMs);
        CloseWith(kNoSuccessorText);
        return;
    }
    if (channel_.State() == LinkState::Connected) {
        everConnected_ = true;
        if (!joinSent_ && rejoin_) {
            Rejoin rejoin;
            rejoin.id = rejoinId_;
            rejoin.token = rejoinToken_;
            const std::vector<uint8_t> bytes = EncodeRejoin(rejoin);
            channel_.SendReliable(kMsgRejoin, bytes.data(), bytes.size());
            hostReachable_ = false; // a promoted host built its seats from its mirror, which holds no reachability
            joinSent_ = true;
        }
        // A Join waits for the host's Nonce (HandleMessage), which it signs.
    }
    Message msg;
    while (state_ != State::Closed && channel_.PopMessage(msg)) {
        HandleMessage(msg, nowMs);
    }
    if (state_ == State::Closed) {
        channel_.Disconnect(nowMs);
        return;
    }
    if (state_ == State::Joined) {
        SyncReach(); // again, in case a full send queue refused it
        SyncPaths();
        links_.Update(nowMs);
        SyncTrack(nowMs);
        flow_.Pump(channel_.ReliableOutstanding(), nowMs, [this](const std::vector<uint8_t>& payload) {
            return channel_.SendReliable(kMsgTrackChunk, payload.data(), payload.size()) == QueueResult::Ok;
        });
        const uint32_t every = clock_.Samples() < kClockWarmSamples ? kClockPingFastMs : kClockPingSlowMs;
        if (!clockPinged_ || Elapsed(nowMs, lastClockPingMs_) >= every) {
            clockPinged_ = true;
            lastClockPingMs_ = nowMs;
            const std::vector<uint8_t> bytes = EncodeClockPing(nowMs);
            channel_.SendUnreliable(kMsgClockPing, bytes.data(), bytes.size());
        }
    }
    if (channel_.State() != LinkState::Disconnected) {
        return;
    }
    switch (channel_.Reason()) {
    case CloseReason::Timeout:
        if (rejoin_ && !everConnected_) {
            // The successor may not be hosting yet: it learns the host is gone on its own clock.
            if (Elapsed(nowMs, rejoinSinceMs_) < kSuccessorWaitMs && channel_.Connect(nowMs)) {
                return;
            }
            CloseWith(kNoSuccessorText);
            break;
        }
        if (state_ == State::Joined && !PlanHandover(room_.successor, room_.successorIp, room_.successorPort, nowMs)) {
            CloseWith(kBadSuccessorText);
            break;
        }
        CloseWith(everConnected_ ? "Lost the connection to the host."
                                 : "No room answered at " + FormatAddress(host_) + ".");
        break;
    case CloseReason::Closed:
        CloseWith("The host closed the room.");
        break;
    case CloseReason::VersionMismatch:
        CloseWith("The host runs a different G-Diffuser build. Both players need the same version.");
        break;
    case CloseReason::Busy:
        CloseWith("The host is busy. Try again in a moment.");
        break;
    default:
        CloseWith("The connection to the host broke.");
        break;
    }
}

void LobbyClient::HandleMessage(const Message& msg, uint32_t nowMs) {
    if (!msg.reliable) {
        ClockPong pong;
        if (msg.type == kMsgClockPong && DecodeClockPong(msg.payload.data(), msg.payload.size(), pong)) {
            clock_.Sample(pong.clientMs, pong.hostMs, nowMs);
            return;
        }
        if (msg.type == kMsgState) {
            HandleState(msg);
            return;
        }
        refused_++;
        return;
    }
    const uint8_t* data = msg.payload.data();
    const size_t len = msg.payload.size();
    switch (msg.type) {
    case kMsgNonce: {
        JoinNonce nonce;
        if (joinSent_ || state_ != State::Connecting || !DecodeNonce(data, len, nonce)) {
            refused_ += joinSent_ ? 0 : 1; // a Rejoin answers no Nonce: the token is its proof
            return;
        }
        if (expectingKey_ && nonce.hostKey != expectKey_) {
            CloseWith("The room at that address is not the room you asked for.");
            return;
        }
        JoinRequest join;
        join.build = build_;
        join.name = me_.name;
        join.colour = me_.colour;
        join.machine = me_.machine;
        join.seat = seat_;
        join.hack = me_.hack;
        join.reachable = reachable_;
        if (!signer_ || !signer_(nonce, join.key, join.proof)) {
            join.key = Bytes32{};
            join.proof.fill(0);
        }
        const std::vector<uint8_t> bytes = EncodeJoin(join);
        channel_.SendReliable(kMsgJoin, bytes.data(), bytes.size());
        hostReachable_ = join.reachable;
        joinSent_ = true;
        return;
    }
    case kMsgKeys: {
        std::vector<MemberKey> keys;
        if (!DecodeKeys(data, len, keys)) {
            refused_++;
            return;
        }
        keys_ = std::move(keys);
        return;
    }
    case kMsgWelcome: {
        uint8_t id = 0;
        if (state_ != State::Connecting || !DecodeWelcome(data, len, id)) {
            refused_++;
            return;
        }
        localId_ = id;
        state_ = State::Joined;
        SyncReach();
        ApplyLocalPaths();
        SyncPaths();
        return;
    }
    case kMsgPeers: {
        std::vector<PeerEntry> entries;
        if (state_ != State::Joined || !DecodePeers(data, len, entries)) {
            refused_++;
            return;
        }
        links_.Apply(entries, nowMs);
        return;
    }
    case kMsgReject: {
        RejectReason reason = RejectReason::BadProfile;
        if (!DecodeReject(data, len, reason)) {
            refused_++;
            CloseWith("The host refused the join.");
            return;
        }
        CloseWith(RejectReasonText(reason));
        return;
    }
    case kMsgRoster: {
        std::vector<PlayerInfo> roster;
        if (!DecodeRoster(data, len, roster)) {
            refused_++;
            return;
        }
        TakeRoster(std::move(roster));
        return;
    }
    case kMsgRoom: {
        RoomState room;
        if (!DecodeRoomState(data, len, room)) {
            refused_++;
            return;
        }
        room_ = room;
        roomAtMs_ = nowMs;
        return;
    }
    case kMsgGrid: {
        Grid grid;
        if (!DecodeGrid(data, len, grid)) {
            refused_++;
            return;
        }
        grid_ = std::move(grid);
        haveGrid_ = true;
        std::vector<uint8_t> slotIds;
        for (const GridEntry& human : grid_.humans) {
            slotIds.push_back(human.id);
        }
        links_.SetRace(grid_.raceIndex, slotIds, nowMs);
        return;
    }
    case kMsgStart: {
        StartSignal start;
        if (!DecodeStart(data, len, start)) {
            refused_++;
            return;
        }
        start_ = start;
        haveStart_ = true;
        return;
    }
    case kMsgVotes: {
        std::vector<Ballot> votes;
        if (!DecodeVotes(data, len, votes)) {
            refused_++;
            return;
        }
        votes_ = std::move(votes);
        return;
    }
    case kMsgLottery: {
        LotteryResult lottery;
        if (!DecodeLottery(data, len, lottery)) {
            refused_++;
            return;
        }
        // The room message that opens Phase::Lottery comes after this one, but the values the draw rests
        // on have not changed since the vote opened.
        lotteryChecked_ = CheckLottery(room_, lottery);
        if (!lotteryChecked_) {
            refused_++;
            Note("The host's lottery result does not match this copy's check.");
        }
        lottery_ = std::move(lottery);
        return;
    }
    case kMsgPicked: {
        Picked picked;
        if (!DecodePicked(data, len, picked)) {
            refused_++;
            return;
        }
        picked_ = picked;
        return;
    }
    case kMsgEmoteLine: {
        EmoteLine line;
        if (!DecodeEmoteLine(data, len, line)) {
            refused_++;
            return;
        }
        Push(lines_, EmoteShown(line, roster_));
        return;
    }
    case kMsgResults: {
        RaceResults results;
        if (!DecodeResults(data, len, results)) {
            refused_++;
            return;
        }
        results_ = std::move(results);
        haveResults_ = true;
        return;
    }
    case kMsgHandoverToken: {
        HandoverToken token;
        if (!DecodeHandoverToken(data, len, token)) {
            refused_++;
            return;
        }
        ownToken_ = token.token;
        return;
    }
    case kMsgHandoverTokens: {
        std::vector<HandoverTokenEntry> table;
        Bytes32 secret{};
        BanList bans;
        if (!DecodeHandoverTokens(data, len, table, secret, bans)) {
            refused_++;
            return;
        }
        tokenTable_ = std::move(table);
        takeoverSecret_ = secret;
        bans_ = std::move(bans);
        return;
    }
    case kMsgHandover: {
        // The host is leaving on purpose: no need to wait out the silence.
        Handover handover;
        if (state_ != State::Joined || !DecodeHandover(data, len, handover)) {
            refused_++;
            return;
        }
        if (!PlanHandover(handover.successor, handover.ip, handover.port, nowMs)) {
            CloseWith(kBadSuccessorText);
            return;
        }
        CloseWith(handover.successor == kSystemId ? "The host closed the room." : "The host left the room.");
        return;
    }
    case kMsgTrackOffer:
    case kMsgTrackChunk:
    case kMsgTrackHave:
    case kMsgTrackNeed:
    case kMsgTrackAbort: {
        // The room message ahead of this one may have named the track in this same Update.
        SyncTrack(nowMs);
        std::vector<Outgoing> out;
        if (!flow_.Receive(msg.type, data, len, nowMs, out)) {
            refused_++;
        }
        for (const Outgoing& reply : out) {
            Send(reply.type, reply.payload);
        }
        return;
    }
    default:
        refused_++;
        return;
    }
}

void LobbyClient::TakeRoster(std::vector<PlayerInfo> roster) {
    // The first roster is the room as this player found it: nobody in it just joined.
    if (haveRoster_) {
        for (const PlayerInfo& was : roster_) {
            const PlayerInfo* now = FindIn(roster, was.id);
            if (now == nullptr || now->name != was.name) {
                Note(LeftText(was.name));
            }
        }
        for (const PlayerInfo& now : roster) {
            const PlayerInfo* was = FindIn(roster_, now.id);
            if (was == nullptr || was->name != now.name) {
                Note(JoinedText(now.name, now.watcher));
            }
        }
    }
    roster_ = std::move(roster);
    haveRoster_ = true;
    std::vector<uint8_t> ids;
    for (const PlayerInfo& player : roster_) {
        ids.push_back(player.id);
    }
    links_.Keep(ids);
}

// The successor promotes only with someone to host: its table names every other member that holds a token.
// A member without a token, or a room with nobody named, has nothing to come back to.
bool LobbyClient::PlanHandover(uint8_t successor, const IpBytes& ip, uint16_t port, uint32_t nowMs) {
    plan_ = HandoverPlan();
    plan_.successor = successor;
    plan_.id = localId_;
    plan_.sinceMs = nowMs;
    if (successor == kSystemId || localId_ == kSystemId) {
        return true;
    }
    plan_.address.ip = ip;
    plan_.address.port = port;
    if (successor == localId_) {
        plan_.kind = tokenTable_.empty() ? HandoverPlan::Kind::None : HandoverPlan::Kind::Promote;
        return true;
    }
    if (ownToken_ == 0 || port == 0) {
        return true;
    }
    // Decision 60: the host's word is the only source of this address, as a record is of a listed one.
    if (room_.type != RoomType::Lan && !DialableIp(ip, port, allowLoopback_)) {
        return false;
    }
    plan_.kind = HandoverPlan::Kind::Rejoin;
    plan_.token = ownToken_;
    return true;
}

void LobbyClient::SetReachable(bool reachable) {
    reachable_ = reachable;
    SyncReach();
}

void LobbyClient::SyncReach() {
    if (state_ != State::Joined || reachable_ == hostReachable_) {
        return;
    }
    const std::vector<uint8_t> bytes = EncodeReach(reachable_);
    if (channel_.SendReliable(kMsgReach, bytes.data(), bytes.size()) == QueueResult::Ok) {
        hostReachable_ = reachable_;
    }
}

bool LobbyClient::SetRejoin(const HandoverPlan& plan) {
    if (plan.kind != HandoverPlan::Kind::Rejoin || plan.address != host_ || plan.token == 0 ||
        state_ != State::Connecting || channel_.State() != LinkState::Disconnected || joinSent_) {
        return false;
    }
    rejoin_ = true;
    rejoinId_ = plan.id;
    rejoinToken_ = plan.token;
    rejoinSinceMs_ = plan.sinceMs;
    return true;
}

RoomMirror LobbyClient::Mirror(uint32_t nowMs) const {
    RoomMirror mirror;
    mirror.roster = roster_;
    mirror.room = Room(nowMs);
    mirror.results = results_;
    mirror.haveResults = haveResults_;
    return mirror;
}

RoomState LobbyClient::Room(uint32_t nowMs) const {
    RoomState room = room_;
    const uint32_t spent = Elapsed(nowMs, roomAtMs_);
    room.phaseLeftMs = spent < room.phaseLeftMs ? room.phaseLeftMs - spent : 0;
    return room;
}

void LobbyClient::Send(uint8_t type, const std::vector<uint8_t>& payload) {
    if (state_ == State::Joined) {
        channel_.SendReliable(type, payload.data(), payload.size());
    }
}

void LobbyClient::SetReady(bool ready) {
    Send(kMsgReady, EncodeReady(ready));
}

void LobbyClient::Vote(uint8_t track) {
    Send(kMsgVote, EncodeVote(track));
}

void LobbyClient::Vote(const TrackChoice& choice) {
    Send(kMsgVote, EncodeVote(choice));
}

void LobbyClient::Pick(uint8_t track) {
    Send(kMsgPick, EncodePick(track));
}

void LobbyClient::Pick(const TrackChoice& choice) {
    Send(kMsgPick, EncodePick(choice));
}

bool LobbyClient::OwnsTrack() const {
    if (room_.track != kTrackCustom) {
        return false;
    }
    if (picked_.track == kTrackCustom && picked_.by == localId_ && picked_.custom.id == room_.custom.id) {
        return true;
    }
    return lottery_.track == kTrackCustom && lottery_.winner < lottery_.ballots.size() &&
           lottery_.ballots[lottery_.winner].id == localId_ && lottery_.custom.id == room_.custom.id;
}

void LobbyClient::SyncTrack(uint32_t nowMs) {
    TrackChoice track;
    track.track = room_.track;
    track.custom = room_.custom;
    const bool open = room_.phase == Phase::Machine || room_.phase == Phase::Assets;
    flow_.Sync(track, OwnsTrack(), room_.raceIndex, open, nowMs);
}

void LobbyClient::Lock(const LockRequest& lock) {
    Send(kMsgLock, EncodeLock(lock));
}

bool LobbyClient::Emote(uint8_t emote, uint32_t nowMs) {
    if (state_ != State::Joined || emote >= kEmoteCount) {
        return false;
    }
    if (!EmoteAllowed(emoteTimes_, nowMs)) {
        Note(kTooFastText);
        return false;
    }
    Send(kMsgEmote, EncodeEmote(emote));
    return true;
}

void LobbyClient::RaceNext(bool raceNext) {
    Send(kMsgSeatNext, EncodeSeatNext(raceNext));
}

void LobbyClient::GridReady(const ReadyToRace& ready) {
    Send(kMsgReadyToRace, EncodeReadyToRace(ready));
}

void LobbyClient::Finish(const FinishReport& report) {
    Send(kMsgFinish, EncodeFinish(report));
}

int LobbyClient::OwnSlot() const {
    if (!haveGrid_) {
        return -1;
    }
    for (size_t i = 0; i < grid_.humans.size(); i++) {
        if (grid_.humans[i].id == localId_) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LobbyClient::HandleState(const Message& msg) {
    RaceState state;
    if (!DecodeState(msg.payload.data(), msg.payload.size(), state)) {
        refused_++;
        return;
    }
    if (!haveGrid_ || state.raceIndex != grid_.raceIndex) {
        return; // a pose of a race that is over, still on its way
    }
    const int own = OwnSlot();
    const size_t racers = grid_.humans.size() + grid_.cpus;
    for (const StatePose& pose : state.poses) {
        if (static_cast<size_t>(pose.slot) >= racers) {
            refused_++; // no such machine in this race
            return;
        }
        if (pose.slot != own) {
            TakeIn(pose, kPathHost, lastNowMs_);
        }
    }
}

void LobbyClient::SendPoses(uint16_t raceIndex, const std::vector<StatePose>& poses) {
    const int own = OwnSlot();
    if (state_ != State::Joined || own < 0 || raceIndex != grid_.raceIndex) {
        return;
    }
    std::vector<StatePose> mine;
    for (const StatePose& pose : poses) {
        if (pose.slot == own && StatePoseOk(pose)) {
            mine.push_back(pose);
        }
    }
    SendState(channel_, raceIndex, mine);
    if (PosePhase(room_.phase)) {
        links_.SendPoses(raceIndex, mine, lastNowMs_);
    }
}

void LobbyClient::TakePoses(uint16_t& raceIndex, std::vector<StatePose>& out) {
    raceIndex = posesRace_;
    TakeBox(posesIn_, out);
}

void LobbyClient::Leave(uint32_t nowMs) {
    if (state_ == State::Closed) {
        return;
    }
    channel_.Disconnect(nowMs);
    state_ = State::Closed;
    closeText_.clear();
    links_.Clear();
}

void LobbyClient::Note(const std::string& text) {
    PushNote(lines_, text);
}

void LobbyClient::CloseWith(const std::string& text) {
    state_ = State::Closed;
    closeText_ = text;
    links_.Clear();
}

LanBrowser::LanBrowser(std::vector<NetAddress> targets, uint32_t nonce, DatagramSend send)
    : targets_(std::move(targets)), nonce_(nonce), send_(std::move(send)) {
}

void LanBrowser::Update(uint32_t nowMs) {
    if (!probed_ || Elapsed(nowMs, lastProbeMs_) >= kProbeEveryMs) {
        const std::vector<uint8_t> probe = EncodeProbe(nonce_);
        for (const NetAddress& target : targets_) {
            send_(target, probe.data(), probe.size());
        }
        probed_ = true;
        lastProbeMs_ = nowMs;
    }
    rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(),
                                [nowMs](const FoundRoom& r) { return Elapsed(nowMs, r.lastSeenMs) >= kRoomExpireMs; }),
                 rooms_.end());
}

bool LanBrowser::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    uint32_t nonce = 0;
    RoomInfo room;
    if (!DecodeRoom(data, len, nonce, room) || nonce != nonce_) {
        return false;
    }
    NetAddress address;
    address.ip = from.ip;
    address.port = room.port;
    for (FoundRoom& found : rooms_) {
        if (found.address == address) {
            found.room = room;
            found.lastSeenMs = nowMs;
            return true;
        }
    }
    if (rooms_.size() < kRoomListCap) {
        FoundRoom found;
        found.address = address;
        found.room = room;
        found.lastSeenMs = nowMs;
        rooms_.push_back(found);
    }
    return true;
}

} // namespace gdx::net::lobby
