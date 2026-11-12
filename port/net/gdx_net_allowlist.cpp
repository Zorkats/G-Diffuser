// port/net/gdx_net_allowlist.cpp -- encoding, signing, decoding and the gate of the signed stable allowlist
// (G-NET O5.9). The format, the trust window and the order of the gate are described in gdx_net_allowlist.h.
// Copyright (c) Zorkats

#include "gdx_net_allowlist.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace gdx::net::allowlist {

namespace {

// "GNAL". It is checked before any length is read, so a wrong file is refused without a walk.
constexpr uint8_t kMagic[4] = {'G', 'N', 'A', 'L'};
// Magic, version, protocol, sequence, issued and grace_until: the fixed part before the first string.
constexpr size_t kFixedBytes = 4 + 1 + 4 + 8 + 8 + 8;

bool IsTag(const std::string& tag) {
    if (tag.empty() || tag.size() > kTagMax) {
        return false;
    }
    for (const char ch : tag) {
        const auto c = static_cast<uint8_t>(ch);
        // Space is excluded: the owner's text format splits on spaces, so a tag with one could not be typed.
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool IsReason(const std::string& reason) {
    if (reason.size() > kReasonMax) {
        return false;
    }
    for (const char ch : reason) {
        const auto c = static_cast<uint8_t>(ch);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool Contains(const std::vector<std::string>& tags, const std::string& tag) {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

bool HasDuplicate(const std::vector<std::string>& tags) {
    for (size_t i = 0; i < tags.size(); i++) {
        for (size_t j = i + 1; j < tags.size(); j++) {
            if (tags[i] == tags[j]) {
                return true;
            }
        }
    }
    return false;
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutStr8(std::vector<uint8_t>& out, const std::string& text) {
    out.push_back(static_cast<uint8_t>(text.size()));
    for (const char ch : text) {
        out.push_back(static_cast<uint8_t>(ch));
    }
}

uint32_t GetU32(const uint8_t* p) {
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) {
        value = (value << 8) | static_cast<uint32_t>(p[i]);
    }
    return value;
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | static_cast<uint64_t>(p[i]);
    }
    return value;
}

// Steps past one Str8 by its length byte alone. Returns false when the bytes end first.
bool SkipStr8(const uint8_t* data, size_t length, size_t& pos) {
    if (pos >= length) {
        return false;
    }
    const size_t n = data[pos];
    if (length - pos - 1 < n) {
        return false;
    }
    pos += 1 + n;
    return true;
}

// Reads one Str8 from a body that ScanBody has already walked. The bounds are checked again anyway.
bool ReadStr8(const uint8_t* data, size_t bodyLength, size_t& pos, std::string& out) {
    if (pos >= bodyLength) {
        return false;
    }
    const size_t n = data[pos];
    if (bodyLength - pos - 1 < n) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data + pos + 1), n);
    pos += 1 + n;
    return true;
}

// Walks the structure by its counts and lengths only, so the signature can be found before any field is read.
// Returns the length of the signed body. Truncation and trailing bytes both fail.
bool ScanBody(const std::vector<uint8_t>& bytes, size_t& bodyLength) {
    const uint8_t* data = bytes.data();
    const size_t length = bytes.size();
    if (length < kFixedBytes + kSignatureBytes) {
        return false;
    }
    if (std::memcmp(data, kMagic, sizeof kMagic) != 0 || data[4] != kFormatVersion) {
        return false;
    }
    size_t pos = kFixedBytes;
    if (!SkipStr8(data, length, pos) || pos >= length) {
        return false;
    }
    const size_t allowedCount = data[pos++];
    for (size_t i = 0; i < allowedCount; i++) {
        if (!SkipStr8(data, length, pos)) {
            return false;
        }
    }
    if (pos >= length) {
        return false;
    }
    const size_t yankedCount = data[pos++];
    for (size_t i = 0; i < yankedCount; i++) {
        if (!SkipStr8(data, length, pos) || !SkipStr8(data, length, pos)) {
            return false;
        }
    }
    if (length - pos != kSignatureBytes) {
        return false;
    }
    bodyLength = pos;
    return true;
}

// Reads the fields of a body that ScanBody has walked. A bad field returns false and leaves list half-written,
// which the caller discards.
bool ParseBody(const uint8_t* data, size_t bodyLength, List& list) {
    size_t pos = 4 + 1;  // magic and version were checked by ScanBody
    list.protocol = GetU32(data + pos);
    pos += 4;
    list.sequence = GetU64(data + pos);
    pos += 8;
    list.issued = GetU64(data + pos);
    pos += 8;
    list.graceUntil = GetU64(data + pos);
    pos += 8;
    if (!ReadStr8(data, bodyLength, pos, list.current) || pos >= bodyLength) {
        return false;
    }
    const size_t allowedCount = data[pos++];
    list.allowed.assign(allowedCount, std::string());
    for (std::string& tag : list.allowed) {
        if (!ReadStr8(data, bodyLength, pos, tag)) {
            return false;
        }
    }
    if (pos >= bodyLength) {
        return false;
    }
    const size_t yankedCount = data[pos++];
    list.yanked.assign(yankedCount, Yank());
    for (Yank& yank : list.yanked) {
        if (!ReadStr8(data, bodyLength, pos, yank.tag) || !ReadStr8(data, bodyLength, pos, yank.reason)) {
            return false;
        }
    }
    return pos == bodyLength;
}

// The fields in wire order, without the signature. Sign appends the signature to these bytes.
void EncodeBody(const List& list, std::vector<uint8_t>& out) {
    out.clear();
    out.insert(out.end(), kMagic, kMagic + sizeof kMagic);
    out.push_back(kFormatVersion);
    PutU32(out, list.protocol);
    PutU64(out, list.sequence);
    PutU64(out, list.issued);
    PutU64(out, list.graceUntil);
    PutStr8(out, list.current);
    out.push_back(static_cast<uint8_t>(list.allowed.size()));
    for (const std::string& tag : list.allowed) {
        PutStr8(out, tag);
    }
    out.push_back(static_cast<uint8_t>(list.yanked.size()));
    for (const Yank& yank : list.yanked) {
        PutStr8(out, yank.tag);
        PutStr8(out, yank.reason);
    }
}

Signature SignatureOf(const std::vector<uint8_t>& bytes) {
    Signature signature{};
    const size_t start = bytes.size() - kSignatureBytes;
    for (size_t i = 0; i < kSignatureBytes; i++) {
        signature[i] = bytes[start + i];
    }
    return signature;
}

}  // namespace

const char* VerdictName(Verdict verdict) {
    switch (verdict) {
        case Verdict::Ok:
            return "ok";
        case Verdict::BadSignature:
            return "bad signature";
        case Verdict::Malformed:
            return "malformed";
        case Verdict::Replayed:
            return "replayed";
    }
    return "unknown";
}

const char* RefusalName(Refusal refusal) {
    switch (refusal) {
        case Refusal::Allowed:
            return "allowed";
        case Refusal::NoList:
            return "no list";
        case Refusal::Stale:
            return "stale";
        case Refusal::ProtocolMismatch:
            return "protocol mismatch";
        case Refusal::Prerelease:
            return "prerelease";
        case Refusal::Yanked:
            return "yanked";
        case Refusal::NotListed:
            return "not listed";
    }
    return "unknown";
}

void KeyPairFromSeed(const Seed& seed, PublicKey& publicKey, SecretKey& secretKey) {
    Seed work = seed;  // Monocypher wipes the seed it is given, so the caller's seed stays as it was
    crypto_ed25519_key_pair(secretKey.data(), publicKey.data(), work.data());
    crypto_wipe(work.data(), work.size());
}

const char* CheckList(const List& list) {
    if (!IsTag(list.current)) {
        return "the current tag is not 1 to 48 printable characters without spaces";
    }
    if (list.allowed.empty() || list.allowed.size() > kMaxCount) {
        return "allowed needs 1 to 255 tags";
    }
    if (list.yanked.size() > kMaxCount) {
        return "yanked holds at most 255 entries";
    }
    for (const std::string& tag : list.allowed) {
        if (!IsTag(tag)) {
            return "an allowed tag is not 1 to 48 printable characters without spaces";
        }
    }
    if (!Contains(list.allowed, list.current)) {
        return "the current tag is not in allowed";
    }
    if (HasDuplicate(list.allowed)) {
        return "allowed repeats a tag";
    }
    std::vector<std::string> yankedTags;
    for (const Yank& yank : list.yanked) {
        if (!IsTag(yank.tag)) {
            return "a yanked tag is not 1 to 48 printable characters without spaces";
        }
        if (!IsReason(yank.reason)) {
            return "a yank reason is not 0 to 120 printable characters";
        }
        yankedTags.push_back(yank.tag);
    }
    if (HasDuplicate(yankedTags)) {
        return "yanked repeats a tag";
    }
    return nullptr;
}

bool Sign(const List& list, const SecretKey& secretKey, std::vector<uint8_t>& out) {
    if (CheckList(list) != nullptr) {
        return false;
    }
    std::vector<uint8_t> bytes;
    EncodeBody(list, bytes);
    Signature signature{};
    crypto_ed25519_sign(signature.data(), secretKey.data(), bytes.data(), bytes.size());
    bytes.insert(bytes.end(), signature.begin(), signature.end());
    out = std::move(bytes);
    return true;
}

Verdict Decode(const std::vector<uint8_t>& bytes, const PublicKey& publicKey, List& out) {
    if (bytes.size() > kMaxBytes) {
        return Verdict::Malformed;
    }
    size_t bodyLength = 0;
    if (!ScanBody(bytes, bodyLength)) {
        return Verdict::Malformed;
    }
    // No field is read until the signature over the whole body has checked out.
    if (crypto_ed25519_check(bytes.data() + bodyLength, publicKey.data(), bytes.data(), bodyLength) != 0) {
        return Verdict::BadSignature;
    }
    List parsed;
    if (!ParseBody(bytes.data(), bodyLength, parsed) || CheckList(parsed) != nullptr) {
        return Verdict::Malformed;
    }
    out = std::move(parsed);
    return Verdict::Ok;
}

Verdict Accept(const std::vector<uint8_t>& bytes, const PublicKey& publicKey, uint64_t nowMonoMs, State& state) {
    List list;
    const Verdict verdict = Decode(bytes, publicKey, list);
    if (verdict != Verdict::Ok) {
        return verdict;
    }
    if (list.sequence < state.highestSequence) {
        return Verdict::Replayed;
    }
    const Signature signature = SignatureOf(bytes);
    // A sequence names one list. Under the same key, a second list with that sequence is an error or a replay, and
    // accepting it would let the copy hold two different lists as the newest.
    if (state.held && list.sequence == state.highestSequence && signature != state.heldSignature) {
        return Verdict::Replayed;
    }
    state.highestSequence = list.sequence;
    state.held = true;
    state.list = std::move(list);
    state.heldSignature = signature;
    state.acceptedMonoMs = nowMonoMs;
    return Verdict::Ok;
}

bool Trusted(const State& state, uint64_t nowMonoMs) {
    if (!state.held || nowMonoMs < state.acceptedMonoMs) {
        return false;
    }
    return nowMonoMs - state.acceptedMonoMs < kTrustMs;
}

Refusal Admit(const State& state, const std::string& ownTag, const std::string& ownChannel, uint32_t ownProtocol,
              uint64_t nowMonoMs, uint64_t nowUnixSeconds) {
    // Refused before the list is read: no list makes a nightly, dev or unknown build a stable release.
    if (ownChannel != "stable") {
        return Refusal::Prerelease;
    }
    if (!state.held) {
        return Refusal::NoList;
    }
    if (!Trusted(state, nowMonoMs)) {
        return Refusal::Stale;
    }
    const List& list = state.list;
    if (ownProtocol != list.protocol) {
        return Refusal::ProtocolMismatch;
    }
    for (const Yank& yank : list.yanked) {
        if (yank.tag == ownTag) {
            return Refusal::Yanked;
        }
    }
    if (ownTag == list.current) {
        return Refusal::Allowed;
    }
    if (!Contains(list.allowed, ownTag)) {
        return Refusal::NotListed;
    }
    // An older stable build stays admitted while listed, until grace_until (ODD 9.1). Zero means no deadline.
    if (list.graceUntil != 0 && nowUnixSeconds >= list.graceUntil) {
        return Refusal::NotListed;
    }
    return Refusal::Allowed;
}

std::string RejectCopy(Refusal refusal, const State& state, const std::string& ownTag, const std::string& ownChannel) {
    if (refusal == Refusal::Allowed) {
        return std::string();
    }
    const std::string required = state.held ? state.list.current : std::string("the latest stable release");
    const std::string needs = "Public rooms require " + required + ". Offline and LAN still work.";
    switch (refusal) {
        case Refusal::Allowed:
            break;
        case Refusal::Prerelease:
            if (ownChannel == "nightly") {
                return "This is a nightly (" + ownTag + "). " + needs;
            }
            if (ownChannel == "dev") {
                return "This is a development build (" + ownTag + "). " + needs;
            }
            return "This build (" + ownTag + ") is not a stable release. " + needs;
        case Refusal::NoList:
            return "Public rooms need the latest stable release list, which this copy has not fetched yet. "
                   "Offline and LAN still work.";
        case Refusal::Stale:
            return "Public rooms need a fresh check of the latest stable release, which has not succeeded for "
                   "24 hours. Offline and LAN still work.";
        case Refusal::ProtocolMismatch:
            return "This build (" + ownTag + ") uses a different network protocol. " + needs;
        case Refusal::Yanked: {
            std::string reason;
            for (const Yank& yank : state.list.yanked) {
                if (yank.tag == ownTag) {
                    reason = yank.reason;
                }
            }
            if (!reason.empty() && reason.back() == '.') {
                reason.pop_back();
            }
            const std::string detail = reason.empty() ? std::string(". ") : ": " + reason + ". ";
            return "This build (" + ownTag + ") was pulled" + detail + needs;
        }
        case Refusal::NotListed:
            return "This build (" + ownTag + ") is not on the list of allowed stable releases. " + needs;
    }
    return std::string();
}

StateBlob EncodeState(const State& state) {
    StateBlob blob{};
    for (size_t i = 0; i < kStateBytes; i++) {
        blob[i] = static_cast<uint8_t>(state.highestSequence >> (8 * i));
    }
    return blob;
}

State DecodeState(const StateBlob& blob) {
    State state;
    state.highestSequence = GetU64(blob.data());
    return state;
}

}  // namespace gdx::net::allowlist
