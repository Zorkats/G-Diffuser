// port/net/gdx_net_track_pack.h -- a custom track's identity and the check a receiver runs on its bytes (G-NET O4.1).
// Copyright (c) Zorkats
//
// A custom track travels as its content bytes (gdx::snapshot::content): the track as its author made it, blind
// to the slot, the name, ghosts and records. Its identity is the SHA-256 of those bytes (decision 4). A
// receiver hashes the whole buffer and compares it with the identity it was promised before a single byte
// reaches the decoder (decision 5, ODD 5 and 8.1 step 4).
#pragma once

#include "gdx_course_snapshot.h"

#include <array>
#include <cstddef>
#include <string>

namespace gdx::net::track {

constexpr size_t kIdBytes = 32;
using TrackId = std::array<unsigned char, kIdBytes>;

// The largest buffer a track may be: the snapshot format's own cap.
constexpr size_t kMaxBytes = gdx::snapshot::MaxBytes;

// The identity of a buffer of content bytes. Any length; Verify decides whether the buffer is a track.
TrackId Identify(const unsigned char* bytes, size_t length);

enum class Verdict {
    Ok,
    Empty,         // no bytes
    Oversized,     // more than kMaxBytes; never hashed
    WrongIdentity, // the hash is not the promised identity; never decoded
    Undecodable,   // the hash matches but the bytes are not a snapshot generation
    NotCanonical,  // a generation, but not the content bytes of one (another slot, a seal, ghosts, padding)
};

const char* VerdictName(Verdict verdict);

// Checks, in this order: the size, the hash against expected, the decoder, and that the decoded track
// encodes back to exactly these bytes. On Ok, out (when not null) holds the decoded track, bound to the
// content identity CONTENT.CRSD on disk 1, directory 0. On any other verdict out is left untouched.
Verdict Verify(const unsigned char* bytes, size_t length, const TrackId& expected, gdx::snapshot::Generation* out);

// Lower-case hex, 64 characters: the cache's file names. FromHex takes upper or lower case and nothing else.
std::string ToHex(const TrackId& id);
bool FromHex(const std::string& text, TrackId& out);

} // namespace gdx::net::track
