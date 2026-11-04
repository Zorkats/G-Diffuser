#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gdx {
namespace hash {

struct Sha256 {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t blockLen;
    bool overflow;
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

inline uint32_t load_be(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

inline void store_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

inline void sha256_block(Sha256* s, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be(block + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0];
    uint32_t b = s->h[1];
    uint32_t c = s->h[2];
    uint32_t d = s->h[3];
    uint32_t e = s->h[4];
    uint32_t f = s->h[5];
    uint32_t g = s->h[6];
    uint32_t hh = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += hh;
}

inline void sha256_init(Sha256* s) {
    s->h[0] = 0x6a09e667u;
    s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu;
    s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu;
    s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->blockLen = 0;
    s->overflow = false;
}

inline void sha256_update(Sha256* s, const void* data, size_t len) {
    if (s->overflow || data == nullptr || len == 0) {
        if (data == nullptr && len > 0) {
            s->overflow = true;
        }
        return;
    }
    if (len > (UINT64_MAX / 8) || s->bits > UINT64_MAX - (uint64_t)len * 8) {
        s->overflow = true;
        return;
    }
    s->bits += (uint64_t)len * 8;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        size_t space = 64 - s->blockLen;
        size_t take = len < space ? len : space;
        std::memcpy(s->block + s->blockLen, p, take);
        s->blockLen += take;
        p += take;
        len -= take;
        if (s->blockLen == 64) {
            sha256_block(s, s->block);
            s->blockLen = 0;
        }
    }
}

inline void sha256_final(Sha256* s, unsigned char out[32]) {
    uint8_t block[64];
    std::memcpy(block, s->block, s->blockLen);
    block[s->blockLen] = 0x80;
    if (s->blockLen >= 56) {
        std::memset(block + s->blockLen + 1, 0, 64 - (s->blockLen + 1));
        sha256_block(s, block);
        std::memset(block, 0, 64);
    } else {
        std::memset(block + s->blockLen + 1, 0, 56 - (s->blockLen + 1));
    }
    for (int i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)(s->bits >> (i * 8));
    }
    sha256_block(s, block);
    for (int i = 0; i < 8; i++) {
        store_be(out + i * 4, s->h[i]);
    }
}

}
}
