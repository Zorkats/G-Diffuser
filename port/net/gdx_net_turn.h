// port/net/gdx_net_turn.h: a TURN client (RFC 8656) that gives this copy a relay address (G-NET decisions 69, 72).
// Copyright (c) Zorkats
//
// Pure, like the STUN client: no socket, no clock, no random source. The socket owner hands it every datagram
// that came from the TURN server's address, the time, and twelve random bytes per allocation; it says what to
// send to the server through a callback, and hands back what other machines sent to the relay address.
//
// Only what two room members need to reach each other through Cloudflare's relay:
// - Allocate, first without credentials to learn the realm and nonce (the 401), then signed with the long-term
//   credential: key MD5(username ":" realm ":" password), MESSAGE-INTEGRITY an HMAC-SHA1 under it.
// - Refresh before the allocation's lifetime runs out, and with lifetime 0 when the room is left.
// - ChannelBind for every peer relay address the room gives, rebound before its permission lapses; poses then go
//   as ChannelData, four bytes of overhead. A peer the server will not bind gets CreatePermission and Send
//   indications instead.
// - A 438 (stale nonce) answer is retried with the new nonce. Any other failure ends the allocation: the owner
//   gets new credentials and starts again, and the room keeps the host's relay meanwhile.
//
// Success answers to signed requests count only with a valid MESSAGE-INTEGRITY. Error answers and peer data are
// not signed by TURN; a forged one costs at most an allocation, and the poses inside carry their own MAC
// (gdx_net_peer_links.h).

#pragma once

#include "gdx_net_socket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gdx::net::turn {

constexpr uint32_t kMagicCookie = 0x2112A442u; // RFC 8489 section 5
constexpr size_t kHeaderBytes = 20;
constexpr size_t kTransactionIdBytes = 12;
using TransactionId = std::array<uint8_t, kTransactionIdBytes>;

// Message types: method with the class bits (RFC 8489 section 5). Every method used here is below 0x10, so a
// request is the method, an indication adds 0x10, a success 0x100 and an error 0x110.
constexpr uint16_t kAllocate = 0x0003;
constexpr uint16_t kRefresh = 0x0004;
constexpr uint16_t kSend = 0x0006;
constexpr uint16_t kData = 0x0007;
constexpr uint16_t kCreatePermission = 0x0008;
constexpr uint16_t kChannelBind = 0x0009;
constexpr uint16_t kIndication = 0x0010;
constexpr uint16_t kSuccess = 0x0100;
constexpr uint16_t kError = 0x0110;

// Attributes (RFC 8489 section 18.3, RFC 8656 section 18).
constexpr uint16_t kAttrUsername = 0x0006;
constexpr uint16_t kAttrIntegrity = 0x0008;
constexpr uint16_t kAttrErrorCode = 0x0009;
constexpr uint16_t kAttrChannelNumber = 0x000C;
constexpr uint16_t kAttrLifetime = 0x000D;
constexpr uint16_t kAttrPeerAddress = 0x0012;
constexpr uint16_t kAttrData = 0x0013;
constexpr uint16_t kAttrRealm = 0x0014;
constexpr uint16_t kAttrNonce = 0x0015;
constexpr uint16_t kAttrRelayedAddress = 0x0016;
constexpr uint16_t kAttrRequestedTransport = 0x0019;
constexpr uint16_t kAttrMappedAddress = 0x0020;
constexpr uint16_t kAttrFingerprint = 0x8028;

constexpr size_t kIntegrityBytes = 20;
constexpr uint8_t kTransportUdp = 17;
constexpr uint16_t kFirstChannel = 0x4000; // RFC 8656 section 12: 0x4000 to 0x4FFF
constexpr uint16_t kLastChannel = 0x4FFF;

// ---- Codec ----

// One message, attribute by attribute. Finish adds MESSAGE-INTEGRITY when a key is given, then FINGERPRINT.
class Builder {
public:
    Builder(uint16_t type, const TransactionId& txid);
    void Add(uint16_t attr, const uint8_t* value, size_t length); // zero padding to 4 bytes
    void AddText(uint16_t attr, const std::string& text);
    void AddU32(uint16_t attr, uint32_t value);
    void AddAddress(uint16_t attr, const NetAddress& address); // XOR form, RFC 8489 section 14.2
    std::vector<uint8_t> Finish(const uint8_t* key, size_t keyLength, bool fingerprint);

private:
    std::vector<uint8_t> bytes_;
    TransactionId txid_;
};

// Where one attribute's value sits in a parsed message.
struct Attr {
    uint16_t type = 0;
    size_t at = 0; // the value's offset in the message
    size_t length = 0;
};

struct Parsed {
    uint16_t type = 0;
    TransactionId txid{};
    std::vector<Attr> attrs;     // in order, up to and including MESSAGE-INTEGRITY; nothing after it but FINGERPRINT
    size_t integrityAt = 0;      // offset of the MESSAGE-INTEGRITY attribute header
    bool hasIntegrity = false;

    const Attr* Find(uint16_t type) const;
};

// Reads a STUN message: header, cookie, declared length, an attribute walk with every length checked, and the
// FINGERPRINT when present (it must close the message). Attributes after MESSAGE-INTEGRITY other than FINGERPRINT
// are ignored (RFC 8489 section 14.5). False for anything else.
bool Parse(const uint8_t* bytes, size_t length, Parsed& out);
// The MESSAGE-INTEGRITY of a parsed message under key.
bool IntegrityOk(const uint8_t* bytes, const Parsed& parsed, const uint8_t* key, size_t keyLength);
bool ReadAddress(const uint8_t* bytes, const Parsed& parsed, uint16_t attr, NetAddress& out);
bool ReadU32(const uint8_t* bytes, const Parsed& parsed, uint16_t attr, uint32_t& out);
std::string ReadText(const uint8_t* bytes, const Parsed& parsed, uint16_t attr);
// The ERROR-CODE as class * 100 + number; 0 without one.
int ErrorCode(const uint8_t* bytes, const Parsed& parsed);
// A ChannelData message (RFC 8656 section 12.4): its first two bits are 01.
bool IsChannelData(const uint8_t* bytes, size_t length);
// MD5(username ":" realm ":" password).
std::array<uint8_t, 16> LongTermKey(const std::string& username, const std::string& realm, const std::string& password);

// ---- Client ----

// Retransmission over UDP (RFC 8489 section 6.2.1, shortened like the STUN client): the RTO doubles from
// kInitialRtoMs, at most kMaxSends sends, then the request has failed.
constexpr uint64_t kInitialRtoMs = 250;
constexpr int kMaxSends = 5;
constexpr int kMaxStaleNonce = 3;         // 438 answers in a row before a request counts as failed
constexpr uint32_t kDefaultLifetimeS = 600;
constexpr uint64_t kRebindMs = 240000;     // permissions last 300 s, channels 600 s (RFC 8656 sections 9, 12)
constexpr size_t kMaxPeers = 16;           // relay peers kept at once; a room has at most 7 others

struct Credentials {
    std::string username;
    std::string password;
};

enum class Failure {
    None,
    Timeout,        // the server did not answer a request
    Unauthorized,   // 401 or 441 to a signed request: the credentials are wrong or expired
    Refused,        // any other error to Allocate or Refresh
    BadAnswer,      // a success without a usable relayed address
};
const char* FailureName(Failure failure);

class Client {
public:
    enum class State { Idle, Allocating, Ready, Failed };
    using SendToServer = std::function<void(const uint8_t* data, size_t length)>;
    using Deliver = std::function<void(const NetAddress& peer, const uint8_t* data, size_t length)>;

    Client(SendToServer send, Deliver deliver);

    // Starts an allocation. seed: twelve random bytes; transaction ids are derived from it with a counter.
    void Start(const Credentials& credentials, const TransactionId& seed, uint64_t nowMs);
    // Retransmissions, the allocation's refresh and the peers' rebinding.
    void Update(uint64_t nowMs);
    // A datagram from the TURN server's address. True when it was TURN traffic (it is then consumed).
    bool Receive(const uint8_t* data, size_t length, uint64_t nowMs);
    // The relay addresses this copy may exchange datagrams with. Bound or permitted while Ready; peers not in the
    // list are let go (the server's permission for them runs out on its own).
    void SetPeers(const std::vector<NetAddress>& peers, uint64_t nowMs);
    // Sends through the relay. False until the peer is bound or permitted.
    bool SendTo(const NetAddress& peer, const uint8_t* data, size_t length);
    // Ends the allocation with a Refresh of lifetime 0, sent once, and returns to Idle.
    void Close();

    State state() const {
        return state_;
    }
    Failure failure() const {
        return failure_;
    }
    // Valid while Ready.
    const NetAddress& relayed() const {
        return relayed_;
    }
    const NetAddress& mapped() const {
        return mapped_;
    }
    uint32_t lifetimeS() const {
        return lifetimeS_;
    }
    bool Bound(const NetAddress& peer) const;
    bool Permitted(const NetAddress& peer) const;

private:
    enum class Purpose { Allocate, Refresh, Bind, Permit };
    struct Pending {
        TransactionId txid{};
        Purpose purpose = Purpose::Allocate;
        NetAddress peer;
        uint16_t channel = 0;
        std::vector<uint8_t> bytes;
        bool signedWithKey = false; // a success answer then needs a valid MESSAGE-INTEGRITY
        uint64_t nextSendMs = 0;
        int sends = 0;
        int stale = 0;
    };
    enum class Link { Binding, Bound, Permitting, Permitted, Failed };
    struct Peer {
        NetAddress address;
        uint16_t channel = 0;
        Link link = Link::Binding;
        uint64_t renewMs = 0;
    };

    TransactionId NextTxid();
    std::vector<uint8_t> Request(Purpose purpose, const Peer* peer, const TransactionId& txid,
                                 uint32_t lifetimeS) const;
    void Issue(Purpose purpose, Peer* peer, uint64_t nowMs, int stale = 0);
    bool Waiting(Purpose purpose, const NetAddress& peer) const;
    void Fail(Failure failure);
    void OnAnswer(const uint8_t* data, const Parsed& parsed, uint64_t nowMs);
    void OnPeerResult(const Pending& pending, bool ok, uint64_t nowMs);
    Peer* FindPeer(const NetAddress& address);
    const Peer* FindPeer(const NetAddress& address) const;
    Peer* FindChannel(uint16_t channel);
    void DropPending(Purpose purpose, const NetAddress& peer);

    SendToServer send_;
    Deliver deliver_;
    State state_ = State::Idle;
    Failure failure_ = Failure::None;
    Credentials credentials_;
    std::string realm_;
    std::string nonce_;
    std::array<uint8_t, 16> key_{};
    bool haveKey_ = false;
    TransactionId seed_{};
    uint32_t counter_ = 0;
    std::vector<Pending> pending_;
    std::vector<Peer> peers_;
    uint16_t nextChannel_ = kFirstChannel;
    NetAddress relayed_;
    NetAddress mapped_;
    uint32_t lifetimeS_ = 0;
    uint64_t refreshMs_ = 0;
};

} // namespace gdx::net::turn
