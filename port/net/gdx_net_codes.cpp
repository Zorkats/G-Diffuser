// port/net/gdx_net_codes.cpp: Crockford base32, room codes and friend codes. The rules are in gdx_net_codes.h.
// Copyright (c) Zorkats

#include "gdx_net_codes.h"

#include <vector>

namespace gdx::net::codes {

namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kBitsPerChar = 5;
constexpr uint32_t kCheckMask = 0xFFFFFu;

// The friend code check polynomial is x^20 + x^10 + x^7 + x^4 + x^2 + x + 1, which is 0x100497. The x^20 term is
// implied by the mask below, and the x^0 term is 1. That constant term is what the guarantee rests on: a generator
// of degree r with a nonzero constant term detects every burst of r bits or fewer. A substituted character is a
// 5-bit burst and an adjacent swap of two distinct characters is a 10-bit burst, and both are below 20 bits, so
// every such typo changes the check. The remaining terms only matter for errors that span more bits.
constexpr uint32_t kCheckPolynomialLow = 0x00497u;

// Returns the 5-bit value of an alphabet character, or -1. The input is expected to be normalised already.
int ValueOf(char c) {
    for (int i = 0; i < 32; i++) {
        if (kAlphabet[i] == c) {
            return i;
        }
    }
    return -1;
}

// Maps one input byte to its normalised character, or returns false when the byte is refused.
bool NormaliseChar(unsigned char c, char& out) {
    unsigned char upper = c;
    if ((c >= 'a') && (c <= 'z')) {
        upper = static_cast<unsigned char>(c - 'a' + 'A');
    }
    if ((upper == 'I') || (upper == 'L')) {
        upper = '1';
    } else if (upper == 'O') {
        upper = '0';
    }
    if (ValueOf(static_cast<char>(upper)) < 0) {
        return false;
    }
    out = static_cast<char>(upper);
    return true;
}

} // namespace

bool Normalise(const std::string& text, std::string& out) {
    std::string result;
    result.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if ((c == ' ') || (c == '-')) {
            continue;
        }
        char mapped = 0;
        if (!NormaliseChar(c, mapped)) {
            return false;
        }
        result.push_back(mapped);
    }
    out = result;
    return true;
}

std::string Base32Encode(const unsigned char* bytes, size_t bitCount) {
    std::string out;
    uint32_t acc = 0;
    size_t accBits = 0;
    for (size_t i = 0; i < bitCount; i++) {
        const uint32_t bit = (static_cast<uint32_t>(bytes[i / 8]) >> (7 - (i % 8))) & 1u;
        acc = (acc << 1) | bit;
        accBits++;
        if (accBits == kBitsPerChar) {
            out.push_back(kAlphabet[acc & 31u]);
            acc = 0;
            accBits = 0;
        }
    }
    if (accBits > 0) {
        acc <<= (kBitsPerChar - accBits);
        out.push_back(kAlphabet[acc & 31u]);
    }
    return out;
}

bool Base32Decode(const std::string& text, size_t bitCount, unsigned char* out) {
    const size_t chars = (bitCount + kBitsPerChar - 1) / kBitsPerChar;
    const size_t byteCount = (bitCount + 7) / 8;
    if ((text.size() != chars) || ((out == nullptr) && (byteCount > 0))) {
        return false;
    }
    // Decoding goes into a scratch buffer, so a refused text never leaves a half-written output.
    std::vector<unsigned char> bytes(byteCount, 0);
    size_t bitPos = 0;
    for (size_t c = 0; c < chars; c++) {
        const int value = ValueOf(text[c]);
        if (value < 0) {
            return false;
        }
        for (int j = 4; j >= 0; j--) {
            const uint32_t bit = (static_cast<uint32_t>(value) >> j) & 1u;
            if (bitPos < bitCount) {
                if (bit != 0) {
                    bytes[bitPos / 8] = static_cast<unsigned char>(bytes[bitPos / 8] | (0x80u >> (bitPos % 8)));
                }
            } else if (bit != 0) {
                return false;
            }
            bitPos++;
        }
    }
    for (size_t i = 0; i < byteCount; i++) {
        out[i] = bytes[i];
    }
    return true;
}

uint32_t RoomCodeFromRandom(const unsigned char random[4]) {
    const uint32_t word = (static_cast<uint32_t>(random[0]) << 24) | (static_cast<uint32_t>(random[1]) << 16) |
                          (static_cast<uint32_t>(random[2]) << 8) | static_cast<uint32_t>(random[3]);
    return word & (kRoomCodeLimit - 1u);
}

std::string FormatRoomCode(uint32_t value) {
    if (value >= kRoomCodeLimit) {
        return std::string();
    }
    std::string out;
    for (size_t i = 0; i < kRoomCodeChars; i++) {
        if (i == kRoomCodeChars / 2) {
            out.push_back('-');
        }
        const int shift = 25 - 5 * static_cast<int>(i);
        out.push_back(kAlphabet[(value >> shift) & 31u]);
    }
    return out;
}

bool ParseRoomCode(const std::string& text, uint32_t& value) {
    std::string normal;
    if (!Normalise(text, normal) || (normal.size() != kRoomCodeChars)) {
        return false;
    }
    uint32_t acc = 0;
    for (const char c : normal) {
        acc = (acc << 5) | static_cast<uint32_t>(ValueOf(c));
    }
    value = acc;
    return true;
}

uint32_t FriendCodeCheck(const std::string& text) {
    uint32_t reg = 0;
    for (const char c : text) {
        const uint32_t symbol = static_cast<uint32_t>(ValueOf(c));
        for (int j = 4; j >= 0; j--) {
            const uint32_t feedback = ((reg >> 19) & 1u) ^ ((symbol >> j) & 1u);
            reg = (reg << 1) & kCheckMask;
            if (feedback != 0) {
                reg ^= kCheckPolynomialLow;
            }
        }
    }
    return reg;
}

std::string FormatFriendCode(const unsigned char key[kFriendKeyBytes]) {
    std::string code = Base32Encode(key, kFriendKeyBytes * 8);
    const uint32_t check = FriendCodeCheck(code);
    for (size_t i = 0; i < kFriendCheckChars; i++) {
        const int shift = 15 - 5 * static_cast<int>(i);
        code.push_back(kAlphabet[(check >> shift) & 31u]);
    }
    std::string out;
    for (size_t i = 0; i < code.size(); i++) {
        if ((i > 0) && ((i % kFriendGroupChars) == 0)) {
            out.push_back(' ');
        }
        out.push_back(code[i]);
    }
    return out;
}

bool ParseFriendCode(const std::string& text, unsigned char key[kFriendKeyBytes]) {
    if (key == nullptr) {
        return false;
    }
    std::string normal;
    if (!Normalise(text, normal) || (normal.size() != kFriendCodeChars)) {
        return false;
    }
    const std::string keyChars = normal.substr(0, kFriendKeyChars);
    const std::string checkChars = normal.substr(kFriendKeyChars);
    unsigned char decoded[kFriendKeyBytes];
    if (!Base32Decode(keyChars, kFriendKeyBytes * 8, decoded)) {
        return false;
    }
    uint32_t check = 0;
    for (const char c : checkChars) {
        check = (check << 5) | static_cast<uint32_t>(ValueOf(c));
    }
    if (check != FriendCodeCheck(keyChars)) {
        return false;
    }
    for (size_t i = 0; i < kFriendKeyBytes; i++) {
        key[i] = decoded[i];
    }
    return true;
}

} // namespace gdx::net::codes
