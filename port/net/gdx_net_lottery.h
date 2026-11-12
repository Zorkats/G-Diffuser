// port/net/gdx_net_lottery.h: the lottery commitment, its reveal and the draw seed of an Open room (G-NET O6.4).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no thread, no game header. The caller supplies every random byte, so this module never
// reads a generator itself.
//
// Decision 39. The host of an Open room draws a secret salt, publishes the commitment with its vote, and reveals the
// salt with the draw. Anyone can check the reveal against the commitment. The draw seed mixes the salt in, so a voter
// who votes last cannot compute the draw before the reveal. RoomHash cannot give that: it is a function of public
// values only (gdx_net_room.h, lines 25 to 27).
//
// Layout. Integers are little-endian. SHA-256 is gdx::hash from port/gdx_sha256.h.
//
//   commitment  SHA-256( "G-NET lottery v1" || u32 roomId || u32 raceIndex || salt[32] )
//   seed        SHA-256( "G-NET lottery seed v1" || u32 roomId || u32 raceIndex || salt[32] || u32 ballot count
//                        || each ballot in ascending player id: u8 id, u8 track, and, when track is kTrackCustom,
//                           the 32-byte custom track id )
//   draw hash   the first four bytes of seed as a u32. The draw takes this word where RoomHash is used now.
//   race salt   SHA-256( "G-NET lottery salt v1" || key[32] || u32 roomId || u32 raceIndex ): the host draws one
//               secret key per room and derives each race's salt from it, so the room machine needs no
//               generator. Revealing one race's salt reveals neither the key nor another race's salt.
//
// A custom track's title is left out of the seed: it is a display name, not part of the track's identity. The ballot
// count keeps the layout unambiguous when two ballot lists differ in length.
//
// Payloads. The commitment travels as its 32 bytes, and the reveal as its 32-byte salt. Each decoder refuses any
// other length.
//
// Not covered here: the host learns the draw before it reveals, so it could withhold the salt when it dislikes the
// result. The room must treat a draw with no reveal as an abandoned draw, never as a fair one.

#pragma once

#include "gdx_net_lobby.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gdx::net::lottery {

constexpr size_t kSaltBytes = 32;
constexpr size_t kCommitmentBytes = 32;
constexpr size_t kSeedBytes = 32;

using Salt = std::array<uint8_t, kSaltBytes>;
using Commitment = std::array<uint8_t, kCommitmentBytes>;
using Seed = std::array<uint8_t, kSeedBytes>;

// The salt is the first kSaltBytes of a buffer the caller filled from a CSPRNG. False when the buffer is null, shorter
// than kSaltBytes, or all zeros, which is what a generator that never ran returns. out is left alone on failure.
bool DrawSalt(const uint8_t* random, size_t length, Salt& out);

Commitment Commit(uint32_t roomId, uint32_t raceIndex, const Salt& salt);

// The salt of one race of a room, from the room's secret key (see "race salt" above).
Salt RaceSalt(const std::array<uint8_t, 32>& key, uint32_t roomId, uint32_t raceIndex);

// True when commitment is the commitment of this salt, room and race. Every byte is compared before the answer comes.
bool Verify(const Commitment& commitment, uint32_t roomId, uint32_t raceIndex, const Salt& salt);

// The draw seed. The ballots may come in any order, since they are sorted by player id first. The room gives one
// ballot per racer; this layout does not check that.
Seed DrawSeed(const Salt& salt, uint32_t roomId, uint32_t raceIndex, const std::vector<gdx::net::lobby::Ballot>& ballots);

// The first four bytes of the seed as a little-endian u32.
uint32_t SeedHash(const Seed& seed);

// Encoders give exactly kCommitmentBytes or kSaltBytes bytes. Decoders take exactly that many, and leave out alone on
// failure.
std::vector<uint8_t> EncodeCommitment(const Commitment& commitment);
bool DecodeCommitment(const uint8_t* data, size_t len, Commitment& out);
std::vector<uint8_t> EncodeReveal(const Salt& salt);
bool DecodeReveal(const uint8_t* data, size_t len, Salt& out);

}  // namespace gdx::net::lottery
