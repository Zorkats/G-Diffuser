// port/tests/gdx_net_hash_tests.cpp: SHA-1, MD5 and HMAC-SHA1 for the TURN credential (G-NET decision 72).
// Copyright (c) Zorkats
//
// The digests are the FIPS 180-4 and RFC 1321 examples, RFC 2202's HMAC-SHA1 cases, and lengths either side of
// the padding boundary (55, 56, 63, 64 and 65 bytes), each checked against Python's hashlib. The last group signs
// the RFC 5769 sample messages by hand, so the MESSAGE-INTEGRITY values printed in that RFC are reproduced from
// these functions alone, short-term and long-term key both.

#include "gdx_net_hash.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
namespace hash = gdx::net::hash;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Two hex digits per byte; every other character is ignored.
Bytes Hex(const char* text) {
    Bytes out;
    int high = -1;
    for (const char* p = text; *p != '\0'; ++p) {
        const int v = HexValue(*p);
        if (v < 0) continue;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<uint8_t>(high * 16 + v));
            high = -1;
        }
    }
    return out;
}

template <size_t N> bool Equals(const std::array<uint8_t, N>& digest, const char* hex) {
    const Bytes want = Hex(hex);
    return want.size() == N && std::memcmp(digest.data(), want.data(), N) == 0;
}

Bytes Text(const char* text) {
    return Bytes(text, text + std::strlen(text));
}

void CheckSha1() {
    std::printf("SHA-1: FIPS 180-4 examples and the padding boundary\n");
    struct Case {
        Bytes message;
        const char* digest;
    };
    const Case cases[] = {
        {Text(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {Text("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {Bytes(55, 'a'), "c1c8bbdc22796e28c0e15163d20899b65621d65a"},
        {Bytes(56, 'a'), "c2db330f6083854c99d4b5bfb6e8f29f201be699"},
        {Bytes(63, 'a'), "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
        {Bytes(64, 'a'), "0098ba824b5c16427bd7a1122a5a442a25ec644d"},
        {Bytes(65, 'a'), "11655326c708d70319be2610e8a57d9a5b959d3b"},
        {Bytes(1000000, 'a'), "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
    };
    for (const Case& c : cases) {
        Check(Equals(hash::Sha1(c.message.data(), c.message.size()), c.digest),
              "SHA-1 of " + std::to_string(c.message.size()) + " bytes");
    }
}

void CheckMd5() {
    std::printf("MD5: the RFC 1321 test suite and the padding boundary\n");
    struct Case {
        Bytes message;
        const char* digest;
    };
    const Case cases[] = {
        {Text(""), "d41d8cd98f00b204e9800998ecf8427e"},
        {Text("a"), "0cc175b9c0f1b6a831c399e269772661"},
        {Text("abc"), "900150983cd24fb0d6963f7d28e17f72"},
        {Text("message digest"), "f96b697d7cb7938d525a2f31aaf161d0"},
        {Text("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b"},
        {Text("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"), "d174ab98d277d9f5a5611c2c9f419d9f"},
        {Text("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
         "57edf4a22be3c955ac49da2e2107b67a"},
        {Bytes(55, 'a'), "ef1772b6dff9a122358552954ad0df65"},
        {Bytes(56, 'a'), "3b0c8ac703f828b04c6c197006d17218"},
        {Bytes(63, 'a'), "b06521f39153d618550606be297466d5"},
        {Bytes(64, 'a'), "014842d480b571495a4a0363793f7367"},
        {Bytes(65, 'a'), "c743a45e0d2e6a95cb859adae0248435"},
        {Bytes(1000000, 'a'), "7707d6ae4e027c70eea2a935c2296f21"},
    };
    for (const Case& c : cases) {
        Check(Equals(hash::Md5(c.message.data(), c.message.size()), c.digest),
              "MD5 of " + std::to_string(c.message.size()) + " bytes");
    }
}

void CheckHmac() {
    std::printf("HMAC-SHA1: RFC 2202 cases 1 to 7\n");
    struct Case {
        Bytes key;
        Bytes data;
        const char* mac;
    };
    const Case cases[] = {
        {Bytes(20, 0x0b), Text("Hi There"), "b617318655057264e28bc0b6fb378c8ef146be00"},
        {Text("Jefe"), Text("what do ya want for nothing?"), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
        {Bytes(20, 0xaa), Bytes(50, 0xdd), "125d7342b9ac11cd91a39af48aa17b4f63f175d3"},
        {Hex("0102030405060708090a0b0c0d0e0f10111213141516171819"), Bytes(50, 0xcd),
         "4c9007f4026250c6bc8414f9bf50c86c2d7235da"},
        {Bytes(20, 0x0c), Text("Test With Truncation"), "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04"},
        {Bytes(80, 0xaa), Text("Test Using Larger Than Block-Size Key - Hash Key First"),
         "aa4ae5e15272d00e95705637ce8a3b55ed402112"},
        {Bytes(80, 0xaa), Text("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"),
         "e8e99d0f45237d786d6bbaa7965c7808bbff1a91"},
    };
    int n = 1;
    for (const Case& c : cases) {
        Check(Equals(hash::HmacSha1(c.key.data(), c.key.size(), c.data.data(), c.data.size()), c.mac),
              "RFC 2202 case " + std::to_string(n++));
    }
}

// The RFC 5769 messages up to their MESSAGE-INTEGRITY attribute. RFC 8489 section 14.5: the HMAC covers the
// message up to that attribute, with the header length set as if the message ended right after it.
void CheckRfc5769() {
    std::printf("RFC 5769: the samples' MESSAGE-INTEGRITY from these hashes alone\n");
    struct Case {
        const char* name;
        Bytes signedPart; // header (length already adjusted) and the attributes before MESSAGE-INTEGRITY
        Bytes key;
        const char* mac;
    };
    const Bytes shortTerm = Text("VOkJxbRl1RmTxUk/WvJxBt");
    // Section 2.4: key = MD5(username ":" realm ":" password), the password after SASLprep. The username is
    // six katakana, written here as their UTF-8 bytes.
    Bytes longTermInput = Hex("e3839ee38388e383aae38383e382afe382b9");
    const Bytes rest = Text(":example.org:TheMatrIX");
    longTermInput.insert(longTermInput.end(), rest.begin(), rest.end());
    const hash::Md5Digest longTermKey = hash::Md5(longTermInput.data(), longTermInput.size());
    Check(Equals(longTermKey, "e8ca7ad59d5eb0518e312911d2dab2a9"), "RFC 5769 2.4 long-term key");
    const Case cases[] = {
        {"2.1 request",
         Hex("00010050 2112a442 b7e7a701 bc34d686 fa87dfae 80220010 5354554e 20746573 7420636c 69656e74 "
             "00240004 6e0001ff 80290008 932ff9b1 51263b36 00060009 6576746a 3a683676 59202020"),
         shortTerm, "9aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"},
        {"2.2 IPv4 response",
         Hex("01010034 2112a442 b7e7a701 bc34d686 fa87dfae 8022000b 74657374 20766563 746f7220 00200008 "
             "0001a147 e112a643"),
         shortTerm, "2b91f599fd9e90c38c7489f92af9ba53f06be7d7"},
        {"2.3 IPv6 response",
         Hex("01010040 2112a442 b7e7a701 bc34d686 fa87dfae 8022000b 74657374 20766563 746f7220 00200014 "
             "0002a147 0113a9fa a5d3f179 bc25f4b5 bed2b9d9"),
         shortTerm, "a382954e4be67bf11784c97c8292c275bfe3ed41"},
        {"2.4 long-term request",
         Hex("00010060 2112a442 78ad3433 c6ad72c0 29da412e 00060012 e3839ee3 8388e383 aae38383 e382afe3 "
             "82b90000 0015001c 662f2f34 39396b39 35346436 4f4c3334 6f4c3946 53547679 36347341 0014000b "
             "6578616d 706c652e 6f726700"),
         Bytes(longTermKey.begin(), longTermKey.end()), "f67024656dd64a3e02b8e0712e85c9a28ca89666"},
    };
    for (const Case& c : cases) {
        Check(Equals(hash::HmacSha1(c.key.data(), c.key.size(), c.signedPart.data(), c.signedPart.size()), c.mac),
              std::string("RFC 5769 ") + c.name);
    }
}

} // namespace

int main() {
    CheckSha1();
    CheckMd5();
    CheckHmac();
    CheckRfc5769();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
