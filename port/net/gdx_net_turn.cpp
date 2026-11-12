// port/net/gdx_net_turn.cpp: the TURN codec and client (RFC 8489, RFC 8656) for the room relay.
// Copyright (c) Zorkats

#include "gdx_net_turn.h"

#include "gdx_net_hash.h"

#include <algorithm>
#include <utility>

namespace gdx::net::turn {
namespace {

constexpr size_t kAttrHeaderBytes = 4;
constexpr size_t kFingerprintAttrBytes = 8;
constexpr uint32_t kFingerprintXor = 0x5354554Eu; // RFC 8489 section 14.7
constexpr uint8_t kFamilyIPv4 = 1;
constexpr uint8_t kFamilyIPv6 = 2;
constexpr size_t kChannelHeaderBytes = 4;
constexpr size_t kMaxChannelPayload = 0xFFFF;

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
}

uint32_t GetU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void PutU16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value & 0xFFu);
}

void PutU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(value & 0xFFu);
}

// The same CRC-32 as gdx_net_stun.cpp, which keeps its copy private.
uint32_t Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

// The HMAC of the message up to an integrity attribute at `at`, the header length counting that attribute as the
// last one (RFC 8489 section 14.5).
hash::Sha1Digest IntegrityOf(const uint8_t* bytes, size_t at, const uint8_t* key, size_t keyLength) {
    std::vector<uint8_t> covered(bytes, bytes + at);
    PutU16(covered.data() + 2, static_cast<uint16_t>(at - kHeaderBytes + kAttrHeaderBytes + kIntegrityBytes));
    return hash::HmacSha1(key, keyLength, covered.data(), covered.size());
}

// The XOR key of an address attribute: the cookie, then the transaction id (RFC 8489 section 14.2).
std::array<uint8_t, 16> XorKey(const uint8_t* txid) {
    std::array<uint8_t, 16> key{};
    PutU32(key.data(), kMagicCookie);
    std::copy(txid, txid + kTransactionIdBytes, key.begin() + 4);
    return key;
}

// When to refresh an allocation of the given lifetime: a minute early, or halfway for a short one.
uint64_t RefreshAfterMs(uint32_t lifetimeS) {
    const uint32_t early = lifetimeS > 120 ? lifetimeS - 60 : lifetimeS / 2;
    return static_cast<uint64_t>(std::max<uint32_t>(early, 1)) * 1000u;
}

uint16_t MethodOf(uint16_t type) {
    return static_cast<uint16_t>(type & ~static_cast<uint16_t>(kError));
}

} // namespace

// ---- Codec ----

Builder::Builder(uint16_t type, const TransactionId& txid) : bytes_(kHeaderBytes, 0), txid_(txid) {
    PutU16(bytes_.data(), type);
    PutU32(bytes_.data() + 4, kMagicCookie);
    std::copy(txid.begin(), txid.end(), bytes_.begin() + 8);
}

void Builder::Add(uint16_t attr, const uint8_t* value, size_t length) {
    const size_t at = bytes_.size();
    const size_t padded = (length + 3) & ~static_cast<size_t>(3);
    bytes_.resize(at + kAttrHeaderBytes + padded, 0);
    PutU16(bytes_.data() + at, attr);
    PutU16(bytes_.data() + at + 2, static_cast<uint16_t>(length));
    if (length > 0) {
        std::copy(value, value + length, bytes_.begin() + static_cast<std::ptrdiff_t>(at + kAttrHeaderBytes));
    }
    PutU16(bytes_.data() + 2, static_cast<uint16_t>(bytes_.size() - kHeaderBytes));
}

void Builder::AddText(uint16_t attr, const std::string& text) {
    Add(attr, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void Builder::AddU32(uint16_t attr, uint32_t value) {
    uint8_t raw[4];
    PutU32(raw, value);
    Add(attr, raw, sizeof(raw));
}

void Builder::AddAddress(uint16_t attr, const NetAddress& address) {
    const std::array<uint8_t, 16> key = XorKey(txid_.data());
    uint8_t value[20] = {};
    const bool v4 = address.IsV4();
    value[1] = v4 ? kFamilyIPv4 : kFamilyIPv6;
    PutU16(value + 2, static_cast<uint16_t>(address.port ^ static_cast<uint16_t>(kMagicCookie >> 16)));
    if (v4) {
        for (size_t i = 0; i < 4; i++) {
            value[4 + i] = static_cast<uint8_t>(address.ip[12 + i] ^ key[i]);
        }
        Add(attr, value, 8);
    } else {
        for (size_t i = 0; i < 16; i++) {
            value[4 + i] = static_cast<uint8_t>(address.ip[i] ^ key[i]);
        }
        Add(attr, value, 20);
    }
}

std::vector<uint8_t> Builder::Finish(const uint8_t* key, size_t keyLength, bool fingerprint) {
    if (key != nullptr) {
        const size_t at = bytes_.size();
        const hash::Sha1Digest mac = IntegrityOf(bytes_.data(), at, key, keyLength);
        Add(kAttrIntegrity, mac.data(), mac.size());
    }
    if (fingerprint) {
        const size_t at = bytes_.size();
        PutU16(bytes_.data() + 2, static_cast<uint16_t>(at - kHeaderBytes + kFingerprintAttrBytes));
        uint8_t value[4];
        PutU32(value, Crc32(bytes_.data(), at) ^ kFingerprintXor);
        Add(kAttrFingerprint, value, sizeof(value));
    }
    return bytes_;
}

const Attr* Parsed::Find(uint16_t wanted) const {
    for (const Attr& attr : attrs) {
        if (attr.type == wanted) {
            return &attr;
        }
    }
    return nullptr;
}

bool Parse(const uint8_t* bytes, size_t length, Parsed& out) {
    if (bytes == nullptr || length < kHeaderBytes || (bytes[0] & 0xC0u) != 0) {
        return false;
    }
    const size_t declared = GetU16(bytes + 2);
    if (declared % 4 != 0 || length != kHeaderBytes + declared || GetU32(bytes + 4) != kMagicCookie) {
        return false;
    }
    Parsed parsed;
    parsed.type = GetU16(bytes);
    std::copy(bytes + 8, bytes + kHeaderBytes, parsed.txid.begin());
    const size_t end = length;
    size_t pos = kHeaderBytes;
    while (pos < end) {
        if (end - pos < kAttrHeaderBytes) {
            return false;
        }
        const uint16_t type = GetU16(bytes + pos);
        const size_t valueLength = GetU16(bytes + pos + 2);
        const size_t valueAt = pos + kAttrHeaderBytes;
        const size_t padded = (valueLength + 3) & ~static_cast<size_t>(3);
        if (padded > end - valueAt) {
            return false;
        }
        const size_t next = valueAt + padded;
        if (type == kAttrFingerprint) {
            // Only a FINGERPRINT that closes the message, over everything before it.
            if (valueLength != 4 || next != end || GetU32(bytes + valueAt) != (Crc32(bytes, pos) ^ kFingerprintXor)) {
                return false;
            }
        } else if (!parsed.hasIntegrity) {
            if (type == kAttrIntegrity) {
                if (valueLength != kIntegrityBytes) {
                    return false;
                }
                parsed.hasIntegrity = true;
                parsed.integrityAt = pos;
            }
            parsed.attrs.push_back(Attr{type, valueAt, valueLength});
        }
        pos = next;
    }
    out = std::move(parsed);
    return true;
}

bool IntegrityOk(const uint8_t* bytes, const Parsed& parsed, const uint8_t* key, size_t keyLength) {
    if (!parsed.hasIntegrity) {
        return false;
    }
    const hash::Sha1Digest mac = IntegrityOf(bytes, parsed.integrityAt, key, keyLength);
    const uint8_t* got = bytes + parsed.integrityAt + kAttrHeaderBytes;
    uint8_t diff = 0;
    for (size_t i = 0; i < kIntegrityBytes; i++) {
        diff = static_cast<uint8_t>(diff | (mac[i] ^ got[i]));
    }
    return diff == 0;
}

bool ReadAddress(const uint8_t* bytes, const Parsed& parsed, uint16_t attr, NetAddress& out) {
    const Attr* found = parsed.Find(attr);
    if (found == nullptr || found->length < 4) {
        return false;
    }
    const uint8_t* value = bytes + found->at;
    const std::array<uint8_t, 16> key = XorKey(parsed.txid.data());
    NetAddress address;
    address.port = static_cast<uint16_t>(GetU16(value + 2) ^ static_cast<uint16_t>(kMagicCookie >> 16));
    if (value[1] == kFamilyIPv4 && found->length == 8) {
        uint32_t ip4 = 0;
        for (size_t i = 0; i < 4; i++) {
            ip4 = (ip4 << 8) | static_cast<uint8_t>(value[4 + i] ^ key[i]);
        }
        address = NetAddress::FromV4(ip4, address.port);
    } else if (value[1] == kFamilyIPv6 && found->length == 20) {
        for (size_t i = 0; i < 16; i++) {
            address.ip[i] = static_cast<uint8_t>(value[4 + i] ^ key[i]);
        }
    } else {
        return false;
    }
    out = address;
    return true;
}

bool ReadU32(const uint8_t* bytes, const Parsed& parsed, uint16_t attr, uint32_t& out) {
    const Attr* found = parsed.Find(attr);
    if (found == nullptr || found->length != 4) {
        return false;
    }
    out = GetU32(bytes + found->at);
    return true;
}

std::string ReadText(const uint8_t* bytes, const Parsed& parsed, uint16_t attr) {
    const Attr* found = parsed.Find(attr);
    if (found == nullptr) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char*>(bytes + found->at), found->length);
}

int ErrorCode(const uint8_t* bytes, const Parsed& parsed) {
    const Attr* found = parsed.Find(kAttrErrorCode);
    if (found == nullptr || found->length < 4) {
        return 0;
    }
    const uint8_t* value = bytes + found->at;
    return static_cast<int>(value[2] & 0x07u) * 100 + static_cast<int>(value[3]);
}

bool IsChannelData(const uint8_t* bytes, size_t length) {
    return bytes != nullptr && length >= kChannelHeaderBytes && (bytes[0] & 0xC0u) == 0x40u;
}

std::array<uint8_t, 16> LongTermKey(const std::string& username, const std::string& realm, const std::string& password) {
    const std::string joined = username + ":" + realm + ":" + password;
    return hash::Md5(reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
}

// ---- Client ----

const char* FailureName(Failure failure) {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::Timeout: return "no answer";
    case Failure::Unauthorized: return "credentials refused";
    case Failure::Refused: return "refused";
    case Failure::BadAnswer: return "bad answer";
    }
    return "unknown";
}

Client::Client(SendToServer send, Deliver deliver) : send_(std::move(send)), deliver_(std::move(deliver)) {}

void Client::Start(const Credentials& credentials, const TransactionId& seed, uint64_t nowMs) {
    state_ = State::Allocating;
    failure_ = Failure::None;
    credentials_ = credentials;
    realm_.clear();
    nonce_.clear();
    key_ = {};
    haveKey_ = false;
    seed_ = seed;
    counter_ = 0;
    pending_.clear();
    for (Peer& peer : peers_) {
        peer.channel = 0;
        peer.link = Link::Binding;
        peer.renewMs = 0;
    }
    relayed_ = NetAddress{};
    mapped_ = NetAddress{};
    lifetimeS_ = 0;
    refreshMs_ = 0;
    Issue(Purpose::Allocate, nullptr, nowMs);
}

TransactionId Client::NextTxid() {
    TransactionId txid = seed_;
    const uint32_t n = counter_++;
    for (size_t i = 0; i < 4; i++) {
        txid[kTransactionIdBytes - 1 - i] = static_cast<uint8_t>(txid[kTransactionIdBytes - 1 - i] ^ ((n >> (8 * i)) & 0xFFu));
    }
    return txid;
}

std::vector<uint8_t> Client::Request(Purpose purpose, const Peer* peer, const TransactionId& txid,
                                     uint32_t lifetimeS) const {
    static constexpr uint16_t kTypes[] = {kAllocate, kRefresh, kChannelBind, kCreatePermission};
    Builder builder(kTypes[static_cast<size_t>(purpose)], txid);
    switch (purpose) {
    case Purpose::Allocate: {
        const uint8_t transport[4] = {kTransportUdp, 0, 0, 0};
        builder.Add(kAttrRequestedTransport, transport, sizeof(transport));
        break;
    }
    case Purpose::Refresh:
        builder.AddU32(kAttrLifetime, lifetimeS);
        break;
    case Purpose::Bind:
        builder.AddU32(kAttrChannelNumber, static_cast<uint32_t>(peer->channel) << 16);
        builder.AddAddress(kAttrPeerAddress, peer->address);
        break;
    case Purpose::Permit:
        builder.AddAddress(kAttrPeerAddress, peer->address);
        break;
    }
    if (!haveKey_) {
        return builder.Finish(nullptr, 0, true);
    }
    builder.AddText(kAttrUsername, credentials_.username);
    builder.AddText(kAttrRealm, realm_);
    builder.AddText(kAttrNonce, nonce_);
    return builder.Finish(key_.data(), key_.size(), true);
}

void Client::Issue(Purpose purpose, Peer* peer, uint64_t nowMs, int stale) {
    if (purpose == Purpose::Bind && peer->channel == 0) {
        if (nextChannel_ > kLastChannel) {
            purpose = Purpose::Permit; // every channel number is spent: this peer goes by Send indications
            peer->link = Link::Permitting;
        } else {
            peer->channel = nextChannel_++;
        }
    }
    Pending pending;
    pending.txid = NextTxid();
    pending.purpose = purpose;
    if (peer != nullptr) {
        pending.peer = peer->address;
        pending.channel = peer->channel;
    }
    pending.bytes = Request(purpose, peer, pending.txid, kDefaultLifetimeS);
    pending.signedWithKey = haveKey_;
    pending.sends = 1;
    pending.stale = stale;
    pending.nextSendMs = nowMs + kInitialRtoMs;
    send_(pending.bytes.data(), pending.bytes.size());
    pending_.push_back(std::move(pending));
}

bool Client::Waiting(Purpose purpose, const NetAddress& peer) const {
    return std::any_of(pending_.begin(), pending_.end(),
                       [&](const Pending& p) { return p.purpose == purpose && p.peer == peer; });
}

void Client::DropPending(Purpose purpose, const NetAddress& peer) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return p.purpose == purpose && p.peer == peer; }),
                   pending_.end());
}

void Client::Fail(Failure failure) {
    state_ = State::Failed;
    failure_ = failure;
    pending_.clear();
}

void Client::Update(uint64_t nowMs) {
    if (state_ != State::Allocating && state_ != State::Ready) {
        return;
    }
    std::vector<Pending> expired;
    for (size_t i = 0; i < pending_.size();) {
        Pending& p = pending_[i];
        if (nowMs < p.nextSendMs) {
            i++;
            continue;
        }
        if (p.sends >= kMaxSends) {
            expired.push_back(std::move(p));
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        send_(p.bytes.data(), p.bytes.size());
        p.nextSendMs = nowMs + (kInitialRtoMs << p.sends);
        p.sends++;
        i++;
    }
    for (const Pending& p : expired) {
        if (p.purpose == Purpose::Allocate || p.purpose == Purpose::Refresh) {
            Fail(Failure::Timeout);
            return;
        }
        OnPeerResult(p, false, nowMs);
    }
    if (state_ != State::Ready) {
        return;
    }
    if (nowMs >= refreshMs_ && !Waiting(Purpose::Refresh, NetAddress{})) {
        Issue(Purpose::Refresh, nullptr, nowMs);
    }
    for (Peer& peer : peers_) {
        const bool fresh = peer.link == Link::Binding || peer.link == Link::Permitting;
        const bool due = (peer.link == Link::Bound || peer.link == Link::Permitted) && nowMs >= peer.renewMs;
        if (!fresh && !due) {
            continue;
        }
        const bool binds = peer.link == Link::Binding || peer.link == Link::Bound;
        const Purpose purpose = binds ? Purpose::Bind : Purpose::Permit;
        if (!Waiting(purpose, peer.address)) {
            Issue(purpose, &peer, nowMs);
        }
    }
}

bool Client::Receive(const uint8_t* data, size_t length, uint64_t nowMs) {
    if (state_ == State::Idle) {
        return false;
    }
    if (IsChannelData(data, length)) {
        const uint16_t channel = GetU16(data);
        const size_t payload = GetU16(data + 2);
        Peer* peer = FindChannel(channel);
        if (peer != nullptr && payload <= length - kChannelHeaderBytes && state_ == State::Ready) {
            deliver_(peer->address, data + kChannelHeaderBytes, payload);
        }
        return true;
    }
    Parsed parsed;
    if (!Parse(data, length, parsed)) {
        return false;
    }
    if (parsed.type == (kData | kIndication)) {
        NetAddress from;
        const Attr* payload = parsed.Find(kAttrData);
        const Peer* peer = nullptr;
        if (state_ == State::Ready && payload != nullptr && ReadAddress(data, parsed, kAttrPeerAddress, from)) {
            peer = FindPeer(from);
        }
        if (peer != nullptr && peer->link != Link::Failed) {
            deliver_(from, data + payload->at, payload->length);
        }
        return true;
    }
    const uint16_t kind = static_cast<uint16_t>(parsed.type & kError);
    if (kind == kSuccess || kind == kError) {
        OnAnswer(data, parsed, nowMs);
    }
    return true;
}

void Client::OnAnswer(const uint8_t* data, const Parsed& parsed, uint64_t nowMs) {
    const auto it = std::find_if(pending_.begin(), pending_.end(),
                                 [&](const Pending& p) { return p.txid == parsed.txid; });
    if (it == pending_.end()) {
        return;
    }
    static constexpr uint16_t kTypes[] = {kAllocate, kRefresh, kChannelBind, kCreatePermission};
    if (MethodOf(parsed.type) != kTypes[static_cast<size_t>(it->purpose)]) {
        return;
    }
    const bool success = (parsed.type & kError) == kSuccess;
    if (success && it->signedWithKey && !IntegrityOk(data, parsed, key_.data(), key_.size())) {
        return; // not from the server that holds our key: the retransmission goes on
    }
    const Pending pending = *it;
    pending_.erase(it);

    if (success) {
        switch (pending.purpose) {
        case Purpose::Allocate: {
            NetAddress relayed;
            if (!ReadAddress(data, parsed, kAttrRelayedAddress, relayed)) {
                Fail(Failure::BadAnswer);
                return;
            }
            relayed_ = relayed;
            ReadAddress(data, parsed, kAttrMappedAddress, mapped_);
            uint32_t lifetime = kDefaultLifetimeS;
            ReadU32(data, parsed, kAttrLifetime, lifetime);
            lifetimeS_ = lifetime;
            refreshMs_ = nowMs + RefreshAfterMs(lifetime);
            state_ = State::Ready;
            Update(nowMs); // binds the peers that were set before the allocation
            return;
        }
        case Purpose::Refresh: {
            uint32_t lifetime = kDefaultLifetimeS;
            ReadU32(data, parsed, kAttrLifetime, lifetime);
            lifetimeS_ = lifetime;
            refreshMs_ = nowMs + RefreshAfterMs(lifetime);
            return;
        }
        case Purpose::Bind:
        case Purpose::Permit:
            OnPeerResult(pending, true, nowMs);
            return;
        }
        return;
    }

    const int code = ErrorCode(data, parsed);
    const std::string nonce = ReadText(data, parsed, kAttrNonce);
    const std::string realm = ReadText(data, parsed, kAttrRealm);
    const bool first401 = code == 401 && !pending.signedWithKey && pending.purpose == Purpose::Allocate;
    const bool stale = code == 438 && pending.stale < kMaxStaleNonce;
    if ((first401 || stale) && !nonce.empty()) {
        nonce_ = nonce;
        if (!realm.empty()) {
            realm_ = realm;
        }
        if (!realm_.empty()) {
            key_ = LongTermKey(credentials_.username, realm_, credentials_.password);
            haveKey_ = true;
            Peer* peer = pending.purpose == Purpose::Bind || pending.purpose == Purpose::Permit
                             ? FindPeer(pending.peer)
                             : nullptr;
            if (peer == nullptr && (pending.purpose == Purpose::Bind || pending.purpose == Purpose::Permit)) {
                return; // the peer was let go meanwhile
            }
            Issue(pending.purpose, peer, nowMs, stale ? pending.stale + 1 : 0);
            return;
        }
    }
    switch (pending.purpose) {
    case Purpose::Allocate:
    case Purpose::Refresh:
        Fail(code == 401 || code == 441 ? Failure::Unauthorized : Failure::Refused);
        return;
    case Purpose::Bind:
    case Purpose::Permit:
        OnPeerResult(pending, false, nowMs);
        return;
    }
}

void Client::OnPeerResult(const Pending& pending, bool ok, uint64_t nowMs) {
    Peer* peer = FindPeer(pending.peer);
    if (peer == nullptr) {
        return;
    }
    if (pending.purpose == Purpose::Bind) {
        if (ok) {
            peer->link = Link::Bound;
            peer->renewMs = nowMs + kRebindMs;
            return;
        }
        // The server would not bind, or did not answer: fall back to a permission and Send indications. The channel
        // number stays spent, since the server may hold it for this peer.
        peer->link = Link::Permitting;
        if (state_ == State::Ready && !Waiting(Purpose::Permit, peer->address)) {
            Issue(Purpose::Permit, peer, nowMs);
        }
        return;
    }
    if (ok) {
        peer->link = Link::Permitted;
        peer->renewMs = nowMs + kRebindMs;
    } else {
        peer->link = Link::Failed;
    }
}

void Client::SetPeers(const std::vector<NetAddress>& peers, uint64_t nowMs) {
    std::vector<NetAddress> wanted;
    for (const NetAddress& address : peers) {
        if (wanted.size() < kMaxPeers && std::find(wanted.begin(), wanted.end(), address) == wanted.end()) {
            wanted.push_back(address);
        }
    }
    for (size_t i = 0; i < peers_.size();) {
        if (std::find(wanted.begin(), wanted.end(), peers_[i].address) == wanted.end()) {
            DropPending(Purpose::Bind, peers_[i].address);
            DropPending(Purpose::Permit, peers_[i].address);
            peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    for (const NetAddress& address : wanted) {
        if (FindPeer(address) == nullptr) {
            Peer peer;
            peer.address = address;
            peers_.push_back(peer);
        }
    }
    Update(nowMs);
}

bool Client::SendTo(const NetAddress& peer, const uint8_t* data, size_t length) {
    const Peer* found = FindPeer(peer);
    if (state_ != State::Ready || found == nullptr || length > kMaxChannelPayload) {
        return false;
    }
    if (found->link == Link::Bound) {
        std::vector<uint8_t> out(kChannelHeaderBytes + length);
        PutU16(out.data(), found->channel);
        PutU16(out.data() + 2, static_cast<uint16_t>(length));
        if (length > 0) {
            std::copy(data, data + length, out.begin() + kChannelHeaderBytes);
        }
        send_(out.data(), out.size());
        return true;
    }
    if (found->link == Link::Permitted) {
        Builder builder(static_cast<uint16_t>(kSend | kIndication), NextTxid());
        builder.AddAddress(kAttrPeerAddress, peer);
        builder.Add(kAttrData, data, length);
        const std::vector<uint8_t> out = builder.Finish(nullptr, 0, false);
        send_(out.data(), out.size());
        return true;
    }
    return false;
}

void Client::Close() {
    if (state_ == State::Ready && haveKey_) {
        const std::vector<uint8_t> out = Request(Purpose::Refresh, nullptr, NextTxid(), 0);
        send_(out.data(), out.size());
    }
    state_ = State::Idle;
    failure_ = Failure::None;
    pending_.clear();
    for (Peer& peer : peers_) {
        peer.channel = 0;
        peer.link = Link::Binding;
        peer.renewMs = 0;
    }
}

bool Client::Bound(const NetAddress& peer) const {
    const Peer* found = FindPeer(peer);
    return state_ == State::Ready && found != nullptr && found->link == Link::Bound;
}

bool Client::Permitted(const NetAddress& peer) const {
    const Peer* found = FindPeer(peer);
    return state_ == State::Ready && found != nullptr && found->link == Link::Permitted;
}

Client::Peer* Client::FindPeer(const NetAddress& address) {
    for (Peer& peer : peers_) {
        if (peer.address == address) {
            return &peer;
        }
    }
    return nullptr;
}

const Client::Peer* Client::FindPeer(const NetAddress& address) const {
    for (const Peer& peer : peers_) {
        if (peer.address == address) {
            return &peer;
        }
    }
    return nullptr;
}

Client::Peer* Client::FindChannel(uint16_t channel) {
    for (Peer& peer : peers_) {
        if (peer.channel == channel && (peer.link == Link::Bound || peer.link == Link::Binding)) {
            return &peer;
        }
    }
    return nullptr;
}

} // namespace gdx::net::turn
