// port/net/gdx_net_hash.cpp: SHA-1 (FIPS 180-4), MD5 (RFC 1321) and HMAC-SHA1 (RFC 2104).
// Copyright (c) Zorkats

#include "gdx_net_hash.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gdx::net::hash {
namespace {

constexpr size_t kBlockBytes = 64; // both hashes work on 512-bit blocks

uint32_t RotateLeft(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

// The message, a 0x80 byte, zeros up to 56 bytes into a block, then the length in bits as 8 bytes, big-endian for
// SHA-1 and little-endian for MD5 (FIPS 180-4 section 5.1.1, RFC 1321 section 3.2).
std::vector<uint8_t> Padded(const uint8_t* data, size_t length, bool bigEndian) {
    std::vector<uint8_t> out(data, data + length);
    out.push_back(0x80);
    while (out.size() % kBlockBytes != kBlockBytes - 8) {
        out.push_back(0);
    }
    const uint64_t bits = static_cast<uint64_t>(length) * 8u;
    for (unsigned i = 0; i < 8; i++) {
        const unsigned shift = bigEndian ? 8u * (7u - i) : 8u * i;
        out.push_back(static_cast<uint8_t>((bits >> shift) & 0xFFu));
    }
    return out;
}

// MD5's sine table: K[i] is the integer part of |sin(i + 1)| times 2^32 (RFC 1321 section 3.4). A double holds
// each product exactly enough for the floor; the RFC 1321 vectors in the tests check every entry.
std::array<uint32_t, 64> SineTable() {
    std::array<uint32_t, 64> table{};
    for (size_t i = 0; i < table.size(); i++) {
        table[i] = static_cast<uint32_t>(std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
    }
    return table;
}

} // namespace

Sha1Digest Sha1(const uint8_t* data, size_t length) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const std::vector<uint8_t> message = Padded(data, length, true);
    for (size_t block = 0; block < message.size(); block += kBlockBytes) {
        uint32_t w[80];
        for (size_t t = 0; t < 16; t++) {
            const uint8_t* p = message.data() + block + 4 * t;
            w[t] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        }
        for (size_t t = 16; t < 80; t++) {
            w[t] = RotateLeft(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (size_t t = 0; t < 80; t++) {
            uint32_t f;
            uint32_t k;
            if (t < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (t < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (t < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = RotateLeft(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    Sha1Digest out{};
    for (size_t i = 0; i < 5; i++) {
        out[4 * i] = static_cast<uint8_t>(h[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
        out[4 * i + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
        out[4 * i + 3] = static_cast<uint8_t>(h[i] & 0xFFu);
    }
    return out;
}

Md5Digest Md5(const uint8_t* data, size_t length) {
    static const std::array<uint32_t, 64> kSine = SineTable();
    static constexpr unsigned kShift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
        6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t h[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    const std::vector<uint8_t> message = Padded(data, length, false);
    for (size_t block = 0; block < message.size(); block += kBlockBytes) {
        uint32_t m[16];
        for (size_t i = 0; i < 16; i++) {
            const uint8_t* p = message.data() + block + 4 * i;
            m[i] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (size_t i = 0; i < 64; i++) {
            uint32_t f;
            size_t g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const uint32_t sum = a + f + kSine[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + RotateLeft(sum, kShift[i]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    Md5Digest out{};
    for (size_t i = 0; i < 4; i++) {
        out[4 * i] = static_cast<uint8_t>(h[i] & 0xFFu);
        out[4 * i + 1] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
        out[4 * i + 2] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
        out[4 * i + 3] = static_cast<uint8_t>(h[i] >> 24);
    }
    return out;
}

Sha1Digest HmacSha1(const uint8_t* key, size_t keyLength, const uint8_t* data, size_t length) {
    std::array<uint8_t, kBlockBytes> block{};
    if (keyLength > kBlockBytes) {
        const Sha1Digest hashed = Sha1(key, keyLength);
        std::copy(hashed.begin(), hashed.end(), block.begin());
    } else if (keyLength > 0) {
        std::copy(key, key + keyLength, block.begin());
    }
    std::vector<uint8_t> inner(kBlockBytes + length);
    for (size_t i = 0; i < kBlockBytes; i++) {
        inner[i] = static_cast<uint8_t>(block[i] ^ 0x36u);
    }
    if (length > 0) {
        std::copy(data, data + length, inner.begin() + kBlockBytes);
    }
    const Sha1Digest innerHash = Sha1(inner.data(), inner.size());
    std::array<uint8_t, kBlockBytes + kSha1Bytes> outer{};
    for (size_t i = 0; i < kBlockBytes; i++) {
        outer[i] = static_cast<uint8_t>(block[i] ^ 0x5Cu);
    }
    std::copy(innerHash.begin(), innerHash.end(), outer.begin() + kBlockBytes);
    return Sha1(outer.data(), outer.size());
}

} // namespace gdx::net::hash
