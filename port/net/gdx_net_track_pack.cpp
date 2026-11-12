// port/net/gdx_net_track_pack.cpp -- a custom track's identity and the check a receiver runs on its bytes (G-NET O4.1).
// Copyright (c) Zorkats

#include "gdx_net_track_pack.h"

#include "gdx_sha256.h"

#include <utility>

namespace gdx::net::track {

TrackId Identify(const unsigned char* bytes, size_t length) {
    gdx::hash::Sha256 state;
    gdx::hash::sha256_init(&state);
    gdx::hash::sha256_update(&state, bytes, length);
    TrackId id{};
    gdx::hash::sha256_final(&state, id.data());
    return id;
}

const char* VerdictName(Verdict verdict) {
    switch (verdict) {
        case Verdict::Ok:
            return "ok";
        case Verdict::Empty:
            return "empty";
        case Verdict::Oversized:
            return "oversized";
        case Verdict::WrongIdentity:
            return "wrong identity";
        case Verdict::Undecodable:
            return "undecodable";
        case Verdict::NotCanonical:
            return "not canonical";
    }
    return "unknown";
}

Verdict Verify(const unsigned char* bytes, size_t length, const TrackId& expected, gdx::snapshot::Generation* out) {
    if (bytes == nullptr || length == 0) {
        return Verdict::Empty;
    }
    if (length > kMaxBytes) {
        return Verdict::Oversized;
    }
    if (Identify(bytes, length) != expected) {
        return Verdict::WrongIdentity;
    }
    const gdx::snapshot::Bytes buffer(bytes, bytes + length);
    gdx::snapshot::Generation decoded;
    if (!gdx::snapshot::decode(buffer, decoded)) {
        return Verdict::Undecodable;
    }
    // Content bytes are a fixed point of content(): anything else is a generation in some other form, and two
    // copies could hold the same track under two identities.
    gdx::snapshot::Bytes canonical;
    if (!gdx::snapshot::content(decoded, canonical) || canonical != buffer) {
        return Verdict::NotCanonical;
    }
    if (out != nullptr) {
        *out = std::move(decoded);
    }
    return Verdict::Ok;
}

std::string ToHex(const TrackId& id) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(kIdBytes * 2);
    for (const unsigned char byte : id) {
        text += kDigits[byte >> 4];
        text += kDigits[byte & 0x0F];
    }
    return text;
}

namespace {
int Nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
} // namespace

bool FromHex(const std::string& text, TrackId& out) {
    if (text.size() != kIdBytes * 2) {
        return false;
    }
    TrackId id{};
    for (size_t i = 0; i < kIdBytes; i++) {
        const int high = Nibble(text[i * 2]);
        const int low = Nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        id[i] = static_cast<unsigned char>(high << 4 | low);
    }
    out = id;
    return true;
}

} // namespace gdx::net::track
