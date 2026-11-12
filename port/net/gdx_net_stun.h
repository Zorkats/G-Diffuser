// port/net/gdx_net_stun.h -- an RFC 8489 Binding client that learns the public address of a UDP socket (G-NET O5.6).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no random source. The caller owns the socket, passes the time in milliseconds
// and supplies the 96-bit transaction id, so every byte this file builds or reads can be checked with fixed
// packets. A BindingClient talks to one server; the caller keeps one per entry of the server list (plan
// decision 28).
//
// Only the mapped address is wanted, so the request carries no attributes, or only FINGERPRINT. The reply is
// read for XOR-MAPPED-ADDRESS, then MAPPED-ADDRESS. MESSAGE-INTEGRITY is walked past and never checked: a
// public server gives no credentials to check it with.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gdx::net::stun {

// An IP address of either family. Family 4 keeps its four bytes at the front; family 0 means no address.
struct IpAddress {
    uint8_t family = 0;
    uint8_t bytes[16] = {};
};

// An address with a port: a mapped address, a server's endpoint, or the local side of a socket.
struct MappedAddress {
    IpAddress ip;
    uint16_t port = 0;
};

constexpr uint32_t kMagicCookie = 0x2112A442u; // RFC 8489 section 5
constexpr size_t kHeaderBytes = 20;            // RFC 8489 section 6.1
constexpr size_t kTransactionIdBytes = 12;
using TransactionId = std::array<uint8_t, kTransactionIdBytes>;

// RFC 8489 section 6.2.1 starts at an RTO of 500 ms and allows Rc = 7 sends, which can run past 30 s. A join
// that waits that long has already failed, so this client sends at most kMaxSends requests, the RTO doubling
// from kInitialRtoMs, and gives up at kGiveUpMs whatever it has sent.
constexpr uint64_t kInitialRtoMs = 250;
constexpr int kMaxSends = 4;
constexpr uint64_t kGiveUpMs = 3000;

// Message types (RFC 8489 section 6.1) and attributes (section 18.2) this file knows.
constexpr uint16_t kTypeBindingRequest = 0x0001;
constexpr uint16_t kTypeBindingSuccess = 0x0101;
constexpr uint16_t kTypeBindingError = 0x0111;
constexpr uint16_t kAttrMappedAddress = 0x0001;
constexpr uint16_t kAttrXorMappedAddress = 0x0020;
constexpr uint16_t kAttrFingerprint = 0x8028;
// The FINGERPRINT is the CRC-32 of the message up to the attribute, XORed with this value (section 14.7).
constexpr uint32_t kFingerprintXor = 0x5354554Eu;

// A Binding Request for txid: the 20-byte header and, with fingerprint, a FINGERPRINT attribute.
std::vector<uint8_t> BuildBindingRequest(const TransactionId& txid, bool fingerprint);

enum class Reply {
    Ok,
    Truncated,      // fewer bytes than the header, or than the length the header declares
    Trailing,       // more bytes than the header declares
    BadLength,      // the declared length is not a multiple of 4
    NotSuccess,     // a request or an indication, not a Binding Success Response
    ErrorResponse,  // a Binding Error Response: the server refused the request
    BadCookie,
    WrongTransaction,
    BadAttribute,   // an attribute runs past the message, or FINGERPRINT is malformed or not last
    BadAddress,     // an address attribute with an unknown family or the wrong length
    NoAddress,      // neither XOR-MAPPED-ADDRESS nor MAPPED-ADDRESS
    BadFingerprint,
};

const char* ReplyName(Reply reply);

// Reads a Binding Success Response to the transaction txid. On Ok, out holds the mapped address; any other
// result leaves out untouched.
Reply ParseBindingReply(const uint8_t* bytes, size_t length, const TransactionId& txid, MappedAddress& out);

// One transaction with one server. Start begins it; the caller sends what Pump returns, feeds every datagram
// from the server to OnDatagram, and calls Pump again as time passes. Once the state is Done or Failed,
// Pump and OnDatagram do nothing.
class BindingClient {
public:
    enum class State { Idle, Waiting, Done, Failed };

    explicit BindingClient(bool fingerprint = true) : fingerprint_(fingerprint) {}

    void Start(uint64_t nowMs, const TransactionId& txid);
    // The request to send now, if one is due.
    std::optional<std::vector<uint8_t>> Pump(uint64_t nowMs);
    // True when the datagram answered this transaction; the address is then known.
    bool OnDatagram(const uint8_t* bytes, size_t length, uint64_t nowMs);

    State state() const { return state_; }
    // Valid once state() is Done.
    const MappedAddress& address() const { return address_; }
    int sends() const { return sends_; }

private:
    void Expire(uint64_t nowMs);

    bool fingerprint_;
    State state_ = State::Idle;
    TransactionId txid_{};
    uint64_t nextSendMs_ = 0;
    uint64_t giveUpMs_ = 0;
    int sends_ = 0;
    MappedAddress address_;
};

} // namespace gdx::net::stun
