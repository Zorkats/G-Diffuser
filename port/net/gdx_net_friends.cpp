// port/net/gdx_net_friends.cpp: the friend identity key, the join proof and the friend list codec. The rules are in
// gdx_net_friends.h.
// Copyright (c) Zorkats

#include "gdx_net_friends.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace gdx::net::friends {

namespace {

constexpr char kIdentityLabel[] = "G-NET friend identity v1";
constexpr size_t kIdentityLabelBytes = sizeof(kIdentityLabel) - 1;

constexpr char kJoinLabel[] = "G-NET join proof v1";
constexpr size_t kJoinLabelBytes = sizeof(kJoinLabel) - 1;
constexpr size_t kJoinMessageBytes = kJoinLabelBytes + 4 + kKeyBytes + kNonceBytes;

// The first line of every directory write, line feed included (CONTRACT.md §8a).
constexpr char kDirectoryLabel[] = "G-NET directory v1\n";
constexpr size_t kDirectoryLabelBytes = sizeof(kDirectoryLabel) - 1;

constexpr size_t kDigestBytes = 16;
constexpr size_t kHeaderBytes = 7;
constexpr unsigned char kMagic[4] = { 'G', 'N', 'F', 'L' };

// Derives the friend key pair. The seed is hashed under its own label, so the Ed25519 seed the friend key uses is
// never the profile seed itself. Monocypher wipes the seed it is given, and the explicit wipe keeps the rule here.
void DeriveKeyPair(const unsigned char profileSeed[kSeedBytes], unsigned char publicKey[kKeyBytes],
                   unsigned char secretKey[kSecretBytes]) {
    unsigned char friendSeed[kSeedBytes];
    crypto_blake2b_keyed(friendSeed, kSeedBytes, profileSeed, kSeedBytes,
                         reinterpret_cast<const unsigned char*>(kIdentityLabel), kIdentityLabelBytes);
    crypto_ed25519_key_pair(secretKey, publicKey, friendSeed);
    crypto_wipe(friendSeed, sizeof(friendSeed));
}

// Writes the signed message of a join proof. The room id is little-endian, the same as every integer in the lobby.
void BuildJoinMessage(uint32_t roomId, const Key& hostKey, const Nonce& hostNonce,
                      unsigned char message[kJoinMessageBytes]) {
    size_t pos = 0;
    for (size_t i = 0; i < kJoinLabelBytes; i++) {
        message[pos++] = static_cast<unsigned char>(kJoinLabel[i]);
    }
    for (int shift = 0; shift < 32; shift += 8) {
        message[pos++] = static_cast<unsigned char>((roomId >> shift) & 0xFFu);
    }
    for (size_t i = 0; i < kKeyBytes; i++) {
        message[pos++] = hostKey[i];
    }
    for (size_t i = 0; i < kNonceBytes; i++) {
        message[pos++] = hostNonce[i];
    }
}

// A name is 1 to kMaxNameBytes printable ASCII bytes, with no space at either end.
bool ValidNameBytes(const unsigned char* bytes, size_t length) {
    if ((length == 0) || (length > kMaxNameBytes)) {
        return false;
    }
    if ((bytes[0] == ' ') || (bytes[length - 1] == ' ')) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if ((bytes[i] < 0x20) || (bytes[i] > 0x7E)) {
            return false;
        }
    }
    return true;
}

// A list is valid when it has at most kMaxFriends entries, every name is valid, and no key is listed twice. The
// decoder checks the same rules, so a list that encodes always decodes.
bool IsValidList(const FriendList& list) {
    if (list.entries.size() > kMaxFriends) {
        return false;
    }
    for (size_t i = 0; i < list.entries.size(); i++) {
        if (!IsValidName(list.entries[i].name)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (list.entries[j].key == list.entries[i].key) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void DeriveFriendKey(const unsigned char profileSeed[kSeedBytes], Key& publicKey) {
    unsigned char secretKey[kSecretBytes];
    unsigned char derived[kKeyBytes];
    DeriveKeyPair(profileSeed, derived, secretKey);
    for (size_t i = 0; i < kKeyBytes; i++) {
        publicKey[i] = derived[i];
    }
    crypto_wipe(secretKey, sizeof(secretKey));
}

void JoinProofSign(const unsigned char profileSeed[kSeedBytes], uint32_t roomId, const Key& hostKey,
                   const Nonce& hostNonce, Signature& signature) {
    unsigned char secretKey[kSecretBytes];
    unsigned char publicKey[kKeyBytes];
    unsigned char message[kJoinMessageBytes];
    DeriveKeyPair(profileSeed, publicKey, secretKey);
    BuildJoinMessage(roomId, hostKey, hostNonce, message);
    crypto_ed25519_sign(signature.data(), secretKey, message, kJoinMessageBytes);
    crypto_wipe(secretKey, sizeof(secretKey));
}

bool DirectorySign(const unsigned char profileSeed[kSeedBytes], const unsigned char* message, size_t length,
                   Signature& signature) {
    if (message == nullptr || length < kDirectoryLabelBytes ||
        std::memcmp(message, kDirectoryLabel, kDirectoryLabelBytes) != 0) {
        return false;
    }
    unsigned char secretKey[kSecretBytes];
    unsigned char publicKey[kKeyBytes];
    DeriveKeyPair(profileSeed, publicKey, secretKey);
    crypto_ed25519_sign(signature.data(), secretKey, message, length);
    crypto_wipe(secretKey, sizeof(secretKey));
    return true;
}

bool JoinProofCheck(const Key& friendKey, uint32_t roomId, const Key& hostKey, const Nonce& hostNonce,
                    const Signature& signature) {
    unsigned char message[kJoinMessageBytes];
    BuildJoinMessage(roomId, hostKey, hostNonce, message);
    return crypto_ed25519_check(signature.data(), friendKey.data(), message, kJoinMessageBytes) == 0;
}

bool IsValidName(const std::string& name) {
    return ValidNameBytes(reinterpret_cast<const unsigned char*>(name.data()), name.size());
}

AddResult Add(FriendList& list, const Key& key, const std::string& name) {
    if (!IsValidName(name)) {
        return AddResult::BadName;
    }
    const int index = Find(list, key);
    if (index >= 0) {
        list.entries[static_cast<size_t>(index)].name = name;
        return AddResult::Updated;
    }
    if (list.entries.size() >= kMaxFriends) {
        return AddResult::Full;
    }
    Friend entry;
    entry.key = key;
    entry.name = name;
    list.entries.push_back(entry);
    return AddResult::Added;
}

bool Remove(FriendList& list, const Key& key) {
    const int index = Find(list, key);
    if (index < 0) {
        return false;
    }
    list.entries.erase(list.entries.begin() + index);
    return true;
}

int Find(const FriendList& list, const Key& key) {
    for (size_t i = 0; i < list.entries.size(); i++) {
        if (list.entries[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* DecodeResultName(DecodeResult result) {
    switch (result) {
        case DecodeResult::Ok:
            return "ok";
        case DecodeResult::Truncated:
            return "truncated";
        case DecodeResult::BadMagic:
            return "bad magic";
        case DecodeResult::BadVersion:
            return "bad version";
        case DecodeResult::NewerVersion:
            return "newer version";
        case DecodeResult::BadCount:
            return "bad count";
        case DecodeResult::BadName:
            return "bad name";
        case DecodeResult::Trailing:
            return "trailing bytes";
        case DecodeResult::BadChecksum:
            return "bad checksum";
        case DecodeResult::Duplicate:
            return "duplicate key";
    }
    return "unknown";
}

bool Encode(const FriendList& list, std::vector<unsigned char>& out) {
    if (!IsValidList(list)) {
        return false;
    }
    const size_t count = list.entries.size();
    std::vector<unsigned char> bytes;
    bytes.reserve(kHeaderBytes + count * (kKeyBytes + 1 + kMaxNameBytes) + kDigestBytes);
    for (size_t i = 0; i < sizeof(kMagic); i++) {
        bytes.push_back(kMagic[i]);
    }
    bytes.push_back(kListVersion);
    bytes.push_back(static_cast<unsigned char>(count & 0xFFu));
    bytes.push_back(static_cast<unsigned char>((count >> 8) & 0xFFu));
    for (const Friend& entry : list.entries) {
        for (size_t i = 0; i < kKeyBytes; i++) {
            bytes.push_back(entry.key[i]);
        }
        bytes.push_back(static_cast<unsigned char>(entry.name.size()));
        for (const char c : entry.name) {
            bytes.push_back(static_cast<unsigned char>(c));
        }
    }
    unsigned char digest[kDigestBytes];
    crypto_blake2b(digest, kDigestBytes, bytes.data(), bytes.size());
    for (size_t i = 0; i < kDigestBytes; i++) {
        bytes.push_back(digest[i]);
    }
    out = bytes;
    return true;
}

DecodeResult Decode(const unsigned char* data, size_t length, FriendList& out) {
    if ((data == nullptr) || (length < kHeaderBytes + kDigestBytes)) {
        return DecodeResult::Truncated;
    }
    for (size_t i = 0; i < sizeof(kMagic); i++) {
        if (data[i] != kMagic[i]) {
            return DecodeResult::BadMagic;
        }
    }
    if (data[4] == 0) {
        return DecodeResult::BadVersion;
    }
    if (data[4] > kListVersion) {
        return DecodeResult::NewerVersion;
    }
    const size_t count = static_cast<size_t>(data[5]) | (static_cast<size_t>(data[6]) << 8);
    if (count > kMaxFriends) {
        return DecodeResult::BadCount;
    }

    // Every length is read after checking that the bytes it describes are present, and the digest always has to
    // fit after the last entry. So no read goes past the buffer, and a truncated file is refused whatever its cut.
    FriendList parsed;
    parsed.entries.reserve(count);
    size_t pos = kHeaderBytes;
    for (size_t i = 0; i < count; i++) {
        if (length - pos < kKeyBytes + 1) {
            return DecodeResult::Truncated;
        }
        const size_t nameLength = static_cast<size_t>(data[pos + kKeyBytes]);
        if ((nameLength == 0) || (nameLength > kMaxNameBytes)) {
            return DecodeResult::BadName;
        }
        const size_t entryBytes = kKeyBytes + 1 + nameLength;
        if (length - pos < entryBytes + kDigestBytes) {
            return DecodeResult::Truncated;
        }
        const unsigned char* nameBytes = data + pos + kKeyBytes + 1;
        if (!ValidNameBytes(nameBytes, nameLength)) {
            return DecodeResult::BadName;
        }
        Friend entry;
        std::copy(data + pos, data + pos + kKeyBytes, entry.key.begin());
        entry.name.assign(reinterpret_cast<const char*>(nameBytes), nameLength);
        parsed.entries.push_back(entry);
        pos += entryBytes;
    }

    if (length - pos < kDigestBytes) {
        return DecodeResult::Truncated;
    }
    if (length - pos > kDigestBytes) {
        return DecodeResult::Trailing;
    }
    unsigned char digest[kDigestBytes];
    crypto_blake2b(digest, kDigestBytes, data, pos);
    unsigned int difference = 0;
    for (size_t i = 0; i < kDigestBytes; i++) {
        difference |= static_cast<unsigned int>(digest[i] ^ data[pos + i]);
    }
    if (difference != 0) {
        return DecodeResult::BadChecksum;
    }

    for (size_t i = 0; i < parsed.entries.size(); i++) {
        for (size_t j = 0; j < i; j++) {
            if (parsed.entries[j].key == parsed.entries[i].key) {
                return DecodeResult::Duplicate;
            }
        }
    }
    out = std::move(parsed);
    return DecodeResult::Ok;
}

} // namespace gdx::net::friends
