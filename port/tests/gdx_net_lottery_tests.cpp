// port/tests/gdx_net_lottery_tests.cpp: the lottery commitment, its reveal, the draw seed and the payload decoders (G-NET O6.4).
// Copyright (c) Zorkats
//
// Usage: gdx_net_lottery_tests
// Prints one line per failed check, then "<N> checks, <M> failed". Exit 0 only when M is 0.
//
// The known-answer vectors were computed with Python's hashlib, from the byte layout in gdx_net_lottery.h and not from
// this code. The SHA-256 header itself is checked against the two standard NIST vectors first.

#include "gdx_net_lottery.h"

#include "gdx_sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using gdx::net::lobby::Ballot;
using gdx::net::lobby::kTrackCustom;
using gdx::net::lobby::kTrackRandom;
using gdx::net::lottery::Commit;
using gdx::net::lottery::Commitment;
using gdx::net::lottery::DecodeCommitment;
using gdx::net::lottery::DecodeReveal;
using gdx::net::lottery::DrawSalt;
using gdx::net::lottery::DrawSeed;
using gdx::net::lottery::EncodeCommitment;
using gdx::net::lottery::EncodeReveal;
using gdx::net::lottery::Salt;
using gdx::net::lottery::Seed;
using gdx::net::lottery::SeedHash;
using gdx::net::lottery::Verify;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Known answers. Room 0x01020304, race 5, salt bytes 0x00 to 0x1F, computed with Python's hashlib.
constexpr uint32_t kRoom = 0x01020304u;
constexpr uint32_t kRace = 5u;
const char kNistAbc[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char kNist448[] = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
const char kCommitKat[] = "58b6235a7fef8a388699a57c21b532ddfe2c408b1c5d491d08e17797296638c6";
const char kSeedKat[] = "823a19ff775e15077bdaf853aa889d320f387324dde36e874a75cce51d7135ab";
constexpr uint32_t kSeedHashKat = 0xff193a82u;

// xorshift64 with a fixed seed: every run sees the same random bytes.
uint64_t gRng = 0x2545F4914F6CDD1DULL;

uint64_t NextRandom() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return gRng;
}

uint8_t NextByte() {
    return static_cast<uint8_t>(NextRandom() >> 24);
}

std::string Hex(const uint8_t* data, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < length; i++) {
        text.push_back(digits[data[i] >> 4]);
        text.push_back(digits[data[i] & 0x0F]);
    }
    return text;
}

Salt KatSalt() {
    Salt salt{};
    for (size_t i = 0; i < salt.size(); i++) {
        salt[i] = static_cast<uint8_t>(i);
    }
    return salt;
}

Ballot StockBallot(uint8_t id, uint8_t track) {
    Ballot ballot;
    ballot.id = id;
    ballot.track = track;
    return ballot;
}

// A custom ballot whose identity is base, base+1, ... and whose title is a display name only.
Ballot CustomBallot(uint8_t id, uint8_t base, const std::string& title) {
    Ballot ballot;
    ballot.id = id;
    ballot.track = kTrackCustom;
    for (size_t i = 0; i < ballot.custom.id.size(); i++) {
        ballot.custom.id[i] = static_cast<uint8_t>(base + i);
    }
    ballot.custom.title = title;
    return ballot;
}

std::vector<Ballot> KatBallots() {
    return {StockBallot(0, 3), StockBallot(2, kTrackRandom), CustomBallot(5, 0x40, "Ana")};
}

void CheckSha256Header() {
    // The module relies on port/gdx_sha256.h. These two vectors pin it: one block, and two blocks with the padding
    // crossing into a second block.
    gdx::hash::Sha256 state;
    uint8_t digest[32];
    gdx::hash::sha256_init(&state);
    gdx::hash::sha256_update(&state, "abc", 3);
    gdx::hash::sha256_final(&state, digest);
    Check(Hex(digest, sizeof(digest)) == kNistAbc, "gdx_sha256.h: SHA-256 of \"abc\" matches the NIST vector");

    const char text448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    gdx::hash::sha256_init(&state);
    gdx::hash::sha256_update(&state, text448, 56);
    gdx::hash::sha256_final(&state, digest);
    Check(Hex(digest, sizeof(digest)) == kNist448, "gdx_sha256.h: SHA-256 of the 448-bit NIST message matches");
}

void CheckCommitment() {
    const Salt salt = KatSalt();
    const Commitment commitment = Commit(kRoom, kRace, salt);
    Check(Hex(commitment.data(), commitment.size()) == kCommitKat, "the commitment matches the known-answer vector");
    Check(Verify(commitment, kRoom, kRace, salt), "the reveal verifies against its commitment");
    Check(!Verify(commitment, kRoom + 1, kRace, salt), "a reveal checked against another room is refused");
    Check(!Verify(commitment, kRoom, kRace + 1, salt), "a reveal checked against another race is refused");
    Check(!Verify(commitment, kRace, kRoom, salt), "the room and the race are not interchangeable");

    // A reveal one bit off is refused, whichever of its 256 bits it is.
    size_t refused = 0;
    for (size_t bit = 0; bit < 256; bit++) {
        Salt flipped = salt;
        flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        if (!Verify(commitment, kRoom, kRace, flipped)) {
            refused++;
        }
    }
    Check(refused == 256, "every one-bit flip of the reveal is refused (" + std::to_string(refused) + " of 256)");

    // A commitment one bit off matches no reveal.
    refused = 0;
    for (size_t bit = 0; bit < 256; bit++) {
        Commitment flipped = commitment;
        flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        if (!Verify(flipped, kRoom, kRace, salt)) {
            refused++;
        }
    }
    Check(refused == 256, "every one-bit flip of the commitment is refused (" + std::to_string(refused) + " of 256)");

    Salt other = salt;
    other[31] ^= 1;
    Check(Commit(kRoom, kRace, other) != commitment, "a different salt gives a different commitment");
}

void CheckDrawSalt() {
    Salt out{};
    out.fill(0xAA);
    Check(!DrawSalt(nullptr, 32, out), "a null buffer is refused");
    Check(out[0] == 0xAA && out[31] == 0xAA, "a refused draw leaves the salt alone");

    uint8_t buffer[40];
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = static_cast<uint8_t>(i);
    }
    Check(!DrawSalt(buffer, 31, out), "a 31-byte buffer is too short");
    Check(out[0] == 0xAA, "a short buffer leaves the salt alone");
    Check(DrawSalt(buffer, 32, out) && out == KatSalt(), "exactly 32 bytes become the salt");

    // Bytes past the first 32 are not part of the salt.
    uint8_t longer[40];
    std::copy(buffer, buffer + sizeof(buffer), longer);
    for (size_t i = 32; i < 40; i++) {
        longer[i] = 0x77;
    }
    out.fill(0);
    Check(DrawSalt(longer, sizeof(longer), out) && out == KatSalt(), "only the first 32 bytes of a longer buffer are the salt");

    // A generator that never ran leaves zeros behind, and a zero salt is refused.
    uint8_t zeros[64] = {};
    out.fill(0xAA);
    Check(!DrawSalt(zeros, sizeof(zeros), out), "an all-zero buffer is refused");
    Check(out[0] == 0xAA, "a refused zero buffer leaves the salt alone");
    uint8_t lastOnly[32] = {};
    lastOnly[31] = 1;
    Check(DrawSalt(lastOnly, sizeof(lastOnly), out) && out[31] == 1, "a buffer with one nonzero byte is accepted");
}

void CheckPayloads() {
    const Salt salt = KatSalt();
    const Commitment commitment = Commit(kRoom, kRace, salt);

    const std::vector<uint8_t> wire = EncodeCommitment(commitment);
    Check(wire.size() == 32, "a commitment payload is 32 bytes");
    Check(Hex(wire.data(), wire.size()) == kCommitKat, "the commitment payload is the commitment itself");
    Commitment back{};
    back.fill(0xAA);
    Check(DecodeCommitment(wire.data(), wire.size(), back) && back == commitment, "a commitment payload round-trips");

    const std::vector<uint8_t> reveal = EncodeReveal(salt);
    Check(reveal.size() == 32, "a reveal payload is 32 bytes");
    Salt revealed{};
    revealed.fill(0xAA);
    Check(DecodeReveal(reveal.data(), reveal.size(), revealed) && revealed == salt, "a reveal payload round-trips");

    std::vector<uint8_t> longer = wire;
    longer.push_back(0);
    const std::vector<uint8_t> shorter(wire.begin(), wire.end() - 1);
    Commitment keepCommitment{};
    keepCommitment.fill(0xAA);
    Check(!DecodeCommitment(longer.data(), longer.size(), keepCommitment), "a commitment with a trailing byte is refused");
    Check(!DecodeCommitment(shorter.data(), shorter.size(), keepCommitment), "a truncated commitment is refused");
    Check(!DecodeCommitment(nullptr, 0, keepCommitment), "an empty commitment is refused");
    Check(!DecodeCommitment(nullptr, 32, keepCommitment), "a null commitment buffer is refused");
    Check(keepCommitment[0] == 0xAA && keepCommitment[31] == 0xAA, "a refused commitment leaves the output alone");

    std::vector<uint8_t> revealLonger = reveal;
    revealLonger.push_back(0);
    const std::vector<uint8_t> revealShorter(reveal.begin(), reveal.end() - 1);
    Salt keepReveal{};
    keepReveal.fill(0xAA);
    Check(!DecodeReveal(revealLonger.data(), revealLonger.size(), keepReveal), "a reveal with a trailing byte is refused");
    Check(!DecodeReveal(revealShorter.data(), revealShorter.size(), keepReveal), "a truncated reveal is refused");
    Check(!DecodeReveal(nullptr, 0, keepReveal), "an empty reveal is refused");
    Check(!DecodeReveal(nullptr, 32, keepReveal), "a null reveal buffer is refused");
    Check(keepReveal[0] == 0xAA && keepReveal[31] == 0xAA, "a refused reveal leaves the output alone");
}

void CheckSeed() {
    const Salt salt = KatSalt();
    // Reverse order on purpose: the seed sorts by player id, so arrival order must not matter.
    std::vector<Ballot> reversed = KatBallots();
    std::reverse(reversed.begin(), reversed.end());
    const Seed seed = DrawSeed(salt, kRoom, kRace, reversed);
    Check(Hex(seed.data(), seed.size()) == kSeedKat, "the seed matches the known-answer vector");
    Check(SeedHash(seed) == kSeedHashKat, "the draw word is the seed's first four bytes, little-endian");
    Check(DrawSeed(salt, kRoom, kRace, KatBallots()) == seed, "ballot order does not change the seed");
    Check(reversed.front().id == 5, "the caller's ballot list keeps its order");

    // Every input the seed names changes it.
    size_t changed = 0;
    for (size_t bit = 0; bit < 256; bit++) {
        Salt flipped = salt;
        flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        if (DrawSeed(flipped, kRoom, kRace, KatBallots()) != seed) {
            changed++;
        }
    }
    Check(changed == 256, "every one-bit flip of the salt changes the seed (" + std::to_string(changed) + " of 256)");
    Check(DrawSeed(salt, kRoom + 1, kRace, KatBallots()) != seed, "another room gives another seed");
    Check(DrawSeed(salt, kRoom, kRace + 1, KatBallots()) != seed, "another race gives another seed");

    std::vector<Ballot> trackChanged = KatBallots();
    trackChanged[0].track = 4;
    Check(DrawSeed(salt, kRoom, kRace, trackChanged) != seed, "a changed stock ballot changes the seed");

    std::vector<Ballot> fewer = KatBallots();
    fewer.pop_back();
    Check(DrawSeed(salt, kRoom, kRace, fewer) != seed, "a missing ballot changes the seed");

    std::vector<Ballot> more = KatBallots();
    more.push_back(StockBallot(7, 1));
    Check(DrawSeed(salt, kRoom, kRace, more) != seed, "an extra ballot changes the seed");

    std::vector<Ballot> otherIdentity = KatBallots();
    otherIdentity[2].custom.id[0] ^= 1;
    Check(DrawSeed(salt, kRoom, kRace, otherIdentity) != seed, "a different custom track identity changes the seed");

    std::vector<Ballot> otherTitle = KatBallots();
    otherTitle[2].custom.title = "Bo";
    Check(DrawSeed(salt, kRoom, kRace, otherTitle) == seed, "a custom track's title is not part of the seed");

    std::vector<Ballot> strayBytes = KatBallots();
    strayBytes[0].custom = CustomBallot(0, 0x90, "x").custom;
    Check(DrawSeed(salt, kRoom, kRace, strayBytes) == seed, "a custom id on a stock ballot is not mixed in");

    const Seed empty = DrawSeed(salt, kRoom, kRace, {});
    Check(empty == DrawSeed(salt, kRoom, kRace, {}), "an empty ballot list gives the same seed each time");
    Check(empty != seed, "an empty ballot list gives a seed of its own");
}

void CheckFuzzDecoders() {
    size_t wrongAccept = 0;
    size_t wrongRefuse = 0;
    size_t touched = 0;
    for (size_t round = 0; round < 3000; round++) {
        const size_t length = static_cast<size_t>(NextRandom() % 81);
        std::vector<uint8_t> bytes(length);
        for (uint8_t& b : bytes) {
            b = NextByte();
        }

        Commitment commitment{};
        commitment.fill(0xAA);
        const bool okCommitment = DecodeCommitment(bytes.data(), bytes.size(), commitment);
        Salt salt{};
        salt.fill(0xAA);
        const bool okReveal = DecodeReveal(bytes.data(), bytes.size(), salt);

        if (length != 32) {
            if (okCommitment || okReveal) {
                wrongAccept++;
            }
            if (commitment[0] != 0xAA || salt[0] != 0xAA) {
                touched++;
            }
        } else {
            if (!okCommitment || !okReveal) {
                wrongRefuse++;
            } else if (std::equal(bytes.begin(), bytes.end(), commitment.begin()) == false ||
                       std::equal(bytes.begin(), bytes.end(), salt.begin()) == false) {
                wrongRefuse++;
            }
        }
    }
    Check(wrongAccept == 0, "fuzz: no payload of the wrong length is accepted (" + std::to_string(wrongAccept) + ")");
    Check(wrongRefuse == 0, "fuzz: every 32-byte payload is accepted and copied (" + std::to_string(wrongRefuse) + " wrong)");
    Check(touched == 0, "fuzz: a refused payload leaves the output alone (" + std::to_string(touched) + " touched)");

    // Every length from 0 to 40, one by one: only 32 is accepted, and the trailing and missing bytes around it are refused.
    size_t lengthErrors = 0;
    for (size_t n = 0; n <= 40; n++) {
        const std::vector<uint8_t> bytes(n, 0x5A);
        Commitment commitment{};
        Salt salt{};
        const bool okCommitment = DecodeCommitment(bytes.data(), bytes.size(), commitment);
        const bool okReveal = DecodeReveal(bytes.data(), bytes.size(), salt);
        if (okCommitment != (n == 32) || okReveal != (n == 32)) {
            lengthErrors++;
        }
    }
    Check(lengthErrors == 0, "every length from 0 to 40 is accepted only at 32 bytes");

    // Random commitments never verify: a verifier that accepted one would let a host change its draw.
    size_t accepted = 0;
    const Salt salt = KatSalt();
    for (size_t round = 0; round < 300; round++) {
        Commitment guess{};
        for (uint8_t& b : guess) {
            b = NextByte();
        }
        if (Verify(guess, kRoom, kRace, salt)) {
            accepted++;
        }
    }
    Check(accepted == 0, "fuzz: no random commitment verifies against the reveal");
}

}  // namespace

int main() {
    CheckSha256Header();
    CheckCommitment();
    CheckDrawSalt();
    CheckPayloads();
    CheckSeed();
    CheckFuzzDecoders();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
