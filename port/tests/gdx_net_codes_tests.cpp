// port/tests/gdx_net_codes_tests.cpp -- Crockford base32, room codes and friend codes (G-NET O5.2 and O7.2).
// Copyright (c) Zorkats
//
// Usage: gdx_net_codes_tests [fuzz rounds, default 3000]
//
// Checks the alphabet and Normalise, base32 round trips and refusals, a room code for fixed random bytes and its
// display, lower-case and confusable forms, and friend code round trips. For every test key, every single-character
// substitution and every adjacent transposition of distinct characters is refused, checked exhaustively. A seeded
// fuzz checks that any friend code the parser accepts is the code of the key it returns.

#include "gdx_net_codes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace codes = gdx::net::codes;

using Key = std::vector<unsigned char>;

// The test keeps its own copy of the alphabet, so a typo in the module's table cannot pass unnoticed.
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

uint64_t gRng = 0x9E3779B97F4A7C15ULL;

uint32_t Next() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return uint32_t(gRng >> 16);
}

int SymbolOf(char c) {
    for (int i = 0; i < 32; i++) {
        if (kAlphabet[i] == c) {
            return i;
        }
    }
    return -1;
}

std::vector<Key> TestKeys() {
    std::vector<Key> keys;
    keys.push_back(Key(32, static_cast<unsigned char>(0x00)));
    keys.push_back(Key(32, static_cast<unsigned char>(0xFF)));
    keys.push_back(Key(32, static_cast<unsigned char>(0x01)));
    keys.push_back(Key(32, static_cast<unsigned char>(0x80)));
    Key ramp(32);
    for (size_t i = 0; i < 32; i++) {
        ramp[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    keys.push_back(ramp);
    for (int k = 0; k < 7; k++) {
        Key random(32);
        for (auto& b : random) {
            b = static_cast<unsigned char>(Next());
        }
        keys.push_back(random);
    }
    return keys;
}

void CheckAlphabetAndNormalise() {
    for (unsigned v = 0; v < 32; v++) {
        const unsigned char byte = static_cast<unsigned char>(v << 3);
        const std::string one = codes::Base32Encode(&byte, 5);
        Check(one == std::string(1, kAlphabet[v]), "value " + std::to_string(v) + " encodes to its alphabet character");
        std::string normal;
        Check(codes::Normalise(one, normal) && normal == one, "alphabet character " + one + " normalises to itself");
    }

    std::string out;
    Check(codes::Normalise("abc-def gh i", out) && out == "ABCDEFGH1",
          "case, hyphens and spaces are dropped, and the letter I reads as 1");
    Check(codes::Normalise("I l O o", out) && out == "1100", "I, l, O and o map to 1, 1, 0 and 0");
    Check(codes::Normalise("", out) && out.empty(), "empty text normalises to empty");
    Check(codes::Normalise(" - - ", out) && out.empty(), "text of separators only normalises to empty");

    const char* refused[] = { "U", "u", "@", "A_B", "A.B", "A\tB", "A/B", "\xC3\x96", "A\x7F" };
    for (const char* text : refused) {
        std::string keep = "KEEP";
        Check(!codes::Normalise(text, keep) && keep == "KEEP",
              std::string("refused and left the output alone: ") + text);
    }
}

void CheckBase32RoundTrip(int rounds) {
    for (int round = 0; round < rounds; round++) {
        unsigned char source[8];
        for (auto& b : source) {
            b = static_cast<unsigned char>(Next());
        }
        const size_t bits = Next() % 65;
        const std::string text = codes::Base32Encode(source, bits);
        Check(text.size() == (bits + 4) / 5, "base32 length is ceil(bits / 5)");

        const size_t byteCount = (bits + 7) / 8;
        std::vector<unsigned char> back(byteCount + 1, 0xAA);
        Check(codes::Base32Decode(text, bits, back.data()), "encoded text decodes");

        std::vector<unsigned char> expected(source, source + byteCount);
        if ((bits % 8) != 0) {
            const size_t keepBits = bits % 8;
            expected.back() = static_cast<unsigned char>(expected.back() & ((0xFFu << (8 - keepBits)) & 0xFFu));
        }
        Check(std::equal(expected.begin(), expected.end(), back.begin()) && back[byteCount] == 0xAA,
              "decode gives back the bits, zeroes the unused bits and writes no further byte");
    }
}

void CheckBase32Refusals() {
    unsigned char out[2] = { 0, 0 };
    Check(codes::Base32Decode("M", 3, out) && out[0] == 0xA0, "M is the bits 101 with zero padding");
    Check(!codes::Base32Decode("Q", 3, out), "nonzero padding is refused (Q is 10111)");
    Check(!codes::Base32Decode("P", 3, out), "nonzero padding is refused (P is 10110)");
    Check(!codes::Base32Decode("MM", 3, out), "a text of the wrong length is refused");
    Check(!codes::Base32Decode("", 3, out), "an empty text is refused for 3 bits");
    Check(codes::Base32Decode("", 0, out), "zero bits decode from an empty text");
    Check(!codes::Base32Decode("m", 3, out), "lower case is refused, because the text must be normalised");
    Check(!codes::Base32Decode("U", 3, out), "U is refused");
    Check(!codes::Base32Decode("M", 3, nullptr), "a null output is refused when bytes are due");
}

void CheckRoomCodes(int rounds) {
    const unsigned char fixed[4] = { 0x12, 0x34, 0x56, 0x78 };
    const uint32_t value = codes::RoomCodeFromRandom(fixed);
    Check(value == 0x12345678u, "a room code takes the low 30 bits of the random word");
    Check(codes::FormatRoomCode(value) == "938-NKR", "bytes 12 34 56 78 give room code 938-NKR");

    const unsigned char topBits[4] = { 0xC0, 0x00, 0x00, 0x00 };
    Check(codes::RoomCodeFromRandom(topBits) == 0, "the top two bits of the random word are dropped");
    const unsigned char ones[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    Check(codes::RoomCodeFromRandom(ones) == 0x3FFFFFFFu, "all ones give the largest room code");
    Check(codes::FormatRoomCode(0x3FFFFFFFu) == "ZZZ-ZZZ", "the largest room code shows as ZZZ-ZZZ");
    const unsigned char one[4] = { 0x00, 0x00, 0x00, 0x01 };
    Check(codes::FormatRoomCode(codes::RoomCodeFromRandom(one)) == "000-001", "room code 1 shows as 000-001");
    Check(codes::FormatRoomCode(codes::kRoomCodeLimit).empty(), "2^30 is not a room code");

    uint32_t parsed = 0;
    const char* accepted[] = { "938-NKR", "938NKR", "938nkr", "938 NKR", " 938-nkr ", "9-3-8-N-K-R" };
    for (const char* text : accepted) {
        parsed = 0xDEADu;
        Check(codes::ParseRoomCode(text, parsed) && parsed == value, std::string("room code accepted: ") + text);
    }
    Check(codes::ParseRoomCode("OOO-OOL", parsed) && parsed == 1, "O and L read as 0 and 1");
    Check(codes::ParseRoomCode("ooo-ool", parsed) && parsed == 1, "lower-case O and L read as 0 and 1");
    Check(codes::ParseRoomCode("000-00I", parsed) && parsed == 1, "I reads as 1");
    Check(codes::ParseRoomCode("zzz-zzz", parsed) && parsed == 0x3FFFFFFFu, "lower-case ZZZ-ZZZ parses");

    parsed = 777;
    const char* refusedRooms[] = { "", "938-NK", "938-NKRR", "938-NKU", "938-NK!", "938-NK\xC3\xA9", "UUU-UUU" };
    for (const char* text : refusedRooms) {
        Check(!codes::ParseRoomCode(text, parsed) && parsed == 777,
              std::string("room code refused and left alone: ") + text);
    }

    for (int round = 0; round < rounds; round++) {
        const uint32_t v = Next() & (codes::kRoomCodeLimit - 1u);
        uint32_t back = 0;
        const std::string shown = codes::FormatRoomCode(v);
        Check(shown.size() == 7 && codes::ParseRoomCode(shown, back) && back == v, "room code round trip");

        std::string raw;
        for (size_t i = 0; i < codes::kRoomCodeChars; i++) {
            raw.push_back(kAlphabet[Next() % 32]);
        }
        Check(codes::ParseRoomCode(raw, back) && codes::FormatRoomCode(back) == raw.substr(0, 3) + "-" + raw.substr(3),
              "a random six-character room code parses and shows back the same characters");
    }
}

void CheckFriendCodeRoundTrip(const std::vector<Key>& keys) {
    for (const Key& key : keys) {
        const std::string shown = codes::FormatFriendCode(key.data());
        Check(shown.size() == 69, "friend code shows 56 characters in 14 groups with 13 single spaces");

        bool shapeOk = true;
        for (size_t i = 0; i < shown.size(); i++) {
            const bool spaceExpected = (i % 5) == 4;
            if (spaceExpected != (shown[i] == ' ')) {
                shapeOk = false;
            }
        }
        Check(shapeOk, "friend code groups are 4 characters separated by single spaces");

        unsigned char back[32] = { 0 };
        Check(codes::ParseFriendCode(shown, back) && std::equal(key.begin(), key.end(), back),
              "friend code round trip");

        std::string lower = shown;
        for (auto& c : lower) {
            if ((c >= 'A') && (c <= 'Z')) {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        Check(codes::ParseFriendCode(lower, back) && std::equal(key.begin(), key.end(), back),
              "lower-case friend code parses");

        std::string hyphens = shown;
        std::replace(hyphens.begin(), hyphens.end(), ' ', '-');
        Check(codes::ParseFriendCode(hyphens, back) && std::equal(key.begin(), key.end(), back),
              "friend code with hyphens parses");

        std::string normal;
        Check(codes::Normalise(shown, normal) && normal.size() == codes::kFriendCodeChars,
              "a friend code normalises to 56 characters");
        Check(codes::FriendCodeCheck(normal) == 0,
              "the check over a whole friend code is zero, so the check is a true remainder");
    }
}

void CheckFriendCodeRefusals(const Key& key) {
    const std::string shown = codes::FormatFriendCode(key.data());
    std::string normal;
    codes::Normalise(shown, normal);

    unsigned char sink[32];
    for (auto& b : sink) {
        b = 0x5A;
    }
    Check(!codes::ParseFriendCode(normal.substr(0, 55), sink), "a friend code of 55 characters is refused");
    Check(!codes::ParseFriendCode(normal + "A", sink), "a friend code of 57 characters is refused");
    Check(!codes::ParseFriendCode("", sink), "an empty friend code is refused");

    std::string badCheck = normal;
    badCheck[52] = (normal[52] == '0') ? '1' : '0';
    Check(!codes::ParseFriendCode(badCheck, sink), "a bad check character is refused");

    bool untouched = true;
    for (const unsigned char b : sink) {
        if (b != 0x5A) {
            untouched = false;
        }
    }
    Check(untouched, "a refused friend code leaves the key output alone");
}

// The padding bits are the low four bits of the last key character. Setting one and recomputing the check makes the
// check right, so only the padding is wrong, and the parser must still refuse.
void CheckPaddingRefused(const Key& key) {
    std::string keyChars = codes::Base32Encode(key.data(), 256);
    const int last = SymbolOf(keyChars[codes::kFriendKeyChars - 1]);
    keyChars[codes::kFriendKeyChars - 1] = kAlphabet[last | 1];
    const uint32_t check = codes::FriendCodeCheck(keyChars);
    std::string code = keyChars;
    for (size_t i = 0; i < codes::kFriendCheckChars; i++) {
        const int shift = 15 - 5 * static_cast<int>(i);
        code.push_back(kAlphabet[(check >> shift) & 31u]);
    }
    unsigned char sink[32] = { 0 };
    Check(!codes::ParseFriendCode(code, sink), "nonzero padding bits are refused even when the check matches");
}

void CheckEveryTypo(const Key& key, size_t& substitutions, size_t& swaps) {
    std::string normal;
    codes::Normalise(codes::FormatFriendCode(key.data()), normal);
    unsigned char sink[32];
    Check(codes::ParseFriendCode(normal, sink), "the unchanged friend code parses");

    for (size_t pos = 0; pos < normal.size(); pos++) {
        for (const char c : std::string(kAlphabet)) {
            if (c == normal[pos]) {
                continue;
            }
            std::string typo = normal;
            typo[pos] = c;
            substitutions++;
            const bool accepted = codes::ParseFriendCode(typo, sink);
            if (accepted) {
                Check(false, "a substitution to " + std::string(1, c) + " at position " + std::to_string(pos) +
                                 " was accepted: " + typo);
            } else {
                gChecks++;
            }
        }
    }

    for (size_t pos = 0; pos + 1 < normal.size(); pos++) {
        if (normal[pos] == normal[pos + 1]) {
            continue;
        }
        std::string typo = normal;
        std::swap(typo[pos], typo[pos + 1]);
        swaps++;
        const bool accepted = codes::ParseFriendCode(typo, sink);
        if (accepted) {
            Check(false, "a transposition at position " + std::to_string(pos) + " was accepted: " + typo);
        } else {
            gChecks++;
        }
    }
}

void FuzzFriendCodes(const std::vector<Key>& keys, int rounds) {
    size_t accepted = 0;
    for (int round = 0; round < rounds; round++) {
        const Key& key = keys[Next() % keys.size()];
        std::string text = codes::FormatFriendCode(key.data());
        const int edits = 1 + static_cast<int>(Next() % 4);
        for (int e = 0; e < edits; e++) {
            const size_t at = Next() % text.size();
            switch (Next() % 4) {
                case 0:
                    text[at] = kAlphabet[Next() % 32];
                    break;
                case 1:
                    text[at] = static_cast<char>(Next() & 0x7F);
                    break;
                case 2:
                    text.erase(at, 1);
                    break;
                default:
                    text.insert(at, 1, kAlphabet[Next() % 32]);
                    break;
            }
        }
        unsigned char back[32];
        if (codes::ParseFriendCode(text, back)) {
            accepted++;
            std::string want;
            std::string got;
            codes::Normalise(codes::FormatFriendCode(back), got);
            codes::Normalise(text, want);
            Check(got == want, "an accepted fuzzed friend code is the code of the key it returns");
        }
    }
    std::printf("fuzz: %d friend-code mutations, %zu accepted, each one the code of its key\n", rounds, accepted);
}

// Friend codes are printed and shared, so a later build must read the codes of an earlier one. These were
// computed outside this module (Python: the key as a 260-bit number, its Crockford characters, and the
// remainder of their bits times x^20 modulo 0x100497), so a change to the alphabet, the bit order or the
// polynomial fails here even when the round trip still holds.
void CheckFriendCodeVectors() {
    struct Vector {
        unsigned char first;
        unsigned char step;
        const char* code;
        uint32_t check;
    };
    const Vector vectors[] = {
        { 0x00, 1, "000G 40R4 0M30 E209 185G R38E 1W81 24GK 2GAH C5RR 34D1 P70X 3RFG Y5J4", 0xF1644u },
        { 0xFF, 0, "ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZZ ZZZG BYED", 0x5F9CDu },
        { 0x00, 0, "0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000", 0x00000u },
    };
    for (const Vector& v : vectors) {
        unsigned char key[codes::kFriendKeyBytes];
        for (size_t i = 0; i < codes::kFriendKeyBytes; i++) {
            key[i] = static_cast<unsigned char>(v.first + v.step * i);
        }
        Check(codes::FormatFriendCode(key) == v.code, std::string("friend code vector ") + v.code);
        std::string normal;
        Check(codes::Normalise(v.code, normal) &&
                  codes::FriendCodeCheck(normal.substr(0, codes::kFriendKeyChars)) == v.check,
              std::string("friend code check vector ") + v.code);
        unsigned char back[codes::kFriendKeyBytes] = {};
        Check(codes::ParseFriendCode(v.code, back) && std::equal(back, back + codes::kFriendKeyBytes, key),
              std::string("friend code vector parses ") + v.code);
    }
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 3000;

    CheckAlphabetAndNormalise();
    CheckBase32RoundTrip(rounds);
    CheckBase32Refusals();
    CheckRoomCodes(rounds);

    const std::vector<Key> keys = TestKeys();
    CheckFriendCodeRoundTrip(keys);
    CheckFriendCodeVectors();
    CheckFriendCodeRefusals(keys[0]);
    for (const Key& key : keys) {
        CheckPaddingRefused(key);
    }

    size_t substitutions = 0;
    size_t swaps = 0;
    for (const Key& key : keys) {
        CheckEveryTypo(key, substitutions, swaps);
    }
    std::printf("typos: %zu keys, %zu substitutions and %zu transpositions, all refused\n", keys.size(),
                substitutions, swaps);

    FuzzFriendCodes(keys, rounds);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
