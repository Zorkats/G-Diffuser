// O2 connection channel (port/net/gdx_net_channel.cpp): wire format, handshake, ordering and acks.
//
// Wire format, all multi-byte fields little-endian. Every packet starts with 'G' 'D', a type byte and
// the connection nonce:
//   hello      (0): version u16
//   accept     (1): version u16
//   refuse     (2): reason u8, version u16
//   data       (3): seq u16, ack u16, ackBits u32, then messages until the end of the datagram
//   disconnect (4): reason u8
//   message:        type u8, channel u8 (0 unreliable, 1 reliable), [id u16 if reliable], length u16, payload
//
// Reliability: the sender keeps a reliable message until a packet carrying it is acked. Acks are
// packet-level: ack is the highest peer sequence received, and ackBits marks the 32 before it. Each
// message is therefore acked by the packet it rode in, and a packet is never acked before it arrived.

#include "gdx_net_channel.h"

#include <utility>

namespace gdx::net {
namespace {

constexpr uint8_t kMagic0 = 'G';
constexpr uint8_t kMagic1 = 'D';

constexpr uint8_t kTypeHello = 0;
constexpr uint8_t kTypeAccept = 1;
constexpr uint8_t kTypeRefuse = 2;
constexpr uint8_t kTypeData = 3;
constexpr uint8_t kTypeDisconnect = 4;

// Magic (2), type (1) and nonce (4) start every packet.
constexpr size_t kBaseBytes = 7;
// The data header adds seq (2), ack (2) and ackBits (4).
constexpr size_t kDataHeaderBytes = kBaseBytes + 8;
// Per-message overhead: type (1), channel (1), length (2), and id (2) for reliable messages.
constexpr size_t kUnreliableOverhead = 4;
constexpr size_t kReliableOverhead = 6;

// Every message must fit in one datagram. This check fails the build if the header constants drift.
static_assert(kDataHeaderBytes + kReliableOverhead + kMaxMessagePayload == kMaxDatagramBytes,
              "one maximum-size message must exactly fill one datagram");

// A lost hello or accept is recovered by the next hello. 250 ms keeps a handshake under a second.
constexpr uint32_t kHelloRetryMs = 250;

// RTO bounds. Under 50 ms a LAN round trip can look lost. Over 1 s one loss stalls a race for too long.
constexpr uint32_t kInitialRtoMs = 250;
constexpr uint32_t kMinRtoMs = 50;
constexpr uint32_t kMaxRtoMs = 1000;

// Packets kept for ack matching. An ack names only the last 33 packets, so older ones are pruned anyway.
constexpr size_t kInflightCap = 128;

// Samples above this come from a stall or a clock jump, not from network latency. Clamping keeps the
// smoothing arithmetic far from overflow.
constexpr uint32_t kMaxRttSampleMs = 60000;

// Bounds-checked little-endian reader. A short read sets a sticky failure flag and returns zero, so a
// parser reads a whole structure and checks Ok() once. It never reads past the buffer it was given.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {
    }

    bool Ok() const {
        return ok_;
    }

    size_t Left() const {
        return len_ - pos_;
    }

    uint8_t U8() {
        return Take(1) ? data_[pos_ - 1] : static_cast<uint8_t>(0);
    }

    uint16_t U16() {
        if (!Take(2)) {
            return 0;
        }
        return static_cast<uint16_t>(data_[pos_ - 2] | (data_[pos_ - 1] << 8));
    }

    uint32_t U32() {
        if (!Take(4)) {
            return 0;
        }
        return static_cast<uint32_t>(data_[pos_ - 4]) | (static_cast<uint32_t>(data_[pos_ - 3]) << 8) |
               (static_cast<uint32_t>(data_[pos_ - 2]) << 16) | (static_cast<uint32_t>(data_[pos_ - 1]) << 24);
    }

    // Returns a pointer to the next count bytes, or nullptr when fewer remain.
    const uint8_t* Bytes(size_t count) {
        if (!Take(count)) {
            return nullptr;
        }
        return data_ + pos_ - count;
    }

private:
    bool Take(size_t count) {
        if (!ok_ || count > len_ - pos_) {
            ok_ = false;
            return false;
        }
        pos_ += count;
        return true;
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

void Put8(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void Put16(std::vector<uint8_t>& out, uint32_t value) {
    Put8(out, value & 0xFFu);
    Put8(out, value >> 8);
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    Put16(out, value & 0xFFFFu);
    Put16(out, value >> 16);
}

// Unsigned subtraction stays correct across the 49-day uint32 wrap, as long as times are passed in order.
uint32_t Elapsed(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs;
}

std::vector<uint8_t> VersionBody(uint16_t version) {
    std::vector<uint8_t> body;
    Put16(body, version);
    return body;
}

std::vector<uint8_t> ReasonBody(CloseReason why) {
    std::vector<uint8_t> body;
    Put8(body, static_cast<uint8_t>(why));
    return body;
}

std::vector<uint8_t> RefuseBody(CloseReason why, uint16_t version) {
    std::vector<uint8_t> body;
    Put8(body, static_cast<uint8_t>(why));
    Put16(body, version);
    return body;
}

// Maps a wire reason code to a CloseReason. Zero and unknown codes make the packet malformed.
bool ReasonFromCode(uint8_t code, CloseReason& out) {
    if (code < static_cast<uint8_t>(CloseReason::Closed) || code > static_cast<uint8_t>(CloseReason::Overflow)) {
        return false;
    }
    out = static_cast<CloseReason>(code);
    return true;
}

} // namespace

const char* CloseReasonName(CloseReason reason) {
    switch (reason) {
    case CloseReason::None:
        return "none";
    case CloseReason::Closed:
        return "closed";
    case CloseReason::Timeout:
        return "timeout";
    case CloseReason::VersionMismatch:
        return "version mismatch";
    case CloseReason::Busy:
        return "server busy";
    case CloseReason::Overflow:
        return "queue overflow";
    }
    return "unknown";
}

const char* QueueResultName(QueueResult result) {
    switch (result) {
    case QueueResult::Ok:
        return "ok";
    case QueueResult::NotConnected:
        return "not connected";
    case QueueResult::TooLarge:
        return "payload too large";
    case QueueResult::QueueFull:
        return "queue full";
    case QueueResult::Invalid:
        return "invalid argument";
    }
    return "unknown";
}

Channel::Channel(const ChannelConfig& config, SendFn send)
    : config_(config), send_(std::move(send)), slots_(kReliableWindow) {
    ResetConnection();
}

bool Channel::Connect(uint32_t nowMs) {
    if (config_.role != Role::Client || state_ != LinkState::Disconnected) {
        return false;
    }
    nonce_ = config_.connectionNonce;
    ResetConnection();
    state_ = LinkState::Connecting;
    reason_ = CloseReason::None;
    connectStartMs_ = nowMs;
    SendHello(nowMs);
    return true;
}

void Channel::Disconnect(uint32_t nowMs) {
    if (state_ != LinkState::Disconnected) {
        Close(CloseReason::Closed, true, nowMs);
    }
}

void Channel::Receive(const uint8_t* data, size_t len, uint32_t nowMs) {
    stats_.packetsIn++;
    if (data == nullptr || len < kBaseBytes || len > kMaxDatagramBytes) {
        stats_.malformed++;
        return;
    }
    if (data[0] != kMagic0 || data[1] != kMagic1) {
        stats_.badMagic++;
        return;
    }
    Reader head(data + 2, len - 2);
    const uint8_t type = head.U8();
    const uint32_t nonce = head.U32();
    const uint8_t* body = data + kBaseBytes;
    const size_t bodyLen = len - kBaseBytes;
    switch (type) {
    case kTypeHello:
        OnHello(nonce, body, bodyLen, nowMs);
        break;
    case kTypeAccept:
        OnAccept(nonce, body, bodyLen, nowMs);
        break;
    case kTypeRefuse:
        OnRefuse(nonce, body, bodyLen, nowMs);
        break;
    case kTypeData:
        OnData(nonce, body, bodyLen, nowMs);
        break;
    case kTypeDisconnect:
        OnDisconnect(nonce, body, bodyLen, nowMs);
        break;
    default:
        stats_.malformed++;
        break;
    }
}

void Channel::Update(uint32_t nowMs) {
    if (state_ == LinkState::Connecting) {
        if (Elapsed(nowMs, connectStartMs_) >= config_.timeoutMs) {
            Close(CloseReason::Timeout, false, nowMs);
        } else if (Elapsed(nowMs, lastHelloMs_) >= kHelloRetryMs) {
            SendHello(nowMs);
        }
        return;
    }
    if (state_ == LinkState::Connected) {
        if (Elapsed(nowMs, lastRecvMs_) >= config_.timeoutMs) {
            // A silent peer gets no goodbye: it may be gone, and a packet would not reach it anyway.
            Close(CloseReason::Timeout, false, nowMs);
            return;
        }
        FlushOutbound(nowMs);
    }
}

QueueResult Channel::SendReliable(uint8_t type, const uint8_t* data, size_t len) {
    if (state_ != LinkState::Connected) {
        return QueueResult::NotConnected;
    }
    if (len > kMaxMessagePayload) {
        return QueueResult::TooLarge;
    }
    if (data == nullptr && len > 0) {
        return QueueResult::Invalid;
    }
    // The limit counts ids, not queue entries, because acked holes do not free an id. The receiver's
    // reorder buffer covers exactly kReliableWindow ids past its next delivery. A message further than
    // that from the oldest unacked id would have no slot to wait in.
    if (!sendQueue_.empty()) {
        const size_t span = static_cast<uint16_t>(nextMsgId_ - sendQueue_.front().id);
        if (span >= kReliableWindow) {
            return QueueResult::QueueFull;
        }
    }
    Pending pending;
    pending.id = nextMsgId_;
    nextMsgId_ = static_cast<uint16_t>(nextMsgId_ + 1);
    pending.msg.type = type;
    pending.msg.reliable = true;
    if (len > 0) {
        pending.msg.payload.assign(data, data + len);
    }
    sendQueue_.push_back(std::move(pending));
    return QueueResult::Ok;
}

QueueResult Channel::SendUnreliable(uint8_t type, const uint8_t* data, size_t len) {
    if (state_ != LinkState::Connected) {
        return QueueResult::NotConnected;
    }
    if (len > kMaxMessagePayload) {
        return QueueResult::TooLarge;
    }
    if (data == nullptr && len > 0) {
        return QueueResult::Invalid;
    }
    if (unreliableOut_.size() >= kUnreliableOutCap) {
        return QueueResult::QueueFull;
    }
    Message msg;
    msg.type = type;
    msg.reliable = false;
    if (len > 0) {
        msg.payload.assign(data, data + len);
    }
    unreliableOut_.push_back(std::move(msg));
    return QueueResult::Ok;
}

bool Channel::PopMessage(Message& out) {
    if (inbox_.empty()) {
        return false;
    }
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

void Channel::OnHello(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs) {
    Reader r(body, len);
    const uint16_t version = r.U16();
    if (!r.Ok() || r.Left() != 0) {
        stats_.malformed++;
        return;
    }
    if (config_.role != Role::Server) {
        stats_.ignored++;
        return;
    }
    if (version != config_.version) {
        // Refused without touching the state, so a wrong-version client cannot take the server's slot.
        SendControl(kTypeRefuse, nonce, RefuseBody(CloseReason::VersionMismatch, config_.version), nowMs);
        return;
    }
    if (state_ == LinkState::Connected) {
        if (nonce == nonce_) {
            // Our accept was lost and the client resent its hello. Answer again and change nothing else.
            lastRecvMs_ = nowMs;
            SendControl(kTypeAccept, nonce, VersionBody(config_.version), nowMs);
        } else {
            SendControl(kTypeRefuse, nonce, RefuseBody(CloseReason::Busy, config_.version), nowMs);
        }
        return;
    }
    // Disconnected: the first valid hello takes the slot. Per-connection state from an earlier peer
    // must not leak into this one, so it is reset first.
    nonce_ = nonce;
    ResetConnection();
    state_ = LinkState::Connected;
    reason_ = CloseReason::None;
    lastRecvMs_ = nowMs;
    SendControl(kTypeAccept, nonce, VersionBody(config_.version), nowMs);
}

void Channel::OnAccept(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs) {
    Reader r(body, len);
    const uint16_t version = r.U16();
    if (!r.Ok() || r.Left() != 0) {
        stats_.malformed++;
        return;
    }
    if (config_.role != Role::Client || state_ != LinkState::Connecting) {
        stats_.ignored++;
        return;
    }
    if (nonce != nonce_) {
        stats_.wrongNonce++;
        return;
    }
    if (version != config_.version) {
        // A server never accepts a version we did not offer, so the peer is broken.
        stats_.malformed++;
        return;
    }
    state_ = LinkState::Connected;
    reason_ = CloseReason::None;
    lastRecvMs_ = nowMs;
    lastSendMs_ = nowMs; // the keepalive clock starts with the connection
}

void Channel::OnRefuse(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs) {
    Reader r(body, len);
    const uint8_t code = r.U8();
    r.U16(); // the server's version: informational, the reason already says what happened
    if (!r.Ok() || r.Left() != 0) {
        stats_.malformed++;
        return;
    }
    CloseReason why = CloseReason::None;
    if (!ReasonFromCode(code, why)) {
        stats_.malformed++;
        return;
    }
    if (config_.role != Role::Client || state_ != LinkState::Connecting) {
        stats_.ignored++;
        return;
    }
    if (nonce != nonce_) {
        stats_.wrongNonce++;
        return;
    }
    Close(why, false, nowMs);
}

void Channel::OnDisconnect(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs) {
    Reader r(body, len);
    const uint8_t code = r.U8();
    if (!r.Ok() || r.Left() != 0) {
        stats_.malformed++;
        return;
    }
    CloseReason why = CloseReason::None;
    if (!ReasonFromCode(code, why)) {
        stats_.malformed++;
        return;
    }
    if (state_ == LinkState::Disconnected) {
        stats_.ignored++;
        return;
    }
    if (nonce != nonce_) {
        stats_.wrongNonce++;
        return;
    }
    // The peer closed the link and said why. Keep its reason, and send nothing back.
    Close(why, false, nowMs);
}

void Channel::OnData(uint32_t nonce, const uint8_t* body, size_t len, uint32_t nowMs) {
    if (state_ != LinkState::Connected) {
        stats_.ignored++;
        return;
    }
    if (nonce != nonce_) {
        stats_.wrongNonce++;
        return;
    }
    uint16_t seq = 0;
    uint16_t ack = 0;
    uint32_t bits = 0;
    std::vector<WireMessage> msgs;
    // Parse the whole packet before any state changes. A single bad field rejects it entirely, so no
    // part of a corrupt packet is delivered, and it is not acked.
    if (!ParseData(body, len, seq, ack, bits, msgs)) {
        stats_.malformed++;
        return;
    }
    // A correct sender never runs more than kReliableWindow ids past our delivery point, so an id
    // beyond it means a broken or hostile peer. Rejecting the packet unacked keeps the message with
    // its sender instead of acking it and losing it. Delivery inside this packet only moves the
    // delivery point forward, so checking against it once here covers every message in the packet.
    for (const WireMessage& m : msgs) {
        const size_t ahead = static_cast<uint16_t>(m.id - nextRecvId_);
        if (m.reliable && ahead >= kReliableWindow && ahead < 0x8000) {
            stats_.outOfWindow++;
            return;
        }
    }
    lastRecvMs_ = nowMs;
    ProcessAcks(ack, bits, nowMs);
    const bool fresh = RecordRemoteSeq(seq);
    if (!msgs.empty()) {
        // Only packets that carry messages need an ack. An ack owed to an ack would loop forever.
        ackPending_ = true;
    }
    for (const WireMessage& m : msgs) {
        if (m.reliable) {
            if (!AcceptReliable(m, nowMs)) {
                return; // the link closed on overflow
            }
        } else if (!fresh) {
            // Unreliable messages are sent once. A repeated packet must not deliver them again.
            stats_.duplicates++;
        } else if (inbox_.size() >= kInboxCap) {
            stats_.overflowDrops++;
        } else {
            inbox_.push_back(MakeMessage(m));
        }
    }
}

bool Channel::ParseData(const uint8_t* body, size_t len, uint16_t& seq, uint16_t& ack, uint32_t& bits,
                        std::vector<WireMessage>& out) {
    Reader r(body, len);
    seq = r.U16();
    ack = r.U16();
    bits = r.U32();
    while (r.Ok() && r.Left() > 0) {
        WireMessage m;
        m.type = r.U8();
        const uint8_t channel = r.U8();
        if (channel > 1) {
            return false;
        }
        m.reliable = channel == 1;
        if (m.reliable) {
            m.id = r.U16();
        }
        m.length = r.U16();
        if (m.length > kMaxMessagePayload) {
            return false;
        }
        m.payload = r.Bytes(m.length);
        if (!r.Ok()) {
            return false;
        }
        out.push_back(m);
    }
    return r.Ok();
}

Message Channel::MakeMessage(const WireMessage& m) {
    Message out;
    out.type = m.type;
    out.reliable = m.reliable;
    out.payload.assign(m.payload, m.payload + m.length);
    return out;
}

void Channel::AppendMessage(Outgoing& out, bool reliable, uint16_t id, const Message& msg) {
    Put8(out.bytes, msg.type);
    Put8(out.bytes, reliable ? 1u : 0u);
    if (reliable) {
        Put16(out.bytes, id);
    }
    Put16(out.bytes, static_cast<uint32_t>(msg.payload.size()));
    out.bytes.insert(out.bytes.end(), msg.payload.begin(), msg.payload.end());
    out.msgs++;
}

void Channel::ResetConnection() {
    nextSeq_ = config_.initialSequence;
    nextMsgId_ = config_.initialSequence;
    // A peer that has received nothing acks "initialSequence - 1" with no bits. Starting the window
    // there means that ack names a packet we never sent, so it cannot falsely ack our first packet.
    remoteSeq_ = static_cast<uint16_t>(config_.initialSequence - 1);
    recvBits_ = 0;
    ackPending_ = false;
    nextRecvId_ = config_.initialSequence;
    sendQueue_.clear();
    unreliableOut_.clear();
    inFlight_.clear();
    inbox_.clear();
    for (Slot& slot : slots_) {
        slot.used = false;
        slot.msg = Message();
    }
    rttValid_ = false;
    srttMs_ = 0;
    rttVarMs_ = 0;
}

void Channel::Close(CloseReason reason, bool notifyPeer, uint32_t nowMs) {
    if (notifyPeer && state_ != LinkState::Disconnected) {
        SendControl(kTypeDisconnect, nonce_, ReasonBody(reason), nowMs);
    }
    state_ = LinkState::Disconnected;
    reason_ = reason;
    // Queued reliable messages can never be delivered now. The inbox is kept so the game can still
    // read what arrived before the link closed.
    sendQueue_.clear();
    unreliableOut_.clear();
    inFlight_.clear();
}

void Channel::SendHello(uint32_t nowMs) {
    SendControl(kTypeHello, nonce_, VersionBody(config_.version), nowMs);
    lastHelloMs_ = nowMs;
}

void Channel::SendControl(uint8_t type, uint32_t nonce, const std::vector<uint8_t>& body, uint32_t nowMs) {
    std::vector<uint8_t> bytes;
    bytes.reserve(kBaseBytes + body.size());
    Put8(bytes, kMagic0);
    Put8(bytes, kMagic1);
    Put8(bytes, type);
    Put32(bytes, nonce);
    bytes.insert(bytes.end(), body.begin(), body.end());
    Emit(bytes, nowMs);
}

void Channel::Emit(const std::vector<uint8_t>& bytes, uint32_t nowMs) {
    stats_.packetsOut++;
    lastSendMs_ = nowMs;
    if (send_) {
        send_(bytes.data(), bytes.size());
    }
}

void Channel::BeginData(Outgoing& out) {
    out.bytes.clear();
    out.msgs = 0;
    out.seq = nextSeq_;
    nextSeq_ = static_cast<uint16_t>(nextSeq_ + 1);
    Put8(out.bytes, kMagic0);
    Put8(out.bytes, kMagic1);
    Put8(out.bytes, kTypeData);
    Put32(out.bytes, nonce_);
    Put16(out.bytes, out.seq);
    Put16(out.bytes, remoteSeq_);
    Put32(out.bytes, recvBits_);
}

void Channel::Place(Outgoing& out, size_t need, uint32_t nowMs) {
    // Messages are never split. A message that does not fit starts the next packet, which always has
    // room for one maximum-size message.
    if (out.msgs > 0 && out.bytes.size() + need > kMaxDatagramBytes) {
        Ship(out, nowMs);
    }
    if (out.msgs == 0) {
        BeginData(out);
    }
}

void Channel::Ship(Outgoing& out, uint32_t nowMs) {
    if (out.msgs > 0) {
        inFlight_.push_back(InFlight{out.seq, nowMs});
        if (inFlight_.size() > kInflightCap) {
            inFlight_.pop_front();
        }
    }
    Emit(out.bytes, nowMs);
    // Every outgoing packet carries the current ack state, so it settles any ack that was owed.
    ackPending_ = false;
    out.bytes.clear();
    out.msgs = 0;
}

void Channel::FlushOutbound(uint32_t nowMs) {
    const uint32_t rto = RtoMs();
    Outgoing out;
    for (Pending& p : sendQueue_) {
        // An unsent message goes out now. A sent one goes out again only after the RTO, because its
        // packet may still be acked, and an early resend would just be a duplicate.
        if (p.sent && Elapsed(nowMs, p.sentMs) < rto) {
            continue;
        }
        Place(out, kReliableOverhead + p.msg.payload.size(), nowMs);
        if (p.sent) {
            stats_.resends++;
        }
        p.sent = true;
        p.sentMs = nowMs;
        p.lastSeq = out.seq;
        AppendMessage(out, true, p.id, p.msg);
    }
    while (!unreliableOut_.empty()) {
        const Message& msg = unreliableOut_.front();
        Place(out, kUnreliableOverhead + msg.payload.size(), nowMs);
        AppendMessage(out, false, 0, msg);
        unreliableOut_.pop_front();
    }
    if (out.msgs > 0) {
        Ship(out, nowMs);
    }
    // An empty packet covers two needs: an ack owed to the peer, and keepalive. Without the keepalive
    // an idle link would look dead to the peer's silence timer.
    if (ackPending_ || Elapsed(nowMs, lastSendMs_) >= config_.keepaliveMs) {
        Outgoing keepalive;
        BeginData(keepalive);
        Ship(keepalive, nowMs);
    }
}

bool Channel::RecordRemoteSeq(uint16_t seq) {
    const int fwd = static_cast<uint16_t>(seq - remoteSeq_);
    if (fwd == 0) {
        return false;
    }
    if (fwd < 0x8000) {
        // A newer packet slides the window forward. The old newest packet becomes bit fwd - 1.
        if (fwd > 32) {
            recvBits_ = 0;
        } else {
            const uint64_t shifted = (static_cast<uint64_t>(recvBits_) << fwd) | (uint64_t{1} << (fwd - 1));
            recvBits_ = static_cast<uint32_t>(shifted);
        }
        remoteSeq_ = seq;
        return true;
    }
    // An older packet is tracked by its bit while it is inside the 32-packet window.
    const int back = static_cast<uint16_t>(remoteSeq_ - seq);
    if (back > 32) {
        // Too old to tell apart from a duplicate, so it is treated as one.
        return false;
    }
    const uint32_t bit = uint32_t{1} << (back - 1);
    if ((recvBits_ & bit) != 0) {
        return false;
    }
    recvBits_ |= bit;
    return true;
}

void Channel::ProcessAcks(uint16_t ack, uint32_t bits, uint32_t nowMs) {
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        const int diff = static_cast<uint16_t>(ack - it->seq);
        const bool acked = diff == 0 || (diff <= 32 && ((bits >> (diff - 1)) & 1u) != 0);
        if (acked) {
            AddRttSample(Elapsed(nowMs, it->sentMs));
            AckSeq(it->seq);
            it = inFlight_.erase(it);
        } else if (diff > 32 && diff < 0x8000) {
            // The peer's window has moved past this packet, so no later ack can name it.
            it = inFlight_.erase(it);
        } else {
            ++it;
        }
    }
}

void Channel::AckSeq(uint16_t seq) {
    for (auto it = sendQueue_.begin(); it != sendQueue_.end();) {
        if (it->sent && it->lastSeq == seq) {
            it = sendQueue_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Channel::AcceptReliable(const WireMessage& m, uint32_t nowMs) {
    const size_t ahead = static_cast<uint16_t>(m.id - nextRecvId_);
    if (ahead == 0) {
        if (inbox_.size() >= kInboxCap) {
            Close(CloseReason::Overflow, true, nowMs);
            return false;
        }
        inbox_.push_back(MakeMessage(m));
        nextRecvId_ = static_cast<uint16_t>(nextRecvId_ + 1);
        return DrainSlots(nowMs);
    }
    if (ahead < kReliableWindow) {
        // Inside the window, a slot index names exactly one id. So an occupied slot is this same
        // message arriving again, not a conflict.
        Slot& slot = slots_[static_cast<size_t>(m.id) % kReliableWindow];
        if (slot.used) {
            stats_.duplicates++;
        } else {
            slot.used = true;
            slot.id = m.id;
            slot.msg = MakeMessage(m);
        }
        return true;
    }
    // OnData already rejected ids beyond the window, so this id is behind the delivery point: it was
    // delivered before.
    stats_.duplicates++;
    return true;
}

bool Channel::DrainSlots(uint32_t nowMs) {
    for (;;) {
        Slot& slot = slots_[static_cast<size_t>(nextRecvId_) % kReliableWindow];
        if (!slot.used || slot.id != nextRecvId_) {
            return true;
        }
        if (inbox_.size() >= kInboxCap) {
            Close(CloseReason::Overflow, true, nowMs);
            return false;
        }
        inbox_.push_back(std::move(slot.msg));
        slot.used = false;
        nextRecvId_ = static_cast<uint16_t>(nextRecvId_ + 1);
    }
}

void Channel::AddRttSample(uint32_t sampleMs) {
    const int32_t sample = static_cast<int32_t>(sampleMs > kMaxRttSampleMs ? kMaxRttSampleMs : sampleMs);
    if (!rttValid_) {
        srttMs_ = sample;
        rttVarMs_ = sample / 2;
        rttValid_ = true;
    } else {
        // RFC 6298 smoothing: 1/8 for the mean, 1/4 for the deviation.
        const int32_t err = sample > srttMs_ ? sample - srttMs_ : srttMs_ - sample;
        rttVarMs_ = (3 * rttVarMs_ + err) / 4;
        srttMs_ = (7 * srttMs_ + sample) / 8;
    }
    stats_.rttMs = static_cast<uint32_t>(srttMs_);
}

uint32_t Channel::RtoMs() const {
    if (!rttValid_) {
        return kInitialRtoMs;
    }
    int32_t rto = srttMs_ + 4 * rttVarMs_;
    if (rto < static_cast<int32_t>(kMinRtoMs)) {
        rto = static_cast<int32_t>(kMinRtoMs);
    }
    if (rto > static_cast<int32_t>(kMaxRtoMs)) {
        rto = static_cast<int32_t>(kMaxRtoMs);
    }
    return static_cast<uint32_t>(rto);
}

} // namespace gdx::net
