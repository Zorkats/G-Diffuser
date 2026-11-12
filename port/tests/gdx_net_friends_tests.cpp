// port/tests/gdx_net_friends_tests.cpp -- friend identity key, join proof and friend list codec (G-NET O7.1 to O7.3).
// Copyright (c) Zorkats
//
// Usage: gdx_net_friends_tests [fuzz rounds, default 3000]
//
// Checks the derived friend key against an independent derivation, the join proof for the right and for a changed
// room, host key, nonce, friend key and signature byte, and the friend list codec: list operations, round trips,
// every refusal, every truncation, every single bit flip, and a seeded fuzz of the decoder. The fuzz builds its own
// valid files and reseals the checksum, so the structural checks run and are not just caught by the digest.

#include "gdx_net_friends.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace friends = gdx::net::friends;
using Bytes = std::vector<unsigned char>;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

uint64_t gRng = 0x2545F4914F6CDD1DULL;

uint32_t Next() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return uint32_t(gRng >> 16);
}

Bytes RandomBytes(size_t n) {
    Bytes out(n);
    for (auto& b : out) {
        b = static_cast<unsigned char>(Next());
    }
    return out;
}

Bytes Text(const std::string& s) {
    Bytes out;
    for (const char c : s) {
        out.push_back(static_cast<unsigned char>(c));
    }
    return out;
}

friends::Key KeyIndex(uint32_t i) {
    friends::Key k{};
    for (size_t j = 0; j < k.size(); j++) {
        k[j] = static_cast<unsigned char>(i * 31u + static_cast<uint32_t>(j));
    }
    k[0] = static_cast<unsigned char>(i & 0xFFu);
    k[1] = static_cast<unsigned char>((i >> 8) & 0xFFu);
    return k;
}

std::string RandomName() {
    const std::string pool = "abcXYZ 09!~";
    const size_t length = 1 + Next() % 16;
    std::string name;
    for (size_t i = 0; i < length; i++) {
        name.push_back(pool[Next() % pool.size()]);
    }
    if (name.front() == ' ') {
        name.front() = 'q';
    }
    if (name.back() == ' ') {
        name.back() = 'z';
    }
    return name;
}

void AppendDigest(Bytes& bytes) {
    unsigned char digest[16];
    crypto_blake2b(digest, 16, bytes.data(), bytes.size());
    for (const unsigned char b : digest) {
        bytes.push_back(b);
    }
}

// Replaces the last 16 bytes with the digest of everything before them, so a mutated file reaches the structure
// checks instead of stopping at the checksum.
void Reseal(Bytes& bytes) {
    if (bytes.size() < 16) {
        return;
    }
    unsigned char digest[16];
    crypto_blake2b(digest, 16, bytes.data(), bytes.size() - 16);
    std::copy(digest, digest + 16, bytes.end() - 16);
}

// The file as the spec describes it, built here without the module. It is the independent layout check.
struct RawEntry {
    Bytes key;
    unsigned char nameLength;
    Bytes name;
};

RawEntry Entry(const friends::Key& key, const std::string& name) {
    RawEntry e;
    e.key.assign(key.begin(), key.end());
    e.nameLength = static_cast<unsigned char>(name.size());
    e.name = Text(name);
    return e;
}

Bytes RawList(unsigned char version, unsigned countField, const std::vector<RawEntry>& entries) {
    const unsigned char magic[4] = { 'G', 'N', 'F', 'L' };
    Bytes out(magic, magic + 4);
    out.push_back(version);
    out.push_back(static_cast<unsigned char>(countField & 0xFFu));
    out.push_back(static_cast<unsigned char>((countField >> 8) & 0xFFu));
    for (const RawEntry& e : entries) {
        out.insert(out.end(), e.key.begin(), e.key.end());
        out.push_back(e.nameLength);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    AppendDigest(out);
    return out;
}

Bytes EncodeOrFail(const friends::FriendList& list) {
    Bytes out;
    const bool ok = friends::Encode(list, out);
    Check(ok, "a valid friend list encodes");
    return out;
}

bool SameList(const friends::FriendList& a, const friends::FriendList& b) {
    if (a.entries.size() != b.entries.size()) {
        return false;
    }
    for (size_t i = 0; i < a.entries.size(); i++) {
        if ((a.entries[i].key != b.entries[i].key) || (a.entries[i].name != b.entries[i].name)) {
            return false;
        }
    }
    return true;
}

friends::FriendList RandomList(size_t n) {
    friends::FriendList list;
    for (size_t i = 0; i < n; i++) {
        friends::Add(list, KeyIndex(static_cast<uint32_t>(i)), RandomName());
    }
    return list;
}

void CheckDerivedKey(const Bytes& seedA, const Bytes& seedB) {
    friends::Key first{};
    friends::Key second{};
    friends::DeriveFriendKey(seedA.data(), first);
    friends::DeriveFriendKey(seedA.data(), second);
    Check(first == second, "the friend key is the same every time for one seed");

    // Derived independently here: BLAKE2b-256 keyed with the seed over the label, then the Ed25519 pair of it.
    const char label[] = "G-NET friend identity v1";
    unsigned char friendSeed[32];
    crypto_blake2b_keyed(friendSeed, 32, seedA.data(), 32, reinterpret_cast<const unsigned char*>(label),
                         sizeof(label) - 1);
    unsigned char secret[64];
    unsigned char pub[32];
    crypto_ed25519_key_pair(secret, pub, friendSeed);
    Check(std::equal(pub, pub + 32, first.begin()), "the friend key is the Ed25519 key of BLAKE2b(seed, label)");

    // Raw seed key pair, as course signing uses it.
    unsigned char rawSeed[32];
    std::copy(seedA.begin(), seedA.end(), rawSeed);
    unsigned char rawSecret[64];
    unsigned char rawPub[32];
    crypto_ed25519_key_pair(rawSecret, rawPub, rawSeed);
    Check(!std::equal(rawPub, rawPub + 32, first.begin()),
          "the friend key differs from the key of the raw seed, so one use cannot reach the other");

    friends::Key other{};
    friends::DeriveFriendKey(seedB.data(), other);
    Check(other != first, "two seeds give two friend keys");

    Bytes flipped = seedA;
    flipped[0] ^= 0x01;
    friends::Key near{};
    friends::DeriveFriendKey(flipped.data(), near);
    Check(near != first, "a one-bit change in the seed changes the friend key");
}

void CheckJoinProof(const Bytes& joinerSeed, const Bytes& otherSeed, const Bytes& hostSeed) {
    friends::Key joiner{};
    friends::DeriveFriendKey(joinerSeed.data(), joiner);
    friends::Key stranger{};
    friends::DeriveFriendKey(otherSeed.data(), stranger);
    friends::Key host{};
    friends::DeriveFriendKey(hostSeed.data(), host);
    const Bytes otherHostSeed = RandomBytes(32);
    friends::Key otherHost{};
    friends::DeriveFriendKey(otherHostSeed.data(), otherHost);

    friends::Nonce nonce{};
    const Bytes nonceBytes = RandomBytes(32);
    std::copy(nonceBytes.begin(), nonceBytes.end(), nonce.begin());
    friends::Nonce otherNonce = nonce;
    otherNonce[17] ^= 0x40;

    const uint32_t room = 0x01020304u;
    friends::Signature sig{};
    friends::JoinProofSign(joinerSeed.data(), room, host, nonce, sig);
    Check(friends::JoinProofCheck(joiner, room, host, nonce, sig), "a join proof checks for its room, host and nonce");

    friends::Signature again{};
    friends::JoinProofSign(joinerSeed.data(), room, host, nonce, again);
    Check(again == sig, "the join proof signature is deterministic");

    Check(!friends::JoinProofCheck(joiner, room + 1, host, nonce, sig), "a proof for another room id fails");
    Check(!friends::JoinProofCheck(joiner, room, otherHost, nonce, sig), "a proof for another host key fails");
    Check(!friends::JoinProofCheck(joiner, room, host, otherNonce, sig), "a proof for another nonce fails");
    Check(!friends::JoinProofCheck(stranger, room, host, nonce, sig), "a proof under another friend key fails");

    bool everyFlipFails = true;
    for (size_t i = 0; i < sig.size(); i++) {
        friends::Signature bad = sig;
        bad[i] ^= 0x01;
        if (friends::JoinProofCheck(joiner, room, host, nonce, bad)) {
            everyFlipFails = false;
            std::printf("FAIL: a flipped signature byte %zu still checks\n", i);
        }
    }
    Check(everyFlipFails, "a flipped bit in any signature byte fails");

    // The signed message, assembled here: label, room id as u32 little-endian, host key, nonce.
    Bytes message = Text("G-NET join proof v1");
    for (int shift = 0; shift < 32; shift += 8) {
        message.push_back(static_cast<unsigned char>((room >> shift) & 0xFFu));
    }
    message.insert(message.end(), host.begin(), host.end());
    message.insert(message.end(), nonce.begin(), nonce.end());
    Check(message.size() == 87, "the join proof message is 87 bytes");
    Check(crypto_ed25519_check(sig.data(), joiner.data(), message.data(), message.size()) == 0,
          "the signed message is the label, the room id little-endian, the host key and the nonce");

    Bytes bigEndian = message;
    for (size_t i = 0; i < 4; i++) {
        bigEndian[19 + i] = message[19 + 3 - i];
    }
    Check(crypto_ed25519_check(sig.data(), joiner.data(), bigEndian.data(), bigEndian.size()) != 0,
          "a big-endian room id would not check, so the byte order is pinned");
}

void CheckListOperations() {
    friends::FriendList list;
    Check(friends::Add(list, KeyIndex(1), "Ana") == friends::AddResult::Added, "a new key is added");
    Check(friends::Add(list, KeyIndex(2), "Bo") == friends::AddResult::Added, "a second key is added");
    Check(friends::Add(list, KeyIndex(1), "Ana B") == friends::AddResult::Updated,
          "a listed key is updated, not added twice");
    Check(list.entries.size() == 2 && list.entries[0].name == "Ana B", "the update keeps the last name seen");
    Check(friends::Find(list, KeyIndex(2)) == 1, "Find returns the index of a listed key");
    Check(friends::Find(list, KeyIndex(9)) == -1, "Find returns -1 for an unlisted key");

    const char* badNames[] = { "", " lead", "trail ", "tab\there", "\x7F", "\xC3\xA9", "ABCDEFGHIJKLMNOPQ" };
    for (const char* name : badNames) {
        const size_t before = list.entries.size();
        Check(friends::Add(list, KeyIndex(50), name) == friends::AddResult::BadName,
              std::string("a bad name is refused: [") + name + "]");
        Check(list.entries.size() == before, "a refused name leaves the list alone");
    }
    Check(friends::Add(list, KeyIndex(50), "ABCDEFGHIJKLMNOP") == friends::AddResult::Added,
          "a 16-byte name is accepted");
    Check(friends::Add(list, KeyIndex(51), "A B") == friends::AddResult::Added, "a name with an inner space is accepted");
    Check(friends::IsValidName("x") && !friends::IsValidName(""), "IsValidName accepts one byte and refuses an empty name");

    Check(friends::Remove(list, KeyIndex(1)), "a listed key is removed");
    Check(!friends::Remove(list, KeyIndex(1)), "a key that is not listed cannot be removed");
    Check(friends::Find(list, KeyIndex(2)) == 0, "the friends after a removed one move up");

    friends::FriendList full;
    for (uint32_t i = 0; i < friends::kMaxFriends; i++) {
        Check(friends::Add(full, KeyIndex(i), "Full") == friends::AddResult::Added, "the list takes 200 keys");
    }
    Check(friends::Add(full, KeyIndex(999), "Over") == friends::AddResult::Full, "a 201st key is refused as full");
    Check(friends::Add(full, KeyIndex(5), "Still") == friends::AddResult::Updated,
          "a full list still updates a key it has");
}

void CheckRoundTrips() {
    for (const size_t n : { size_t(0), size_t(1), size_t(5), size_t(friends::kMaxFriends) }) {
        const friends::FriendList list = RandomList(n);
        const Bytes encoded = EncodeOrFail(list);
        friends::FriendList back;
        const auto result = friends::Decode(encoded.data(), encoded.size(), back);
        Check(result == friends::DecodeResult::Ok, "a list of " + std::to_string(n) + " decodes");
        Check(SameList(back, list), "a list of " + std::to_string(n) + " comes back the same");
        Bytes again;
        Check(friends::Encode(back, again) && again == encoded, "a decoded list encodes to the same bytes");
    }

    // The encoder agrees with the layout written out by hand above.
    friends::FriendList two;
    friends::Add(two, KeyIndex(7), "Hi");
    friends::Add(two, KeyIndex(8), "Yo");
    std::vector<RawEntry> raw = { Entry(KeyIndex(7), "Hi"), Entry(KeyIndex(8), "Yo") };
    Check(EncodeOrFail(two) == RawList(1, 2, raw), "the encoder writes the layout the spec describes");
}

void CheckEncodeRefusals() {
    friends::FriendList duplicate;
    duplicate.entries.push_back(friends::Friend{ KeyIndex(3), "One" });
    duplicate.entries.push_back(friends::Friend{ KeyIndex(3), "Two" });
    Bytes out{ 0x11 };
    Check(!friends::Encode(duplicate, out) && out.size() == 1, "a list with a duplicate key does not encode");

    friends::FriendList badName;
    badName.entries.push_back(friends::Friend{ KeyIndex(4), "" });
    Check(!friends::Encode(badName, out) && out.size() == 1, "a list with an empty name does not encode");

    friends::FriendList over;
    for (uint32_t i = 0; i <= friends::kMaxFriends; i++) {
        over.entries.push_back(friends::Friend{ KeyIndex(i), "Over" });
    }
    Check(!friends::Encode(over, out) && out.size() == 1, "a 201-entry list does not encode");
}

void CheckDecodeRefusals() {
    const friends::FriendList list = RandomList(3);
    const Bytes good = EncodeOrFail(list);

    friends::FriendList sentinel;
    friends::Add(sentinel, KeyIndex(777), "Keep");
    auto refuse = [&](const Bytes& bytes, friends::DecodeResult want, const std::string& what) {
        friends::FriendList out = sentinel;
        const auto got = friends::Decode(bytes.data(), bytes.size(), out);
        Check(got == want, what + " (got " + friends::DecodeResultName(got) + ")");
        Check(SameList(out, sentinel), what + ": the output is left alone");
    };

    Bytes badMagic = good;
    badMagic[0] = 'X';
    refuse(badMagic, friends::DecodeResult::BadMagic, "a bad magic is refused");

    refuse(RawList(0, 0, {}), friends::DecodeResult::BadVersion, "version 0 is refused");
    refuse(RawList(2, 0, {}), friends::DecodeResult::NewerVersion, "version 2 is refused as newer");
    refuse(RawList(255, 0, {}), friends::DecodeResult::NewerVersion, "version 255 is refused as newer");

    std::vector<RawEntry> many;
    for (uint32_t i = 0; i < 201; i++) {
        many.push_back(Entry(KeyIndex(i), "Many"));
    }
    refuse(RawList(1, 201, many), friends::DecodeResult::BadCount, "a count of 201 is refused");
    refuse(RawList(1, 0xFFFF, {}), friends::DecodeResult::BadCount, "a count of 65535 is refused");

    refuse(RawList(1, 1, { Entry(KeyIndex(1), "") }), friends::DecodeResult::BadName,
           "a name length of 0 is refused");
    std::vector<RawEntry> longName = { RawEntry{ Bytes(32, 1), 17, Bytes(17, 'a') } };
    refuse(RawList(1, 1, longName), friends::DecodeResult::BadName, "a name of 17 bytes is refused");
    std::vector<RawEntry> control = { RawEntry{ Bytes(32, 1), 1, Bytes{ 0x01 } } };
    refuse(RawList(1, 1, control), friends::DecodeResult::BadName, "a control byte in a name is refused");
    std::vector<RawEntry> lead = { Entry(KeyIndex(1), " Ana") };
    refuse(RawList(1, 1, lead), friends::DecodeResult::BadName, "a leading space is refused");
    std::vector<RawEntry> trail = { Entry(KeyIndex(1), "Ana ") };
    refuse(RawList(1, 1, trail), friends::DecodeResult::BadName, "a trailing space is refused");
    std::vector<RawEntry> high = { RawEntry{ Bytes(32, 1), 1, Bytes{ 0x80 } } };
    refuse(RawList(1, 1, high), friends::DecodeResult::BadName, "a byte above 0x7E is refused");

    refuse(RawList(1, 2, { Entry(KeyIndex(9), "One"), Entry(KeyIndex(9), "Two") }),
           friends::DecodeResult::Duplicate, "a key listed twice is refused");

    Bytes trailing = good;
    trailing.push_back(0x00);
    refuse(trailing, friends::DecodeResult::Trailing, "a byte after the checksum is refused");

    Bytes badSum = good;
    badSum.back() ^= 0x01;
    refuse(badSum, friends::DecodeResult::BadChecksum, "a changed checksum byte is refused");

    Bytes badKey = good;
    badKey[7] ^= 0x01;
    refuse(badKey, friends::DecodeResult::BadChecksum, "a changed key byte is refused by the checksum");

    refuse(Bytes(), friends::DecodeResult::Truncated, "an empty file is truncated");
    {
        friends::FriendList out = sentinel;
        Check(friends::Decode(nullptr, 0, out) == friends::DecodeResult::Truncated && SameList(out, sentinel),
              "a null buffer is refused as truncated");
    }
}

void CheckEveryTruncation() {
    const friends::FriendList list = RandomList(4);
    const Bytes good = EncodeOrFail(list);
    bool allRefused = true;
    for (size_t cut = 0; cut < good.size(); cut++) {
        friends::FriendList out;
        if (friends::Decode(good.data(), cut, out) == friends::DecodeResult::Ok) {
            allRefused = false;
            std::printf("FAIL: a file cut to %zu bytes decoded\n", cut);
        }
    }
    Check(allRefused, "every truncation of a valid file is refused");
}

void CheckEveryBitFlip() {
    const friends::FriendList list = RandomList(3);
    const Bytes good = EncodeOrFail(list);
    bool allRefused = true;
    for (size_t i = 0; i < good.size(); i++) {
        for (int bit = 0; bit < 8; bit++) {
            Bytes bad = good;
            bad[i] = static_cast<unsigned char>(bad[i] ^ (1u << bit));
            friends::FriendList out;
            if (friends::Decode(bad.data(), bad.size(), out) == friends::DecodeResult::Ok) {
                allRefused = false;
                std::printf("FAIL: a flipped bit at byte %zu bit %d decoded\n", i, bit);
            }
        }
    }
    Check(allRefused, "every single bit flip of a valid file is refused");
}

void Fuzz(int rounds, std::map<std::string, int>& tally) {
    for (int round = 0; round < rounds; round++) {
        const friends::FriendList list = RandomList(Next() % 7);
        Bytes bytes = EncodeOrFail(list);
        const int edits = 1 + static_cast<int>(Next() % 6);
        for (int e = 0; e < edits && !bytes.empty(); e++) {
            const size_t at = (Next() & 1) ? Next() % std::min<size_t>(bytes.size(), 40) : Next() % bytes.size();
            switch (Next() % 6) {
                case 0:
                    bytes[at] = static_cast<unsigned char>(bytes[at] ^ (1u << (Next() % 8)));
                    break;
                case 1:
                    bytes[at] = static_cast<unsigned char>(Next());
                    break;
                case 2:
                    bytes.resize(at);
                    break;
                case 3:
                    bytes.insert(bytes.begin() + static_cast<long>(at), static_cast<unsigned char>(Next()));
                    break;
                case 4:
                    bytes.erase(bytes.begin() + static_cast<long>(at));
                    break;
                default:
                    bytes[at] = static_cast<unsigned char>((Next() & 1) ? 0xFF : 0x00);
                    break;
            }
        }
        if ((Next() & 1) != 0) {
            Reseal(bytes);
        }

        friends::FriendList out;
        const auto verdict = friends::Decode(bytes.data(), bytes.size(), out);
        tally[friends::DecodeResultName(verdict)]++;
        if (verdict == friends::DecodeResult::Ok) {
            Bytes again;
            Check(friends::Encode(out, again) && again == bytes, "an accepted fuzzed file encodes back to itself");
        }
    }

    // Noise behind the magic and the version: a valid-looking header and random bytes after it.
    for (int round = 0; round < rounds / 4; round++) {
        Bytes noise = RandomBytes(Next() % 300);
        if (noise.size() >= 7) {
            const unsigned char head[7] = { 'G', 'N', 'F', 'L', 1, 0, 0 };
            std::copy(head, head + 7, noise.begin());
        }
        friends::FriendList out;
        const auto verdict = friends::Decode(noise.data(), noise.size(), out);
        tally[friends::DecodeResultName(verdict)]++;
        Check(verdict != friends::DecodeResult::Ok, "random bytes behind a valid header do not decode");
    }
}

} // namespace

// The directory signer (CONTRACT.md §8a in gnet-directory): it signs only a directory message, under the friend key.
void CheckDirectorySign(const Bytes& seed, const Bytes& otherSeed) {
    friends::Key key{};
    friends::DeriveFriendKey(seed.data(), key);
    friends::Key other{};
    friends::DeriveFriendKey(otherSeed.data(), other);

    const Bytes message = Text("G-NET directory v1\nPOST /v1/rooms\n{\"code\":\"ABC234\"}");
    friends::Signature sig{};
    Check(friends::DirectorySign(seed.data(), message.data(), message.size(), sig), "a directory message is signed");
    Check(crypto_ed25519_check(sig.data(), key.data(), message.data(), message.size()) == 0,
          "the directory signature checks under the friend key");
    Check(crypto_ed25519_check(sig.data(), other.data(), message.data(), message.size()) != 0,
          "the directory signature fails under another key");

    friends::Signature keep{};
    keep.fill(0x77);
    const Bytes join = Text("G-NET join proof v1");
    Check(!friends::DirectorySign(seed.data(), join.data(), join.size(), keep), "a join proof label is not signed");
    const Bytes noLineFeed = Text("G-NET directory v1 POST");
    Check(!friends::DirectorySign(seed.data(), noLineFeed.data(), noLineFeed.size(), keep),
          "the label without its line feed is not signed");
    const Bytes shortText = Text("G-NET");
    Check(!friends::DirectorySign(seed.data(), shortText.data(), shortText.size(), keep), "a short message is not signed");
    Check(!friends::DirectorySign(seed.data(), nullptr, 0, keep), "no message is not signed");
    bool untouched = true;
    for (unsigned char b : keep) {
        untouched = untouched && b == 0x77;
    }
    Check(untouched, "a refused message leaves the signature alone");
}

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3000;

    const Bytes seedA = RandomBytes(32);
    const Bytes seedB = RandomBytes(32);
    const Bytes seedC = RandomBytes(32);
    CheckDerivedKey(seedA, seedB);
    CheckJoinProof(seedA, seedB, seedC);
    CheckDirectorySign(seedA, seedB);

    CheckListOperations();
    CheckRoundTrips();
    CheckEncodeRefusals();
    CheckDecodeRefusals();
    CheckEveryTruncation();
    CheckEveryBitFlip();

    std::map<std::string, int> tally;
    Fuzz(rounds, tally);
    std::printf("fuzz verdicts:");
    for (const auto& [name, n] : tally) {
        std::printf(" %s %d,", name.c_str(), n);
    }
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
