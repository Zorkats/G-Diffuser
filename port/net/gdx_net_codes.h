// port/net/gdx_net_codes.h: Crockford base32, room codes and friend codes (G-NET O5.2 and O7.2).
// Copyright (c) Zorkats
//
// Pure: no socket, no clock, no crypto and no random source. A room code is made from bytes the caller passes in.
// The integrator must pass OS CSPRNG output, because a room code that a stranger can predict lets that stranger
// walk into the room.
//
// The alphabet is Crockford base32: 0-9 and A-Z without I, L, O and U. Those four are the letters that read as
// digits on a screen or on paper, so a typed code can be corrected rather than refused. Normalise maps I and L to
// 1 and O to 0. U is refused, because it is the one letter a reader cannot map to anything safe.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gdx::net::codes {

constexpr size_t kRoomCodeChars = 6;
// A room code is 30 bits: its value is below kRoomCodeLimit.
constexpr uint32_t kRoomCodeLimit = 1u << 30;

constexpr size_t kFriendKeyBytes = 32;
// 256 key bits take 52 characters, which hold 260 bits. The last 4 bits are padding and must be zero.
constexpr size_t kFriendKeyChars = 52;
// 4 characters carry the 20-bit check.
constexpr size_t kFriendCheckChars = 4;
constexpr size_t kFriendCodeChars = kFriendKeyChars + kFriendCheckChars;
constexpr size_t kFriendGroupChars = 4;

// Upper-cases ASCII letters, maps I and L to 1 and O to 0, and drops spaces and hyphens. Refuses U, a non-ASCII byte
// and any other character. out is written only on success.
bool Normalise(const std::string& text, std::string& out);

// Encodes the first bitCount bits of bytes, most significant bit first. The last character is padded with zero bits.
// bytes must hold at least (bitCount + 7) / 8 bytes when bitCount is above zero.
std::string Base32Encode(const unsigned char* bytes, size_t bitCount);

// Decodes text, which must already be normalised: exactly (bitCount + 4) / 5 characters from the alphabet, with zero
// padding bits. Writes (bitCount + 7) / 8 bytes to out, with the unused low bits of the last byte zero. out is
// written only on success.
bool Base32Decode(const std::string& text, size_t bitCount, unsigned char* out);

// Takes the low 30 bits of 4 bytes of CSPRNG output. Masking rather than taking a modulus keeps every value equally
// likely.
uint32_t RoomCodeFromRandom(const unsigned char random[4]);

// Writes the display form "ABC-DEF". Returns an empty string when value is kRoomCodeLimit or more.
std::string FormatRoomCode(uint32_t value);

// Accepts the display form, lower case, spaces and the confusable letters. Leaves value alone on any refusal.
bool ParseRoomCode(const std::string& text, uint32_t& value);

// The friend code is the 32-byte friend key in base32, then 4 check characters, in 14 groups of 4 separated by one
// space each.
std::string FormatFriendCode(const unsigned char key[kFriendKeyBytes]);

// Accepts any display form that Normalise accepts. Writes key only on success. Refuses a wrong length, a character
// outside the alphabet, a check that does not match and nonzero padding bits.
bool ParseFriendCode(const std::string& text, unsigned char key[kFriendKeyBytes]);

// The 20-bit remainder of the 5-bit values of text, taken most significant bit first, divided by the friend code
// polynomial (see gdx_net_codes.cpp). text must hold only alphabet characters. Fed the key characters, it gives the
// check characters; fed a whole friend code, it gives 0 for every code this module wrote.
uint32_t FriendCodeCheck(const std::string& text);

} // namespace gdx::net::codes
