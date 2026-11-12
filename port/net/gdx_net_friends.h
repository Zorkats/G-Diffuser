// port/net/gdx_net_friends.h: the friend identity key, the join proof and the local friend list (G-NET O7.1 to O7.3).
// Copyright (c) Zorkats
//
// Pure: no file IO, no clock and no random source. The caller reads and writes the friend list through the recovery
// writer, and supplies the profile seed and the host's nonce. Secret keys are derived inside this module and wiped
// before it returns, so a caller never holds one.
//
// A friend is a key the player stored locally, with the name last seen. The friend key is derived from the profile
// seed for this purpose only, so course signing, which keeps the raw seed, is unaffected (decision 41).
//
// Friend list file. Integers are little-endian, as in the lobby:
//
//   0x00  magic      "GNFL"
//   0x04  version    u8 = 1; a higher number is refused as newer, 0 as bad
//   0x05  count      u16, 0..kMaxFriends
//   0x07  entries    count x { key 32 bytes, nameLength u8 (1..kMaxNameBytes), name bytes }
//   end   checksum   16 bytes, BLAKE2b-128 of every byte before it
//
// The checksum is BLAKE2b-128 rather than CRC-32. A 32-bit CRC lets about one corrupted file in four billion pass,
// and the digest is already linked for the keys, so the stronger check costs nothing. The digest is unkeyed: the
// file holds no secret, and a forged checksum only edits the local list of its own owner.
//
// The join proof is signed by the joiner, not the host. The host hands out a nonce, and the signed message binds
// that nonce to the room and the host's key, so a proof made for one room cannot be replayed into another (ODD 10,
// decision 42).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gdx::net::friends {

constexpr size_t kKeyBytes = 32;
constexpr size_t kSeedBytes = 32;
constexpr size_t kSecretBytes = 64;
constexpr size_t kNonceBytes = 32;
constexpr size_t kSignatureBytes = 64;
constexpr size_t kMaxFriends = 200;
constexpr size_t kMaxNameBytes = 16;
constexpr uint8_t kListVersion = 1;

using Key = std::array<unsigned char, kKeyBytes>;
using Nonce = std::array<unsigned char, kNonceBytes>;
using Signature = std::array<unsigned char, kSignatureBytes>;

// O7.2: the friend public key, derived from the 32-byte profile seed as BLAKE2b-256 keyed with the seed over the
// message "G-NET friend identity v1" (no terminator), then the Ed25519 key pair of that value.
void DeriveFriendKey(const unsigned char profileSeed[kSeedBytes], Key& publicKey);

// O7.3: the joiner signs "G-NET join proof v1" (no terminator), the room id as u32 little-endian, the host's public
// key and the host's nonce. profileSeed is the joiner's own seed.
void JoinProofSign(const unsigned char profileSeed[kSeedBytes], uint32_t roomId, const Key& hostKey,
                   const Nonce& hostNonce, Signature& signature);

// Returns true when signature is the joiner's proof for this room, host and nonce, under the joiner's friend key.
bool JoinProofCheck(const Key& friendKey, uint32_t roomId, const Key& hostKey, const Nonce& hostNonce,
                    const Signature& signature);

// Signs a room directory write with the friend key (CONTRACT.md §8a in the gnet-directory repository): the friend
// key is the room's host key there. Only a message that starts with "G-NET directory v1" and a line feed is signed,
// so this cannot be made to sign a join proof or anything else. false, and signature untouched, otherwise.
bool DirectorySign(const unsigned char profileSeed[kSeedBytes], const unsigned char* message, size_t length,
                   Signature& signature);

// O7.1: the local friend list. Entries keep their insertion order, and the order survives a round trip.
struct Friend {
    Key key;
    std::string name;
};

struct FriendList {
    std::vector<Friend> entries;
};

// A name is 1 to kMaxNameBytes bytes of printable ASCII (0x20 to 0x7E) with no space at either end. It matches the
// rule course authorship uses, and the ODD 11 safety row for names.
bool IsValidName(const std::string& name);

enum class AddResult {
    Added,
    Updated,   // the key was already listed; its name is now the one last seen
    Full,
    BadName,
};

// Adds a key, or refreshes the name of a key already listed. A full list still accepts an update.
AddResult Add(FriendList& list, const Key& key, const std::string& name);

// Returns true when a friend with this key was listed and is now removed.
bool Remove(FriendList& list, const Key& key);

// Returns the index of the friend with this key, or -1.
int Find(const FriendList& list, const Key& key);

enum class DecodeResult {
    Ok,
    Truncated,
    BadMagic,
    BadVersion,
    NewerVersion,
    BadCount,
    BadName,
    Trailing,
    BadChecksum,
    Duplicate,
};

const char* DecodeResultName(DecodeResult result);

// Writes the file bytes for list. Returns false, and writes nothing, when the list is not valid: more than
// kMaxFriends entries, a bad name, or a key listed twice.
bool Encode(const FriendList& list, std::vector<unsigned char>& out);

// Reads a whole file. out is written only on Ok. Every refusal is checked before a byte is trusted, and a buffer
// with bytes after the checksum is refused rather than read past.
DecodeResult Decode(const unsigned char* data, size_t length, FriendList& out);

} // namespace gdx::net::friends
