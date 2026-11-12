// port/net/gdx_net_stun.cpp -- the RFC 8489 Binding codec and the client's retransmission (G-NET O5.6).
// Copyright (c) Zorkats

#include "gdx_net_stun.h"

#include <algorithm>

namespace gdx::net::stun {
namespace {

constexpr size_t kAttrHeaderBytes = 4;
constexpr size_t kFingerprintValueBytes = 4;
constexpr size_t kFingerprintAttrBytes = kAttrHeaderBytes + kFingerprintValueBytes;
constexpr uint8_t kFamilyIPv4 = 1; // RFC 8489 section 14.1
constexpr uint8_t kFamilyIPv6 = 2;
constexpr size_t kIPv4ValueBytes = 8;   // reserved, family, port, 4 address bytes
constexpr size_t kIPv6ValueBytes = 20;  // reserved, family, port, 16 address bytes

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
}

uint32_t ReadU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteU16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value & 0xFFu);
}

void WriteU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(value & 0xFFu);
}

// CRC-32 with the reflected ISO 3309 polynomial, the one RFC 8489 section 14.7 names. Bitwise, because the
// messages are a few hundred bytes and a table is more code to review than the loop is time.
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

// The FINGERPRINT of a message whose FINGERPRINT attribute would start at end. The header length has to count
// that attribute as well, so it is set to cover everything up to and including it (RFC 8489 section 14.7).
uint32_t FingerprintOf(const uint8_t* bytes, size_t end) {
    std::vector<uint8_t> covered(bytes, bytes + end);
    WriteU16(covered.data() + 2, static_cast<uint16_t>(end - kHeaderBytes + kFingerprintAttrBytes));
    return Crc32(covered.data(), covered.size()) ^ kFingerprintXor;
}

// An XOR-MAPPED-ADDRESS value XORs the port with the top half of the cookie, and the address with the cookie
// followed by the transaction id (RFC 8489 section 14.2). MAPPED-ADDRESS carries the same fields in clear.
Reply DecodeAddress(const uint8_t* value, size_t length, bool xored, const TransactionId& txid, MappedAddress& out) {
    if (length < 4) {
        return Reply::BadAddress;
    }
    uint8_t key[16] = {};
    WriteU32(key, kMagicCookie);
    std::copy(txid.begin(), txid.end(), key + 4);

    MappedAddress decoded;
    decoded.port = ReadU16(value + 2);
    if (xored) {
        decoded.port = static_cast<uint16_t>(decoded.port ^ static_cast<uint16_t>(kMagicCookie >> 16));
    }
    const uint8_t family = value[1];
    if (family == kFamilyIPv4) {
        if (length != kIPv4ValueBytes) {
            return Reply::BadAddress;
        }
        decoded.ip.family = 4;
        for (size_t i = 0; i < 4; i++) {
            decoded.ip.bytes[i] = static_cast<uint8_t>(value[4 + i] ^ (xored ? key[i] : 0));
        }
    } else if (family == kFamilyIPv6) {
        if (length != kIPv6ValueBytes) {
            return Reply::BadAddress;
        }
        decoded.ip.family = 6;
        for (size_t i = 0; i < 16; i++) {
            decoded.ip.bytes[i] = static_cast<uint8_t>(value[4 + i] ^ (xored ? key[i] : 0));
        }
    } else {
        return Reply::BadAddress;
    }
    out = decoded;
    return Reply::Ok;
}

// Where an attribute sits in the message, found by the walk and read after it.
struct Attr {
    bool present = false;
    size_t start = 0;
    size_t valueLength = 0;
};

} // namespace

const char* ReplyName(Reply reply) {
    switch (reply) {
    case Reply::Ok: return "ok";
    case Reply::Truncated: return "truncated";
    case Reply::Trailing: return "trailing bytes";
    case Reply::BadLength: return "length is not a multiple of 4";
    case Reply::NotSuccess: return "not a binding success response";
    case Reply::ErrorResponse: return "binding error response";
    case Reply::BadCookie: return "bad magic cookie";
    case Reply::WrongTransaction: return "transaction id does not match";
    case Reply::BadAttribute: return "malformed attribute";
    case Reply::BadAddress: return "malformed address attribute";
    case Reply::NoAddress: return "no mapped address";
    case Reply::BadFingerprint: return "fingerprint does not match";
    }
    return "unknown";
}

std::vector<uint8_t> BuildBindingRequest(const TransactionId& txid, bool fingerprint) {
    std::vector<uint8_t> message(kHeaderBytes, 0);
    WriteU16(message.data(), kTypeBindingRequest);
    WriteU16(message.data() + 2, static_cast<uint16_t>(fingerprint ? kFingerprintAttrBytes : 0));
    WriteU32(message.data() + 4, kMagicCookie);
    std::copy(txid.begin(), txid.end(), message.begin() + 8);
    if (fingerprint) {
        const uint32_t value = FingerprintOf(message.data(), message.size());
        const size_t at = message.size();
        message.resize(at + kFingerprintAttrBytes);
        WriteU16(message.data() + at, kAttrFingerprint);
        WriteU16(message.data() + at + 2, static_cast<uint16_t>(kFingerprintValueBytes));
        WriteU32(message.data() + at + kAttrHeaderBytes, value);
    }
    return message;
}

Reply ParseBindingReply(const uint8_t* bytes, size_t length, const TransactionId& txid, MappedAddress& out) {
    if (length < kHeaderBytes) {
        return Reply::Truncated;
    }
    const uint16_t type = ReadU16(bytes);
    if (type != kTypeBindingSuccess) {
        return type == kTypeBindingError ? Reply::ErrorResponse : Reply::NotSuccess;
    }
    const size_t declared = ReadU16(bytes + 2);
    if (declared % 4 != 0) {
        return Reply::BadLength;
    }
    if (length < kHeaderBytes + declared) {
        return Reply::Truncated;
    }
    if (length > kHeaderBytes + declared) {
        return Reply::Trailing;
    }
    if (ReadU32(bytes + 4) != kMagicCookie) {
        return Reply::BadCookie;
    }
    if (!std::equal(txid.begin(), txid.end(), bytes + 8)) {
        return Reply::WrongTransaction;
    }

    // Attributes are walked with every length checked before a byte is read. Unknown attributes are skipped,
    // comprehension-required ones included: the mapped address is all this client needs from the reply.
    const size_t end = kHeaderBytes + declared;
    Attr xorMapped;
    Attr mapped;
    Attr fingerprint;
    size_t pos = kHeaderBytes;
    while (pos < end) {
        if (end - pos < kAttrHeaderBytes) {
            return Reply::BadAttribute;
        }
        const uint16_t attrType = ReadU16(bytes + pos);
        const size_t valueLength = ReadU16(bytes + pos + 2);
        const size_t valueAt = pos + kAttrHeaderBytes;
        const size_t padded = (valueLength + 3) & ~static_cast<size_t>(3);
        if (padded > end - valueAt) {
            return Reply::BadAttribute;
        }
        const size_t next = valueAt + padded;
        const Attr here{true, pos, valueLength};
        if (attrType == kAttrFingerprint) {
            // Only a FINGERPRINT that closes the message can be checked: its CRC covers all that precedes it.
            if (valueLength != kFingerprintValueBytes || next != end) {
                return Reply::BadAttribute;
            }
            fingerprint = here;
        } else if (attrType == kAttrXorMappedAddress) {
            if (!xorMapped.present) {
                xorMapped = here;
            }
        } else if (attrType == kAttrMappedAddress) {
            if (!mapped.present) {
                mapped = here;
            }
        }
        pos = next;
    }

    if (fingerprint.present) {
        const uint32_t expected = FingerprintOf(bytes, fingerprint.start);
        if (expected != ReadU32(bytes + fingerprint.start + kAttrHeaderBytes)) {
            return Reply::BadFingerprint;
        }
    }
    // XOR-MAPPED-ADDRESS wins where both are present. A malformed one is an error, not a reason to fall back.
    if (xorMapped.present) {
        return DecodeAddress(bytes + xorMapped.start + kAttrHeaderBytes, xorMapped.valueLength, true, txid, out);
    }
    if (mapped.present) {
        return DecodeAddress(bytes + mapped.start + kAttrHeaderBytes, mapped.valueLength, false, txid, out);
    }
    return Reply::NoAddress;
}

void BindingClient::Start(uint64_t nowMs, const TransactionId& txid) {
    state_ = State::Waiting;
    txid_ = txid;
    nextSendMs_ = nowMs;
    giveUpMs_ = nowMs + kGiveUpMs;
    sends_ = 0;
    address_ = MappedAddress{};
}

void BindingClient::Expire(uint64_t nowMs) {
    if (state_ == State::Waiting && nowMs >= giveUpMs_) {
        state_ = State::Failed;
    }
}

std::optional<std::vector<uint8_t>> BindingClient::Pump(uint64_t nowMs) {
    Expire(nowMs);
    if (state_ != State::Waiting || sends_ >= kMaxSends || nowMs < nextSendMs_) {
        return std::nullopt;
    }
    sends_++;
    nextSendMs_ = nowMs + (kInitialRtoMs << (sends_ - 1));
    return BuildBindingRequest(txid_, fingerprint_);
}

bool BindingClient::OnDatagram(const uint8_t* bytes, size_t length, uint64_t nowMs) {
    Expire(nowMs);
    if (state_ != State::Waiting) {
        return false;
    }
    MappedAddress found;
    if (ParseBindingReply(bytes, length, txid_, found) != Reply::Ok) {
        return false;
    }
    address_ = found;
    state_ = State::Done;
    return true;
}

} // namespace gdx::net::stun
