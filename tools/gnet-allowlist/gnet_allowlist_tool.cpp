// tools/gnet-allowlist/gnet_allowlist_tool.cpp -- the owner's key and signing tool for the G-NET stable allowlist
// (G-NET O5.9). README.md has the input format and the release steps.
// Copyright (c) Zorkats
//
// Usage:
//   gnet_allowlist_tool keygen <secret-out> <public-out>
//   gnet_allowlist_tool sign <secret-file> <in.txt> <out.bin>
//   gnet_allowlist_tool sign --secret-env NAME <in.txt> <out.bin>
//   gnet_allowlist_tool verify <public-hex> <in.bin>
//
// The secret is the 32-byte Ed25519 seed as 64 hex digits. The tool never prints it, and keygen refuses to
// overwrite a file, so a key is never replaced by accident.

#include "gdx_net_allowlist.h"

#include "monocypher.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#endif

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace allow = gdx::net::allowlist;

namespace {

void Fail(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
}

void Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  gnet_allowlist_tool keygen <secret-out> <public-out>\n"
                 "  gnet_allowlist_tool sign <secret-file> <in.txt> <out.bin>\n"
                 "  gnet_allowlist_tool sign --secret-env NAME <in.txt> <out.bin>\n"
                 "  gnet_allowlist_tool verify <public-hex> <in.bin>\n");
}

// Fills out from the operating system's random source. A clock-seeded generator would make the key guessable.
bool SystemRandom(uint8_t* out, size_t length) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    size_t done = 0;
    while (done < length) {
        const ssize_t got = getrandom(out + done, length - done, 0);
        if (got <= 0) {
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(length));
    return urandom.gcount() == static_cast<std::streamsize>(length);
#endif
}

// MSVC flags getenv under /W3, so Windows reads the variable through _dupenv_s instead.
bool EnvValue(const std::string& name, std::string& out) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) != 0 || value == nullptr) {
        return false;
    }
    out = value;
    std::free(value);
    return true;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return false;
    }
    out = value;
    return true;
#endif
}

bool FileExists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

bool ReadText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return !in.bad();
}

bool ReadBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::string text;
    if (!ReadText(path, text)) {
        return false;
    }
    out.assign(text.size(), uint8_t{0});
    for (size_t i = 0; i < text.size(); i++) {
        out[i] = static_cast<uint8_t>(text[i]);
    }
    return true;
}

bool WriteText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << text;
    out.close();
    return !out.fail();
}

bool WriteBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
}

// The secret file is the owner's to keep. On POSIX only the owner may read it; Windows uses the profile's ACL.
void RestrictToOwner(const std::string& path) {
#if !defined(_WIN32)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        Fail("could not restrict " + path + " to its owner");
    }
#else
    (void)path;
#endif
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string ToHex(const uint8_t* data, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < length; i++) {
        text.push_back(kDigits[data[i] >> 4]);
        text.push_back(kDigits[data[i] & 0x0F]);
    }
    return text;
}

// Reads exactly length bytes of hex, in either case, with white space around them allowed. out is written only on
// success, so a bad secret never leaves a partial value behind.
bool FromHex(const std::string& text, uint8_t* out, size_t length) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        begin++;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        end--;
    }
    if (end - begin != 2 * length) {
        return false;
    }
    std::vector<uint8_t> bytes(length, uint8_t{0});
    for (size_t i = 0; i < length; i++) {
        const int high = HexValue(text[begin + 2 * i]);
        const int low = HexValue(text[begin + 2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    for (size_t i = 0; i < length; i++) {
        out[i] = bytes[i];
    }
    return true;
}

// Whole decimal numbers only: no sign, no space, no overflow past maxValue.
bool ParseDecimal(const std::string& text, uint64_t maxValue, uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<uint64_t>(ch - '0');
        if (value > (maxValue - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// Parses the owner's list file (README.md has the lines). Returns an empty string on success, else the first
// problem with its line number. The list's own rules are checked later by allow::CheckList.
std::string ParseListText(const std::string& text, allow::List& list) {
    std::set<std::string> seen;
    std::istringstream lines(text);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        lineNumber++;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::string where = "line " + std::to_string(lineNumber) + ": ";
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space);
        const std::string value = space == std::string::npos ? std::string() : line.substr(space + 1);

        if (key == "allow") {
            list.allowed.push_back(value);
            continue;
        }
        if (key == "yank") {
            const size_t split = value.find(' ');
            allow::Yank yank;
            yank.tag = value.substr(0, split);
            yank.reason = split == std::string::npos ? std::string() : value.substr(split + 1);
            list.yanked.push_back(yank);
            continue;
        }
        if (key != "protocol" && key != "sequence" && key != "issued" && key != "grace_until" && key != "current") {
            return where + "unknown key '" + key + "'";
        }
        if (!seen.insert(key).second) {
            return where + key + " is given twice";
        }
        if (key == "current") {
            list.current = value;
            continue;
        }
        uint64_t number = 0;
        const uint64_t limit = key == "protocol" ? 0xFFFFFFFFull : ~static_cast<uint64_t>(0);
        if (!ParseDecimal(value, limit, number)) {
            return where + key + " needs a whole number in range";
        }
        if (key == "protocol") {
            list.protocol = static_cast<uint32_t>(number);
        } else if (key == "sequence") {
            list.sequence = number;
        } else if (key == "issued") {
            list.issued = number;
        } else {
            list.graceUntil = number;
        }
    }
    for (const char* required : {"protocol", "sequence", "issued", "current"}) {
        if (seen.count(required) == 0) {
            return std::string("missing ") + required;
        }
    }
    return std::string();
}

int CommandKeygen(const std::string& secretPath, const std::string& publicPath) {
    if (FileExists(secretPath) || FileExists(publicPath)) {
        Fail("refusing to overwrite an existing file");
        return 2;
    }
    allow::Seed seed{};
    if (!SystemRandom(seed.data(), seed.size())) {
        Fail("the operating system random source failed");
        return 1;
    }
    allow::PublicKey publicKey{};
    allow::SecretKey secretKey{};
    allow::KeyPairFromSeed(seed, publicKey, secretKey);
    const std::string seedHex = ToHex(seed.data(), seed.size());
    const std::string publicHex = ToHex(publicKey.data(), publicKey.size());
    crypto_wipe(seed.data(), seed.size());
    crypto_wipe(secretKey.data(), secretKey.size());

    if (!WriteText(secretPath, seedHex + "\n")) {
        Fail("could not write the secret file");
        return 1;
    }
    RestrictToOwner(secretPath);
    if (!WriteText(publicPath, publicHex + "\n")) {
        Fail("could not write the public key file; the secret file was written and must be kept");
        return 1;
    }
    std::printf("public key: %s\n", publicHex.c_str());
    std::printf("the secret is in %s and is not printed here\n", secretPath.c_str());
    return 0;
}

int CommandSign(bool fromEnv, const std::string& secretSource, const std::string& inPath, const std::string& outPath) {
    std::string secretText;
    if (fromEnv) {
        if (!EnvValue(secretSource, secretText)) {
            Fail("environment variable " + secretSource + " is not set");
            return 2;
        }
    } else if (!ReadText(secretSource, secretText)) {
        Fail("could not read the secret file");
        return 2;
    }
    allow::Seed seed{};
    if (!FromHex(secretText, seed.data(), seed.size())) {
        Fail("the secret is not 64 hex digits");
        return 2;
    }
    allow::PublicKey publicKey{};
    allow::SecretKey secretKey{};
    allow::KeyPairFromSeed(seed, publicKey, secretKey);
    crypto_wipe(seed.data(), seed.size());

    std::string inText;
    if (!ReadText(inPath, inText)) {
        Fail("could not read " + inPath);
        return 2;
    }
    allow::List list;
    const std::string parseProblem = ParseListText(inText, list);
    if (!parseProblem.empty()) {
        Fail(parseProblem);
        return 2;
    }
    const char* listProblem = allow::CheckList(list);
    if (listProblem != nullptr) {
        Fail(std::string("the list is not valid: ") + listProblem);
        return 2;
    }
    std::vector<uint8_t> bytes;
    if (!allow::Sign(list, secretKey, bytes)) {
        Fail("signing refused the list");
        return 1;
    }
    crypto_wipe(secretKey.data(), secretKey.size());

    // The bytes must verify under the public key derived from the same seed, so a bad seed never ships.
    allow::List check;
    if (allow::Decode(bytes, publicKey, check) != allow::Verdict::Ok) {
        Fail("the signed bytes do not verify under the derived public key");
        return 1;
    }
    if (!WriteBytes(outPath, bytes)) {
        Fail("could not write " + outPath);
        return 1;
    }
    std::printf("signed sequence %llu: current %s, %zu allowed, %zu yanked, %zu bytes\n",
                static_cast<unsigned long long>(list.sequence), list.current.c_str(), list.allowed.size(),
                list.yanked.size(), bytes.size());
    std::printf("public key: %s\n", ToHex(publicKey.data(), publicKey.size()).c_str());
    return 0;
}

int CommandVerify(const std::string& publicHex, const std::string& inPath) {
    allow::PublicKey publicKey{};
    if (!FromHex(publicHex, publicKey.data(), publicKey.size())) {
        Fail("the public key is not 64 hex digits");
        return 2;
    }
    std::vector<uint8_t> bytes;
    if (!ReadBytes(inPath, bytes)) {
        Fail("could not read " + inPath);
        return 2;
    }
    allow::List list;
    const allow::Verdict verdict = allow::Decode(bytes, publicKey, list);
    if (verdict != allow::Verdict::Ok) {
        std::printf("verify: %s\n", allow::VerdictName(verdict));
        return 1;
    }
    std::printf("verify: ok\n");
    std::printf("protocol %u\n", static_cast<unsigned>(list.protocol));
    std::printf("sequence %llu\n", static_cast<unsigned long long>(list.sequence));
    std::printf("issued %llu\n", static_cast<unsigned long long>(list.issued));
    std::printf("grace_until %llu\n", static_cast<unsigned long long>(list.graceUntil));
    std::printf("current %s\n", list.current.c_str());
    for (const std::string& tag : list.allowed) {
        std::printf("allow %s\n", tag.c_str());
    }
    for (const allow::Yank& yank : list.yanked) {
        std::printf("yank %s %s\n", yank.tag.c_str(), yank.reason.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        args.emplace_back(argv[i]);
    }
    if (args.size() == 3 && args[0] == "keygen") {
        return CommandKeygen(args[1], args[2]);
    }
    if (args.size() == 4 && args[0] == "sign") {
        return CommandSign(false, args[1], args[2], args[3]);
    }
    if (args.size() == 5 && args[0] == "sign" && args[1] == "--secret-env") {
        return CommandSign(true, args[2], args[3], args[4]);
    }
    if (args.size() == 3 && args[0] == "verify") {
        return CommandVerify(args[1], args[2]);
    }
    Usage();
    return 2;
}
