// port/net/gdx_net_lobby_session.cpp: O2 LAN lobby host, client and room browser (gdx_net_lobby_session.h).

#include "gdx_net_lobby_session.h"

#include <algorithm>
#include <utility>

namespace gdx::net::lobby {

namespace {

uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs; // unsigned: correct across the 49-day wrap
}

// Every channel packet starts with "GD"; anything else from an unknown address never gets a channel.
bool LooksLikeChannelPacket(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 2 && data[0] == 'G' && data[1] == 'D';
}

ShownLine Resolve(const ChatLine& line, const std::vector<PlayerInfo>& roster) {
    ShownLine shown;
    shown.from = line.from;
    shown.text = line.text;
    for (const PlayerInfo& p : roster) {
        if (p.id == line.from) {
            shown.name = p.name;
            shown.colour = p.colour;
            break;
        }
    }
    return shown;
}

void Push(std::deque<ShownLine>& log, ShownLine line) {
    log.push_back(std::move(line));
    while (log.size() > kChatLogCap) {
        log.pop_front();
    }
}

} // namespace

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
    std::deque<uint32_t> chatTimes;
};

LobbyHost::LobbyHost(std::string build, LocalProfile me, uint16_t port, DatagramSend send)
    : build_(std::move(build)), me_(std::move(me)), port_(port), send_(std::move(send)) {
    PlayerInfo self;
    self.id = kHostId;
    self.host = true;
    self.colour = me_.colour;
    self.machine = me_.machine;
    self.name = me_.name;
    roster_.push_back(self);
}

LobbyHost::~LobbyHost() = default;

void LobbyHost::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (closed_ || data == nullptr) {
        return;
    }
    if (IsDiscovery(data, len)) {
        uint32_t nonce = 0;
        if (!DecodeProbe(data, len, nonce)) {
            refused_++;
            return;
        }
        RoomInfo room;
        room.port = port_;
        room.players = static_cast<uint8_t>(roster_.size());
        room.cap = static_cast<uint8_t>(kMaxPlayers);
        room.open = roster_.size() < kMaxPlayers;
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
    if (!LooksLikeChannelPacket(data, len)) {
        refused_++;
        return;
    }
    size_t pending = 0;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        pending += peer->joined ? 0 : 1;
    }
    if (pending >= kPendingPeerCap) {
        refused_++;
        return;
    }
    auto peer = std::make_unique<Peer>();
    peer->addr = from;
    peer->createdMs = nowMs;
    ChannelConfig config;
    config.role = Role::Server;
    const NetAddress to = from;
    peer->channel =
        std::make_unique<Channel>(config, [this, to](const uint8_t* bytes, size_t n) { send_(to, bytes, n); });
    peer->channel->Receive(data, len, nowMs);
    peers_.push_back(std::move(peer));
}

void LobbyHost::Update(uint32_t nowMs) {
    if (closed_) {
        return;
    }
    // Index loop: HandleMessage never adds or removes peers, but Reject may mark one.
    for (size_t i = 0; i < peers_.size(); i++) {
        Peer& peer = *peers_[i];
        peer.channel->Update(nowMs);
        if (peer.channel->State() == LinkState::Connected) {
            peer.everConnected = true;
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
    std::vector<std::string> left;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->dead && peer->joined) {
            const PlayerInfo* player = FindPlayer(peer->id);
            left.push_back(player != nullptr ? player->name : std::string());
            RemovePlayer(peer->id);
        }
    }
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [](const std::unique_ptr<Peer>& p) { return p->dead; }),
                 peers_.end());
    if (!left.empty()) {
        BroadcastRoster();
        for (const std::string& name : left) {
            Notice(name + " left the room.");
        }
    }
}

void LobbyHost::HandleMessage(Peer& peer, const Message& msg, uint32_t nowMs) {
    if (!msg.reliable) {
        refused_++;
        return;
    }
    if (msg.type == kMsgJoin) {
        HandleJoin(peer, msg, nowMs);
        return;
    }
    PlayerInfo* player = peer.joined ? FindPlayer(peer.id) : nullptr;
    if (player == nullptr) {
        refused_++;
        return;
    }
    if (msg.type == kMsgReady) {
        bool ready = false;
        if (!DecodeReady(msg.payload.data(), msg.payload.size(), ready)) {
            refused_++;
            return;
        }
        if (player->ready != ready) {
            player->ready = ready;
            BroadcastRoster();
        }
        return;
    }
    if (msg.type == kMsgChat) {
        std::string text;
        if (!DecodeChat(msg.payload.data(), msg.payload.size(), text)) {
            refused_++;
            return;
        }
        while (!peer.chatTimes.empty() && Elapsed(nowMs, peer.chatTimes.front()) >= kChatWindowMs) {
            peer.chatTimes.pop_front();
        }
        if (peer.chatTimes.size() >= kChatBurst) {
            refused_++;
            ChatLine warn;
            warn.text = "You are sending messages too fast. Wait a moment.";
            const std::vector<uint8_t> bytes = EncodeChatLine(warn);
            peer.channel->SendReliable(kMsgChatLine, bytes.data(), bytes.size());
            return;
        }
        peer.chatTimes.push_back(nowMs);
        ChatLine line;
        line.from = peer.id;
        line.text = text;
        Relay(line);
        return;
    }
    refused_++;
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
    if (join.build != build_) {
        Reject(peer, RejectReason::Build, nowMs);
        return;
    }
    if (roster_.size() >= kMaxPlayers) {
        Reject(peer, RejectReason::Full, nowMs);
        return;
    }
    PlayerInfo player;
    player.id = FreeId();
    player.colour = join.colour;
    player.machine = join.machine;
    player.name = join.name;
    roster_.push_back(player);
    std::sort(roster_.begin(), roster_.end(), [](const PlayerInfo& a, const PlayerInfo& b) { return a.id < b.id; });
    peer.joined = true;
    peer.id = player.id;

    const std::vector<uint8_t> welcome = EncodeWelcome(player.id);
    peer.channel->SendReliable(kMsgWelcome, welcome.data(), welcome.size());
    BroadcastRoster();
    Notice(player.name + " joined the room.");
}

void LobbyHost::Reject(Peer& peer, RejectReason reason, uint32_t nowMs) {
    const std::vector<uint8_t> bytes = EncodeReject(reason);
    peer.channel->SendReliable(kMsgReject, bytes.data(), bytes.size());
    peer.rejecting = true;
    peer.rejectMs = nowMs;
    if (peer.joined) {
        const PlayerInfo* player = FindPlayer(peer.id);
        const std::string name = player != nullptr ? player->name : std::string();
        RemovePlayer(peer.id);
        peer.joined = false;
        BroadcastRoster();
        Notice(name + " left the room.");
    }
}

void LobbyHost::SetReady(bool ready) {
    if (closed_ || roster_.empty() || roster_[0].ready == ready) {
        return;
    }
    roster_[0].ready = ready;
    BroadcastRoster();
}

bool LobbyHost::Chat(const std::string& text) {
    const std::string clean = SanitizeChat(text);
    if (closed_ || clean.empty()) {
        return false;
    }
    ChatLine line;
    line.from = kHostId;
    line.text = clean;
    Relay(line);
    return true;
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
    roster_.resize(1);
    closed_ = true;
}

void LobbyHost::Relay(const ChatLine& line) {
    Log(line);
    SendToPlayers(kMsgChatLine, EncodeChatLine(line));
}

void LobbyHost::Notice(const std::string& text) {
    ChatLine line;
    line.from = kSystemId;
    line.text = SanitizeChat(text);
    if (line.text.empty()) {
        return;
    }
    Relay(line);
}

void LobbyHost::BroadcastRoster() {
    SendToPlayers(kMsgRoster, EncodeRoster(roster_));
}

void LobbyHost::SendToPlayers(uint8_t type, const std::vector<uint8_t>& payload) {
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->joined && !peer->rejecting) {
            peer->channel->SendReliable(type, payload.data(), payload.size());
        }
    }
}

void LobbyHost::RemovePlayer(uint8_t id) {
    roster_.erase(std::remove_if(roster_.begin(), roster_.end(),
                                 [id](const PlayerInfo& p) { return p.id == id && !p.host; }),
                  roster_.end());
}

uint8_t LobbyHost::FreeId() const {
    for (uint8_t id = 1; id < kMaxPlayers; id++) {
        bool used = false;
        for (const PlayerInfo& p : roster_) {
            used = used || p.id == id;
        }
        if (!used) {
            return id;
        }
    }
    return kSystemId; // unreachable: HandleJoin checks the seat count first
}

PlayerInfo* LobbyHost::FindPlayer(uint8_t id) {
    for (PlayerInfo& p : roster_) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

void LobbyHost::Log(const ChatLine& line) {
    Push(chat_, Resolve(line, roster_));
}

namespace {

ChannelConfig ClientConfig(uint32_t nonce) {
    ChannelConfig config;
    config.role = Role::Client;
    config.connectionNonce = nonce;
    return config;
}

} // namespace

LobbyClient::LobbyClient(std::string build, LocalProfile me, NetAddress host, uint32_t nonce, DatagramSend send)
    : build_(std::move(build)), me_(std::move(me)), host_(host),
      channel_(ClientConfig(nonce), [send, host](const uint8_t* bytes, size_t n) { send(host, bytes, n); }) {
}

void LobbyClient::Start(uint32_t nowMs) {
    if (!channel_.Connect(nowMs)) {
        CloseWith("Could not start the connection.");
    }
}

void LobbyClient::Receive(const NetAddress& from, const uint8_t* data, size_t len, uint32_t nowMs) {
    if (state_ == State::Closed || from != host_ || IsDiscovery(data, len)) {
        return;
    }
    channel_.Receive(data, len, nowMs);
}

void LobbyClient::Update(uint32_t nowMs) {
    if (state_ == State::Closed) {
        return;
    }
    channel_.Update(nowMs);
    if (channel_.State() == LinkState::Connected) {
        everConnected_ = true;
        if (!joinSent_) {
            JoinRequest join;
            join.build = build_;
            join.name = me_.name;
            join.colour = me_.colour;
            join.machine = me_.machine;
            const std::vector<uint8_t> bytes = EncodeJoin(join);
            channel_.SendReliable(kMsgJoin, bytes.data(), bytes.size());
            joinSent_ = true;
        }
    }
    Message msg;
    while (state_ != State::Closed && channel_.PopMessage(msg)) {
        HandleMessage(msg);
    }
    if (state_ == State::Closed) {
        channel_.Disconnect(nowMs);
        return;
    }
    if (channel_.State() != LinkState::Disconnected) {
        return;
    }
    switch (channel_.Reason()) {
    case CloseReason::Timeout:
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

void LobbyClient::HandleMessage(const Message& msg) {
    if (!msg.reliable) {
        refused_++;
        return;
    }
    switch (msg.type) {
    case kMsgWelcome: {
        uint8_t id = 0;
        if (state_ != State::Connecting || !DecodeWelcome(msg.payload.data(), msg.payload.size(), id)) {
            refused_++;
            return;
        }
        localId_ = id;
        state_ = State::Joined;
        return;
    }
    case kMsgReject: {
        RejectReason reason = RejectReason::BadProfile;
        if (!DecodeReject(msg.payload.data(), msg.payload.size(), reason)) {
            refused_++;
            CloseWith("The host refused the join.");
            return;
        }
        CloseWith(RejectReasonText(reason));
        return;
    }
    case kMsgRoster: {
        std::vector<PlayerInfo> roster;
        if (!DecodeRoster(msg.payload.data(), msg.payload.size(), roster)) {
            refused_++;
            return;
        }
        roster_ = std::move(roster);
        return;
    }
    case kMsgChatLine: {
        ChatLine line;
        if (!DecodeChatLine(msg.payload.data(), msg.payload.size(), line)) {
            refused_++;
            return;
        }
        Push(chat_, Resolve(line, roster_));
        return;
    }
    default:
        refused_++;
        return;
    }
}

void LobbyClient::SetReady(bool ready) {
    if (state_ != State::Joined) {
        return;
    }
    const std::vector<uint8_t> bytes = EncodeReady(ready);
    channel_.SendReliable(kMsgReady, bytes.data(), bytes.size());
}

bool LobbyClient::Chat(const std::string& text) {
    const std::string clean = SanitizeChat(text);
    if (state_ != State::Joined || clean.empty()) {
        return false;
    }
    const std::vector<uint8_t> bytes = EncodeChat(clean);
    return channel_.SendReliable(kMsgChat, bytes.data(), bytes.size()) == QueueResult::Ok;
}

void LobbyClient::Leave(uint32_t nowMs) {
    if (state_ == State::Closed) {
        return;
    }
    channel_.Disconnect(nowMs);
    state_ = State::Closed;
    closeText_.clear();
}

void LobbyClient::CloseWith(const std::string& text) {
    state_ = State::Closed;
    closeText_ = text;
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
