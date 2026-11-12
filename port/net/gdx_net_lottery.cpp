// port/net/gdx_net_lottery.cpp: the lottery commitment, its reveal and the draw seed of an Open room (G-NET O6.4).
// Copyright (c) Zorkats

#include "gdx_net_lottery.h"

#include "../gdx_sha256.h"

#include <algorithm>
#include <cstring>

namespace gdx::net::lottery {
namespace {

// Domain strings. The text is hashed without its terminator, and the two differ, so a commitment can never be read
// as a seed or the other way round.
template <size_t N>
void FeedText(gdx::hash::Sha256* state, const char (&text)[N]) {
    gdx::hash::sha256_update(state, text, N - 1);
}

void FeedU32(gdx::hash::Sha256* state, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    gdx::hash::sha256_update(state, bytes, sizeof(bytes));
}

bool AllZero(const uint8_t* data, size_t length) {
    uint8_t any = 0;
    for (size_t i = 0; i < length; i++) {
        any |= data[i];
    }
    return any == 0;
}

}  // namespace

bool DrawSalt(const uint8_t* random, size_t length, Salt& out) {
    if (random == nullptr || length < kSaltBytes || AllZero(random, kSaltBytes)) {
        return false;
    }
    std::memcpy(out.data(), random, kSaltBytes);
    return true;
}

Commitment Commit(uint32_t roomId, uint32_t raceIndex, const Salt& salt) {
    gdx::hash::Sha256 state;
    gdx::hash::sha256_init(&state);
    FeedText(&state, "G-NET lottery v1");
    FeedU32(&state, roomId);
    FeedU32(&state, raceIndex);
    gdx::hash::sha256_update(&state, salt.data(), salt.size());
    Commitment out{};
    gdx::hash::sha256_final(&state, out.data());
    return out;
}

Salt RaceSalt(const std::array<uint8_t, 32>& key, uint32_t roomId, uint32_t raceIndex) {
    gdx::hash::Sha256 state;
    gdx::hash::sha256_init(&state);
    FeedText(&state, "G-NET lottery salt v1");
    gdx::hash::sha256_update(&state, key.data(), key.size());
    FeedU32(&state, roomId);
    FeedU32(&state, raceIndex);
    Salt out{};
    gdx::hash::sha256_final(&state, out.data());
    return out;
}

bool Verify(const Commitment& commitment, uint32_t roomId, uint32_t raceIndex, const Salt& salt) {
    const Commitment expect = Commit(roomId, raceIndex, salt);
    // No early exit: the answer does not reveal how many leading bytes matched.
    uint8_t difference = 0;
    for (size_t i = 0; i < kCommitmentBytes; i++) {
        difference |= static_cast<uint8_t>(expect[i] ^ commitment[i]);
    }
    return difference == 0;
}

Seed DrawSeed(const Salt& salt, uint32_t roomId, uint32_t raceIndex, const std::vector<gdx::net::lobby::Ballot>& ballots) {
    // A copy, so the caller's list keeps its order. Ascending id makes the seed independent of arrival order.
    std::vector<gdx::net::lobby::Ballot> sorted = ballots;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const gdx::net::lobby::Ballot& a, const gdx::net::lobby::Ballot& b) { return a.id < b.id; });

    gdx::hash::Sha256 state;
    gdx::hash::sha256_init(&state);
    FeedText(&state, "G-NET lottery seed v1");
    FeedU32(&state, roomId);
    FeedU32(&state, raceIndex);
    gdx::hash::sha256_update(&state, salt.data(), salt.size());
    FeedU32(&state, static_cast<uint32_t>(sorted.size()));
    for (const gdx::net::lobby::Ballot& ballot : sorted) {
        const uint8_t head[2] = {ballot.id, ballot.track};
        gdx::hash::sha256_update(&state, head, sizeof(head));
        // Only a custom track has an identity to mix in. A stock index or a random draw ends at its two bytes.
        if (ballot.track == gdx::net::lobby::kTrackCustom) {
            gdx::hash::sha256_update(&state, ballot.custom.id.data(), ballot.custom.id.size());
        }
    }
    Seed out{};
    gdx::hash::sha256_final(&state, out.data());
    return out;
}

uint32_t SeedHash(const Seed& seed) {
    return static_cast<uint32_t>(seed[0]) | (static_cast<uint32_t>(seed[1]) << 8) |
           (static_cast<uint32_t>(seed[2]) << 16) | (static_cast<uint32_t>(seed[3]) << 24);
}

std::vector<uint8_t> EncodeCommitment(const Commitment& commitment) {
    return std::vector<uint8_t>(commitment.begin(), commitment.end());
}

bool DecodeCommitment(const uint8_t* data, size_t len, Commitment& out) {
    if (data == nullptr || len != kCommitmentBytes) {
        return false;
    }
    std::memcpy(out.data(), data, kCommitmentBytes);
    return true;
}

std::vector<uint8_t> EncodeReveal(const Salt& salt) {
    return std::vector<uint8_t>(salt.begin(), salt.end());
}

bool DecodeReveal(const uint8_t* data, size_t len, Salt& out) {
    if (data == nullptr || len != kSaltBytes) {
        return false;
    }
    std::memcpy(out.data(), data, kSaltBytes);
    return true;
}

}  // namespace gdx::net::lottery
