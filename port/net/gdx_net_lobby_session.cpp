// port/net/gdx_net_lobby_session.cpp: LAN room host, client and room browser (gdx_net_lobby_session.h).

#include "gdx_net_lobby_session.h"

#include <algorithm>
#include <utility>

namespace gdx::net::lobby {

namespace {

const char* const kTooFastText = "You are sending messages too fast. Wait a moment.";

uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs; // unsigned: correct across the 49-day wrap
}

// Every channel packet starts with "GD"; anything else from an unknown address never gets a channel.
bool LooksLikeChannelPacket(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 2 && data[0] == 'G' && data[1] == 'D';
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
};

LobbyHost::LobbyHost(std::string build, LocalProfile me, uint16_t port, const RoomConfig& config, DatagramSend send)
    : build_(std::move(build)), me_(std::move(me)), port_(port), send_(std::move(send)),
      machine_(config, HostPlayer(me_)) {
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
        const RoomState state = machine_.State(nowMs);
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
    config.timeoutMs = kLinkTimeoutMs;
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
    // First, so the channel updates below send them in this tick.
    FlushPoses();
    // Index loop: HandleMessage never adds or removes peers, but Reject may mark one.
    for (size_t i = 0; i < peers_.size(); i++) {
        Peer& peer = *peers_[i];
        peer.posesThisUpdate = 0;
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
    std::vector<uint8_t> gone;
    for (const std::unique_ptr<Peer>& peer : peers_) {
        if (peer->dead && peer->joined) {
            gone.push_back(peer->id);
        }
    }
    // Drop the dead links first, so the roster that follows goes only to players still here.
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [](const std::unique_ptr<Peer>& p) { return p->dead; }),
                 peers_.end());
    for (uint8_t id : gone) {
        RemovePlayer(id, nowMs);
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
    case kMsgVote: {
        uint8_t track = 0;
        ok = DecodeVote(data, len, track) && machine_.Vote(id, track, nowMs, out);
        break;
    }
    case kMsgPick: {
        uint8_t track = 0;
        ok = DecodePick(data, len, track) && machine_.Pick(id, track, nowMs, out);
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
    if (join.build != build_) {
        Reject(peer, RejectReason::Build, nowMs);
        return;
    }
    uint8_t id = kSystemId;
    RejectReason why = RejectReason::Full;
    std::vector<Outgoing> out;
    if (!machine_.Join(join, nowMs, id, why, out)) {
        Reject(peer, why, nowMs);
        return;
    }
    peer.joined = true;
    peer.id = id;
    // The welcome goes first: the roster and the room that follow mean nothing without the player's own id.
    const std::vector<uint8_t> welcome = EncodeWelcome(id);
    peer.channel->SendReliable(kMsgWelcome, welcome.data(), welcome.size());
    Dispatch(out);
    Note(JoinedText(join.name, join.seat == Seat::Watch));
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
    std::vector<Outgoing> out;
    machine_.Leave(id, nowMs, out);
    Dispatch(out);
    Note(LeftText(name));
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

bool LobbyHost::Pick(uint8_t track, uint32_t nowMs) {
    std::vector<Outgoing> out;
    const bool ok = !closed_ && machine_.Pick(kHostId, track, nowMs, out);
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
                         DatagramSend send)
    : build_(std::move(build)), me_(std::move(me)), seat_(seat), host_(host),
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
            join.seat = seat_;
            const std::vector<uint8_t> bytes = EncodeJoin(join);
            channel_.SendReliable(kMsgJoin, bytes.data(), bytes.size());
            joinSent_ = true;
        }
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
    case kMsgWelcome: {
        uint8_t id = 0;
        if (state_ != State::Connecting || !DecodeWelcome(data, len, id)) {
            refused_++;
            return;
        }
        localId_ = id;
        state_ = State::Joined;
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
        lotteryChecked_ =
            RunLottery(room_.roomId, room_.raceIndex, lottery.ballots, room_.pool, room_.lastTrack) == lottery;
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

void LobbyClient::Pick(uint8_t track) {
    Send(kMsgPick, EncodePick(track));
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
    if (posesRace_ != state.raceIndex) {
        posesIn_.clear();
        posesRace_ = state.raceIndex;
    }
    for (const StatePose& pose : state.poses) {
        if (static_cast<size_t>(pose.slot) >= racers) {
            refused_++; // no such machine in this race
            return;
        }
        if (pose.slot != own) {
            PushPose(posesIn_, pose);
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
}

void LobbyClient::Note(const std::string& text) {
    PushNote(lines_, text);
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
