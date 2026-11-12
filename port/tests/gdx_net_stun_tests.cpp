// port/tests/gdx_net_stun_tests.cpp -- the STUN Binding codec and client (G-NET O5.6).
// Copyright (c) Zorkats
//
// Usage: gdx_net_stun_tests [fuzz rounds per sample, default 3000]
//
// The IPv4 response is the RFC 5769 section 2.2 sample. Its FINGERPRINT is checked against a second CRC-32,
// written here in table form, so the library's bitwise CRC is held to the RFC value and to an independent
// implementation. The IPv6 response is shaped like the RFC 5769 section 2.4 sample, with its address and
// MESSAGE-INTEGRITY placeholder zeroed; the parser walks past MESSAGE-INTEGRITY and never checks it, so the
// placeholder changes nothing that is tested.

#include "gdx_net_stun.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
namespace stun = gdx::net::stun;

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

// Two hex digits per byte; every other character (spaces, line breaks) is ignored.
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

// The independent CRC-32: a reflected table, not the library's bit loop.
uint32_t Crc32Ref(const uint8_t* data, size_t length) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void Put16(Bytes& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v & 0xFF);
}

void Put32(Bytes& b, size_t at, uint32_t v) {
    Put16(b, at, static_cast<uint16_t>(v >> 16));
    Put16(b, at + 2, static_cast<uint16_t>(v & 0xFFFF));
}

// The FINGERPRINT the RFC defines, for a message whose FINGERPRINT would start at fpAt.
uint32_t FingerprintRef(const Bytes& message, size_t fpAt) {
    Bytes covered(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(fpAt));
    Put16(covered, 2, static_cast<uint16_t>(fpAt - 20 + 8));
    return Crc32Ref(covered.data(), covered.size()) ^ 0x5354554Eu;
}

const stun::TransactionId kTxid = { 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae };
const stun::TransactionId kOtherTxid = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

// RFC 5769 section 2.2: IPv4 Binding Success Response, 192.0.2.1:32853, with MESSAGE-INTEGRITY and FINGERPRINT.
const char* const kRfc5769V4 =
    "01 01 00 3c 21 12 a4 42 b7 e7 a7 01 bc 34 d6 86 "
    "fa 87 df ae 80 22 00 0b 74 65 73 74 20 76 65 63 "
    "74 6f 72 20 00 20 00 08 00 01 a1 47 e1 12 a6 43 "
    "00 08 00 14 2b 91 f5 99 fd 9e 90 c3 8c 74 89 f9 "
    "2a f9 ba 53 f0 6b e7 d7 80 28 00 04 c0 7d 4c 96";

// RFC 5769 section 2.4 shape: IPv6 Binding Success Response, 2001:db8:1234:5678:11:2233:4455:6677:32853.
// The 20 bytes after the MESSAGE-INTEGRITY header are a placeholder; the FINGERPRINT is appended by the test.
const char* const kRfc5769V6Head =
    "01 01 00 48 21 12 a4 42 b7 e7 a7 01 bc 34 d6 86 "
    "fa 87 df ae 80 22 00 0b 74 65 73 74 20 76 65 63 "
    "74 6f 72 20 00 20 00 14 00 02 a1 47 01 13 a9 fa "
    "a5 d3 f1 79 bc 25 f4 b5 be d2 b9 d9 00 08 00 14 "
    "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00";

Bytes RfcV4() { return Hex(kRfc5769V4); }

Bytes RfcV6() {
    Bytes head = Hex(kRfc5769V6Head);
    const uint32_t fp = FingerprintRef(head, head.size());
    head.push_back(0x80);
    head.push_back(0x28);
    head.push_back(0x00);
    head.push_back(0x04);
    head.push_back(static_cast<uint8_t>(fp >> 24));
    head.push_back(static_cast<uint8_t>((fp >> 16) & 0xFF));
    head.push_back(static_cast<uint8_t>((fp >> 8) & 0xFF));
    head.push_back(static_cast<uint8_t>(fp & 0xFF));
    return head;
}

struct RawAttr {
    uint16_t type;
    Bytes value;
};

// A message with the given attributes, each padded to 4 bytes, and a FINGERPRINT when asked for. The header
// type, cookie and transaction id are caller-chosen so the tests can break one field at a time.
Bytes Build(uint16_t type, uint32_t cookie, const stun::TransactionId& txid, const std::vector<RawAttr>& attrs,
            bool fingerprint) {
    Bytes body;
    for (const RawAttr& a : attrs) {
        const size_t at = body.size();
        body.resize(at + 4 + ((a.value.size() + 3) & ~static_cast<size_t>(3)), 0);
        Put16(body, at, a.type);
        Put16(body, at + 2, static_cast<uint16_t>(a.value.size()));
        std::copy(a.value.begin(), a.value.end(), body.begin() + static_cast<std::ptrdiff_t>(at + 4));
    }
    Bytes message(20, 0);
    Put16(message, 0, type);
    Put32(message, 4, cookie);
    std::copy(txid.begin(), txid.end(), message.begin() + 8);
    message.insert(message.end(), body.begin(), body.end());
    if (fingerprint) {
        // The header length counts the FINGERPRINT too, so it is set before the CRC covers the header.
        const size_t fpAt = message.size();
        Put16(message, 2, static_cast<uint16_t>(fpAt - 20 + 8));
        Bytes covered(message.begin(), message.begin() + static_cast<std::ptrdiff_t>(fpAt));
        const uint32_t value = Crc32Ref(covered.data(), covered.size()) ^ 0x5354554Eu;
        message.resize(fpAt + 8);
        Put16(message, fpAt, 0x8028);
        Put16(message, fpAt + 2, 4);
        Put32(message, fpAt + 4, value);
    } else {
        Put16(message, 2, static_cast<uint16_t>(body.size()));
    }
    return message;
}

Bytes XorV4(uint16_t port, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint16_t xp = static_cast<uint16_t>(port ^ 0x2112);
    return Bytes{ 0x00, 0x01, static_cast<uint8_t>(xp >> 8), static_cast<uint8_t>(xp & 0xFF),
                  static_cast<uint8_t>(a ^ 0x21), static_cast<uint8_t>(b ^ 0x12),
                  static_cast<uint8_t>(c ^ 0xA4), static_cast<uint8_t>(d ^ 0x42) };
}

Bytes MappedV4(uint16_t port, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return Bytes{ 0x00, 0x01, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port & 0xFF), a, b, c, d };
}

const uint8_t kV6Address[16] = { 0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
                                 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

bool SameAddress(const stun::MappedAddress& a, const stun::MappedAddress& b) {
    return a.ip.family == b.ip.family && a.port == b.port && std::memcmp(a.ip.bytes, b.ip.bytes, 16) == 0;
}

stun::MappedAddress Sentinel() {
    stun::MappedAddress s;
    s.ip.family = 0x55;
    std::memset(s.ip.bytes, 0xAA, 16);
    s.port = 0xBEEF;
    return s;
}

void CheckRequests() {
    const Bytes plain = stun::BuildBindingRequest(kTxid, false);
    Check(plain.size() == 20, "a request without FINGERPRINT is the bare 20-byte header");
    Check(plain[0] == 0x00 && plain[1] == 0x01, "request type is Binding Request 0x0001");
    Check(plain[2] == 0 && plain[3] == 0, "bare request declares length 0");
    Check(plain[4] == 0x21 && plain[5] == 0x12 && plain[6] == 0xA4 && plain[7] == 0x42, "request carries the magic cookie");
    Check(std::memcmp(plain.data() + 8, kTxid.data(), 12) == 0, "request carries the transaction id");

    const Bytes fp = stun::BuildBindingRequest(kTxid, true);
    Check(fp.size() == 28, "a request with FINGERPRINT is 28 bytes");
    Check(fp[2] == 0 && fp[3] == 8, "FINGERPRINT request declares length 8");
    Check(fp[20] == 0x80 && fp[21] == 0x28 && fp[22] == 0x00 && fp[23] == 0x04, "FINGERPRINT attribute header");
    const uint32_t want = FingerprintRef(fp, 20);
    const uint32_t got = (static_cast<uint32_t>(fp[24]) << 24) | (static_cast<uint32_t>(fp[25]) << 16) |
                         (static_cast<uint32_t>(fp[26]) << 8) | fp[27];
    Check(got == want, "request FINGERPRINT matches the reference CRC-32");
    Check(stun::BuildBindingRequest(kTxid, true) == fp, "the request is deterministic for one transaction id");
}

void CheckRfcVectors() {
    const Bytes v4 = RfcV4();
    Check(v4.size() == 80, "RFC 5769 IPv4 vector is 80 bytes");
    Check(FingerprintRef(v4, 72) == 0xc07d4c96u, "the reference CRC-32 reproduces the RFC 5769 IPv4 FINGERPRINT c07d4c96");

    stun::MappedAddress out = Sentinel();
    Check(stun::ParseBindingReply(v4.data(), v4.size(), kTxid, out) == stun::Reply::Ok,
          "RFC 5769 IPv4 response parses, FINGERPRINT included");
    Check(out.ip.family == 4 && std::memcmp(out.ip.bytes, "\xC0\x00\x02\x01", 4) == 0,
          "RFC 5769 IPv4 response maps to 192.0.2.1");
    Check(out.port == 32853, "RFC 5769 IPv4 response maps to port 32853");

    Bytes bad = v4;
    bad[77] ^= 0x01;
    stun::MappedAddress untouched = Sentinel();
    Check(stun::ParseBindingReply(bad.data(), bad.size(), kTxid, untouched) == stun::Reply::BadFingerprint,
          "a flipped FINGERPRINT bit is refused");
    Check(SameAddress(untouched, Sentinel()), "a refused reply leaves the output unchanged");

    Bytes addrFlip = v4;
    addrFlip[45] ^= 0x10; // the third-to-last address byte of XOR-MAPPED-ADDRESS (value at 40)
    Check(stun::ParseBindingReply(addrFlip.data(), addrFlip.size(), kTxid, untouched) == stun::Reply::BadFingerprint,
          "a flipped address bit breaks the FINGERPRINT");

    const Bytes v6 = RfcV6();
    Check(v6.size() == 92, "IPv6 vector is 92 bytes");
    stun::MappedAddress out6 = Sentinel();
    Check(stun::ParseBindingReply(v6.data(), v6.size(), kTxid, out6) == stun::Reply::Ok,
          "IPv6 response parses, FINGERPRINT included");
    Check(out6.ip.family == 6 && std::memcmp(out6.ip.bytes, kV6Address, 16) == 0,
          "IPv6 response XORs the address with cookie and transaction id: 2001:db8:1234:5678:11:2233:4455:6677");
    Check(out6.port == 32853, "IPv6 response maps to port 32853");
}

void CheckMappedAddressChoice() {
    // MAPPED-ADDRESS alone: no XOR, the bytes are the address.
    const Bytes mapped = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                               { { stun::kAttrMappedAddress, MappedV4(8080, 198, 51, 100, 7) } }, true);
    stun::MappedAddress out = Sentinel();
    Check(stun::ParseBindingReply(mapped.data(), mapped.size(), kTxid, out) == stun::Reply::Ok,
          "MAPPED-ADDRESS alone parses");
    Check(out.ip.family == 4 && out.ip.bytes[0] == 198 && out.ip.bytes[3] == 7 && out.port == 8080,
          "MAPPED-ADDRESS is read in clear: 198.51.100.7:8080");

    // Both present: XOR-MAPPED-ADDRESS wins.
    const Bytes both = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                             { { stun::kAttrMappedAddress, MappedV4(1, 10, 0, 0, 1) },
                               { stun::kAttrXorMappedAddress, XorV4(32853, 192, 0, 2, 1) } },
                             true);
    stun::MappedAddress pick = Sentinel();
    Check(stun::ParseBindingReply(both.data(), both.size(), kTxid, pick) == stun::Reply::Ok,
          "a reply with both address attributes parses");
    Check(pick.ip.bytes[0] == 192 && pick.port == 32853, "XOR-MAPPED-ADDRESS is preferred over MAPPED-ADDRESS");

    // A malformed XOR-MAPPED-ADDRESS is an error; the client does not fall back to the clear address.
    const Bytes badXor = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                               { { stun::kAttrMappedAddress, MappedV4(1, 10, 0, 0, 1) },
                                 { stun::kAttrXorMappedAddress, Bytes{ 0x00, 0x03, 0, 1, 2, 3, 4, 5 } } },
                               false);
    Check(stun::ParseBindingReply(badXor.data(), badXor.size(), kTxid, out) == stun::Reply::BadAddress,
          "a malformed XOR-MAPPED-ADDRESS is refused, not replaced by MAPPED-ADDRESS");

    // Unknown attributes, comprehension-required or not, are skipped (the client needs only the address).
    const Bytes unknown = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                                { { 0x0030, Bytes{ 1, 2, 3, 4, 5 } },
                                  { 0x8023, Bytes{ 9, 9 } },
                                  { stun::kAttrXorMappedAddress, XorV4(32853, 192, 0, 2, 1) } },
                                true);
    Check(stun::ParseBindingReply(unknown.data(), unknown.size(), kTxid, out) == stun::Reply::Ok,
          "unknown attributes are skipped in a binding success response");

    const Bytes noAddr = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                               { { 0x8022, Bytes{ 't', 'e', 's', 't' } } }, true);
    Check(stun::ParseBindingReply(noAddr.data(), noAddr.size(), kTxid, out) == stun::Reply::NoAddress,
          "a reply with no address attribute is NoAddress");
}

void CheckHeaderRefusals() {
    const Bytes v4 = RfcV4();
    stun::MappedAddress out = Sentinel();

    Bytes request = v4;
    Put16(request, 0, stun::kTypeBindingRequest);
    Check(stun::ParseBindingReply(request.data(), request.size(), kTxid, out) == stun::Reply::NotSuccess,
          "a Binding Request is not a success response");

    Bytes indication = v4;
    Put16(indication, 0, 0x0011);
    Check(stun::ParseBindingReply(indication.data(), indication.size(), kTxid, out) == stun::Reply::NotSuccess,
          "a Binding Indication is not a success response");

    Bytes error = v4;
    Put16(error, 0, stun::kTypeBindingError);
    Check(stun::ParseBindingReply(error.data(), error.size(), kTxid, out) == stun::Reply::ErrorResponse,
          "a Binding Error Response is refused");
    Check(SameAddress(out, Sentinel()), "a refused error response leaves the output unchanged");

    Bytes cookie = v4;
    cookie[4] ^= 0x01;
    Check(stun::ParseBindingReply(cookie.data(), cookie.size(), kTxid, out) == stun::Reply::BadCookie,
          "a wrong magic cookie is refused");

    Check(stun::ParseBindingReply(v4.data(), v4.size(), kOtherTxid, out) == stun::Reply::WrongTransaction,
          "a reply to another transaction id is refused");

    Bytes oddLength = v4;
    Put16(oddLength, 2, 0x3d);
    Check(stun::ParseBindingReply(oddLength.data(), oddLength.size(), kTxid, out) == stun::Reply::BadLength,
          "a declared length that is not a multiple of 4 is refused");

    Bytes longDeclared = v4;
    Put16(longDeclared, 2, 0x40);
    Check(stun::ParseBindingReply(longDeclared.data(), longDeclared.size(), kTxid, out) == stun::Reply::Truncated,
          "a declared length past the datagram is refused as truncated");

    Bytes shortDeclared = v4;
    Put16(shortDeclared, 2, 0x38);
    Check(stun::ParseBindingReply(shortDeclared.data(), shortDeclared.size(), kTxid, out) == stun::Reply::Trailing,
          "bytes past the declared length are refused as trailing");

    Bytes longer = v4;
    longer.push_back(0);
    Check(stun::ParseBindingReply(longer.data(), longer.size(), kTxid, out) == stun::Reply::Trailing,
          "one trailing byte after a whole reply is refused");

    Bytes more = v4;
    more.insert(more.end(), 4, 0xEE);
    Check(stun::ParseBindingReply(more.data(), more.size(), kTxid, out) == stun::Reply::Trailing,
          "four trailing bytes after a whole reply are refused");

    bool allRefused = true;
    for (size_t n = 0; n < v4.size(); n++) {
        stun::MappedAddress probe = Sentinel();
        if (stun::ParseBindingReply(v4.data(), n, kTxid, probe) == stun::Reply::Ok || !SameAddress(probe, Sentinel())) {
            allRefused = false;
        }
    }
    Check(allRefused, "every truncation of the RFC 5769 IPv4 reply is refused and leaves the output unchanged");
}

void CheckAttributeWalk() {
    stun::MappedAddress out = Sentinel();
    const Bytes goodXor = XorV4(32853, 192, 0, 2, 1);

    // The attribute length runs past the message.
    Bytes runs = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid, {}, false);
    runs.insert(runs.end(), { 0x00, 0x20, 0xFF, 0xF0, 0x00, 0x01, 0x02, 0x03 });
    Put16(runs, 2, 8);
    Check(stun::ParseBindingReply(runs.data(), runs.size(), kTxid, out) == stun::Reply::BadAttribute,
          "an attribute longer than the message is refused");

    // A declared length that is not a multiple of 4 is caught before any attribute is walked.
    const Bytes partial = { 0x01, 0x01, 0x00, 0x03, 0x21, 0x12, 0xA4, 0x42, 0xB7, 0xE7, 0xA7, 0x01,
                            0xBC, 0x34, 0xD6, 0x86, 0xFA, 0x87, 0xDF, 0xAE, 0x00, 0x20, 0x00 };
    Check(stun::ParseBindingReply(partial.data(), partial.size(), kTxid, out) == stun::Reply::BadLength,
          "a three-byte attribute tail is refused by the length rule");

    // The padded value runs past the message.
    const Bytes padded = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid, {}, false);
    Bytes pad = padded;
    pad.insert(pad.end(), { 0x00, 0x20, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04 });
    Put16(pad, 2, 8);
    Check(stun::ParseBindingReply(pad.data(), pad.size(), kTxid, out) == stun::Reply::BadAttribute,
          "a padded value that runs past the message is refused");

    // FINGERPRINT must close the message.
    Bytes fpMiddle = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                           { { stun::kAttrXorMappedAddress, goodXor } }, false);
    fpMiddle.insert(fpMiddle.end(), { 0x80, 0x28, 0x00, 0x04, 0, 0, 0, 0 });
    fpMiddle.insert(fpMiddle.end(), { 0x80, 0x22, 0x00, 0x00 });
    Put16(fpMiddle, 2, static_cast<uint16_t>(fpMiddle.size() - 20));
    Check(stun::ParseBindingReply(fpMiddle.data(), fpMiddle.size(), kTxid, out) == stun::Reply::BadAttribute,
          "a FINGERPRINT followed by another attribute is refused");

    Bytes fpLong = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                         { { stun::kAttrXorMappedAddress, goodXor } }, false);
    fpLong.insert(fpLong.end(), { 0x80, 0x28, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 0, 0 });
    Put16(fpLong, 2, static_cast<uint16_t>(fpLong.size() - 20));
    Check(stun::ParseBindingReply(fpLong.data(), fpLong.size(), kTxid, out) == stun::Reply::BadAttribute,
          "a FINGERPRINT of 8 bytes is refused");

    Bytes fpWrong = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                          { { stun::kAttrXorMappedAddress, goodXor } }, true);
    fpWrong[fpWrong.size() - 1] ^= 0x80;
    Check(stun::ParseBindingReply(fpWrong.data(), fpWrong.size(), kTxid, out) == stun::Reply::BadFingerprint,
          "a wrong FINGERPRINT value is refused");

    // A FINGERPRINT-less reply still parses: the attribute is optional.
    const Bytes noFp = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                             { { stun::kAttrXorMappedAddress, goodXor } }, false);
    Check(stun::ParseBindingReply(noFp.data(), noFp.size(), kTxid, out) == stun::Reply::Ok,
          "a reply without FINGERPRINT parses");
}

void CheckAddressRefusals() {
    stun::MappedAddress out = Sentinel();
    const struct {
        const char* what;
        Bytes value;
    } cases[] = {
        { "an unknown family", Bytes{ 0x00, 0x03, 0, 1, 2, 3, 4, 5 } },
        { "an IPv4 value of 12 bytes", Bytes{ 0x00, 0x01, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { "an IPv6 value of 8 bytes", Bytes{ 0x00, 0x02, 0, 1, 2, 3, 4, 5 } },
        { "a value of 3 bytes", Bytes{ 0x00, 0x01, 0 } },
        { "an empty value", Bytes{} },
    };
    for (const auto& c : cases) {
        const Bytes message = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                                    { { stun::kAttrXorMappedAddress, c.value } }, false);
        Check(stun::ParseBindingReply(message.data(), message.size(), kTxid, out) == stun::Reply::BadAddress,
              std::string("XOR-MAPPED-ADDRESS with ") + c.what + " is refused");
    }
    const Bytes wellFormed = Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
                                   { { stun::kAttrXorMappedAddress, XorV4(32853, 192, 0, 2, 1) } }, false);
    stun::MappedAddress probe = Sentinel();
    Check(stun::ParseBindingReply(wellFormed.data(), wellFormed.size(), kTxid, probe) == stun::Reply::Ok,
          "the well-formed IPv4 address control case parses");
    Check(!SameAddress(probe, Sentinel()), "a parsed address replaces the output");
    Check(SameAddress(out, Sentinel()), "refused addresses leave the output unchanged");
}

void CheckClient() {
    const Bytes v4 = RfcV4();

    // Retransmission: sends at 0, 250, 750 and 1750 ms after start, given up at 3000 ms.
    stun::BindingClient timed;
    timed.Start(1000, kTxid);
    Check(timed.state() == stun::BindingClient::State::Waiting, "Start leaves the client waiting");
    std::vector<uint64_t> sentAt;
    for (uint64_t t = 1000; t <= 5000; t++) {
        const bool wasWaiting = timed.state() == stun::BindingClient::State::Waiting;
        auto request = timed.Pump(t);
        if (request.has_value()) {
            sentAt.push_back(t);
            Check(request->size() == 28, "each retransmission carries the FINGERPRINT");
            Check(*request == stun::BuildBindingRequest(kTxid, true), "retransmissions repeat the same request");
        }
        if (!wasWaiting) {
            Check(!request.has_value(), "a finished client sends nothing");
        }
    }
    const std::vector<uint64_t> want = { 1000, 1250, 1750, 2750 };
    Check(sentAt == want, "requests go at start, +250, +750 and +1750 ms (RTO doubling)");
    Check(timed.sends() == stun::kMaxSends && timed.sends() == 4, "exactly four requests are sent");
    Check(timed.state() == stun::BindingClient::State::Failed, "the client fails at start + 3000 ms");

    stun::BindingClient quiet;
    quiet.Start(0, kTxid);
    Check(quiet.Pump(0).has_value(), "the first request is sent at once");
    Check(!quiet.Pump(249).has_value(), "no second request before 250 ms");
    Check(quiet.Pump(250).has_value(), "the second request goes at 250 ms");
    Check(!quiet.Pump(749).has_value(), "no third request before 750 ms");
    Check(quiet.Pump(750).has_value(), "the third request goes at 750 ms");
    Check(!quiet.Pump(1749).has_value(), "no fourth request before 1750 ms");
    Check(quiet.Pump(1750).has_value(), "the fourth request goes at 1750 ms");
    Check(!quiet.Pump(2999).has_value() && quiet.state() == stun::BindingClient::State::Waiting,
          "still waiting at 2999 ms");
    Check(!quiet.Pump(3000).has_value() && quiet.state() == stun::BindingClient::State::Failed,
          "Failed at 3000 ms with no reply");
    Check(quiet.sends() == 4, "a failed client stays at four requests");

    // A late reply after the give-up time is ignored.
    stun::BindingClient late;
    late.Start(0, kTxid);
    (void)late.Pump(0);
    Check(!late.OnDatagram(v4.data(), v4.size(), 3000), "a reply at the give-up time is ignored");
    Check(late.state() == stun::BindingClient::State::Failed, "the late reply leaves the client Failed");

    // A reply that answers: Done, the address, and then silence.
    stun::BindingClient ok;
    ok.Start(500, kTxid);
    (void)ok.Pump(500);
    Check(ok.OnDatagram(v4.data(), v4.size(), 600), "the RFC 5769 reply completes the transaction");
    Check(ok.state() == stun::BindingClient::State::Done, "a matched reply moves the client to Done");
    Check(ok.address().ip.family == 4 && ok.address().port == 32853 && ok.address().ip.bytes[0] == 192,
          "the done client reports 192.0.2.1:32853");
    Check(!ok.Pump(700).has_value(), "a done client sends nothing");
    Check(!ok.OnDatagram(v4.data(), v4.size(), 800), "a done client takes no second reply");

    // Replies that do not answer this transaction, and error responses, keep the client waiting.
    stun::BindingClient picky;
    picky.Start(0, kTxid);
    (void)picky.Pump(0);
    Bytes error = v4;
    Put16(error, 0, stun::kTypeBindingError);
    Check(!picky.OnDatagram(error.data(), error.size(), 10), "an error response is ignored by the client");
    Check(picky.state() == stun::BindingClient::State::Waiting, "an error response leaves the client waiting");
    Bytes otherTx = v4;
    std::copy(kOtherTxid.begin(), kOtherTxid.end(), otherTx.begin() + 8);
    Check(!picky.OnDatagram(otherTx.data(), otherTx.size(), 20), "a reply for another transaction is ignored");
    Check(picky.OnDatagram(v4.data(), v4.size(), 30), "the matching reply still completes after the strays");

    // The request without FINGERPRINT is 20 bytes.
    stun::BindingClient bare(false);
    bare.Start(0, kTxid);
    const auto bareRequest = bare.Pump(0);
    Check(bareRequest.has_value() && bareRequest->size() == 20, "a client without fingerprint sends 20 bytes");

    // Start again resets a finished client.
    stun::BindingClient again;
    again.Start(0, kTxid);
    (void)again.Pump(0);
    (void)again.OnDatagram(v4.data(), v4.size(), 10);
    again.Start(5000, kOtherTxid);
    Check(again.state() == stun::BindingClient::State::Waiting && again.sends() == 0,
          "Start resets a done client to waiting with no sends");
    Check(again.address().port == 0, "Start clears the old address");
}

uint64_t gRng = 0x9E3779B97F4A7C15ull;

uint64_t Next() {
    gRng += 0x9E3779B97F4A7C15ull;
    uint64_t z = gRng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Fuzz(int rounds) {
    const std::vector<Bytes> seeds = {
        RfcV4(),
        RfcV6(),
        Build(stun::kTypeBindingSuccess, stun::kMagicCookie, kTxid,
              { { stun::kAttrMappedAddress, MappedV4(1, 2, 3, 4, 5) },
                { stun::kAttrXorMappedAddress, XorV4(4444, 9, 9, 9, 9) } },
              true),
        Build(stun::kTypeBindingError, stun::kMagicCookie, kTxid, { { 0x0009, Bytes{ 0, 0, 4, 20 } } }, false),
    };
    int ok = 0;
    for (int round = 0; round < rounds; round++) {
        Bytes data = seeds[static_cast<size_t>(Next() % seeds.size())];
        switch (Next() % 5) {
        case 0: {
            const int flips = 1 + static_cast<int>(Next() % 3);
            for (int i = 0; i < flips && !data.empty(); i++) {
                data[static_cast<size_t>(Next() % data.size())] ^= static_cast<uint8_t>(1u << static_cast<unsigned>(Next() % 8));
            }
            break;
        }
        case 1:
            data.resize(static_cast<size_t>(Next() % (data.size() + 1)));
            break;
        case 2: {
            const size_t extra = static_cast<size_t>(Next() % 9);
            for (size_t i = 0; i < extra; i++) {
                data.push_back(static_cast<uint8_t>(Next()));
            }
            break;
        }
        case 3:
            if (data.size() >= 2) {
                const size_t at = static_cast<size_t>(Next() % (data.size() - 1));
                Put16(data, at, static_cast<uint16_t>(Next()));
            }
            break;
        default: {
            data.assign(static_cast<size_t>(Next() % 120), 0);
            for (uint8_t& b : data) {
                b = static_cast<uint8_t>(Next());
            }
            if (data.size() >= 20 && (Next() & 1u)) {
                Put16(data, 0, stun::kTypeBindingSuccess);
                Put32(data, 4, stun::kMagicCookie);
                std::copy(kTxid.begin(), kTxid.end(), data.begin() + 8);
            }
            break;
        }
        }
        stun::MappedAddress out = Sentinel();
        const stun::Reply r = stun::ParseBindingReply(data.data(), data.size(), kTxid, out);
        if (r == stun::Reply::Ok) {
            ok++;
            Check(out.ip.family == 4 || out.ip.family == 6, "a parsed fuzz reply has a known family");
        } else {
            Check(SameAddress(out, Sentinel()), "a refused fuzz reply leaves the output unchanged");
        }
        stun::BindingClient client;
        client.Start(0, kTxid);
        (void)client.Pump(0);
        (void)client.OnDatagram(data.data(), data.size(), 1);
    }
    std::printf("fuzz: %d rounds, %d parsed\n", rounds, ok);
}

} // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 3000;
    CheckRequests();
    CheckRfcVectors();
    CheckMappedAddressChoice();
    CheckHeaderRefusals();
    CheckAttributeWalk();
    CheckAddressRefusals();
    CheckClient();
    Fuzz(rounds > 0 ? rounds : 3000);
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
