// port/net/gdx_net_hash.h: SHA-1, MD5 and HMAC-SHA1 for the TURN long-term credential (G-NET decision 72).
// Copyright (c) Zorkats
//
// TURN (RFC 8656) signs every authenticated request with MESSAGE-INTEGRITY, an HMAC-SHA1 under the key
// MD5(username ":" realm ":" password) (RFC 8489 sections 9.2.2 and 14.5). Those two hashes are needed for that
// and nothing else: both are broken for collision resistance, and no other G-NET code may use them. Monocypher,
// which the rest of G-NET uses, has neither.
//
// Written from FIPS 180-4 section 6.1 and RFC 1321, and checked against their test vectors and RFC 2202's.
// One-shot functions: the messages are a few hundred bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gdx::net::hash {

constexpr size_t kSha1Bytes = 20;
constexpr size_t kMd5Bytes = 16;
using Sha1Digest = std::array<uint8_t, kSha1Bytes>;
using Md5Digest = std::array<uint8_t, kMd5Bytes>;

Sha1Digest Sha1(const uint8_t* data, size_t length);
Md5Digest Md5(const uint8_t* data, size_t length);
// RFC 2104 with SHA-1: a key longer than the 64-byte block is hashed first.
Sha1Digest HmacSha1(const uint8_t* key, size_t keyLength, const uint8_t* data, size_t length);

} // namespace gdx::net::hash
