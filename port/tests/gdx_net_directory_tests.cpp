// port/tests/gdx_net_directory_tests.cpp: room directory client tests: reader, rules, failover, fuzz (G-NET O5.5).
// Copyright (c) Zorkats

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gdx_sha256.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "net/gdx_net_directory.h"

#define CHECK(cond) Check((cond), std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond)

namespace {

namespace dir = gdx::net::directory;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) {
        gFailures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// The CONTRACT.md §9 values. The host key is the example key of §9: RFC 8032 section 7.1, TEST 1.
const std::string kHostSeed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const std::string kRegisterSig =
    "1d41386803afce3fef18587ea7fe65aa1873f5f483d51a920c609e8da61d8bf5be9fdd984b0dea7af90dae7678e673a63a9204ab4984b1ae1930f1de5d6b6e0b";
const std::string kDelistSig =
    "d99b94937488ff15dc91a27f9c28c84a54ccb9ce06c6ad86a845bf51b2ca3765fc0f3c1715d7333de0e1057e0726e8b1fcaeb087a2f5b66f435107ab558e2e0f";
const std::string kHostKey = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const std::string kZeroHex(64, '0');
const std::string kZeroHash = "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925";
const std::string kBaseA = "https://a.example";
const std::string kBaseB = "https://b.example";

// The draft keys in encoding order, for the example draft of §9.
const std::string kDraftBytes =
    "\"type\":\"open\",\"humans\":2,\"freeSeats\":4,\"reachable\":true,\"protocol\":3,"
    "\"hostKey\":\"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a\","
    "\"endpoints\":[{\"family\":\"v4\",\"addr\":\"203.0.113.7\",\"port\":24302,\"how\":\"upnp\"}]}";

// §9 register request, byte for byte.
const std::string kRegisterBytes =
    "{\"code\":\"ABC234\",\"takeoverHash\":\"66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925\","
    "\"type\":\"open\",\"humans\":2,\"freeSeats\":4,\"reachable\":true,\"protocol\":3,"
    "\"hostKey\":\"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a\","
    "\"endpoints\":[{\"family\":\"v4\",\"addr\":\"203.0.113.7\",\"port\":24302,\"how\":\"upnp\"}]}";

// §9 record, 42 seconds into its life: ttl 138.
const std::string kRoomObject =
    "{\"code\":\"ABC234\",\"type\":\"open\",\"humans\":2,\"freeSeats\":4,\"reachable\":true,\"protocol\":3,"
    "\"hostKey\":\"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a\","
    "\"endpoints\":[{\"family\":\"v4\",\"addr\":\"203.0.113.7\",\"port\":24302,\"how\":\"upnp\"}],\"ttl\":138}";

dir::Draft ExampleDraft() {
    dir::Draft draft;
    draft.type = dir::RoomType::Open;
    draft.humans = 2;
    draft.freeSeats = 4;
    draft.reachable = true;
    draft.protocol = 3;
    draft.hostKey = kHostKey;
    dir::Endpoint endpoint;
    endpoint.family = dir::Family::V4;
    endpoint.addr = "203.0.113.7";
    endpoint.port = 24302;
    endpoint.how = dir::How::Upnp;
    draft.endpoints.push_back(endpoint);
    return draft;
}

dir::RegisterRequest RegisterSample() {
    dir::RegisterRequest request;
    request.code = "ABC234";
    request.takeoverHash = kZeroHash;
    request.draft = ExampleDraft();
    return request;
}

dir::HeartbeatRequest HeartbeatSample() {
    dir::HeartbeatRequest request;
    request.code = "ABC234";
    request.draft = ExampleDraft();
    return request;
}

dir::TakeoverRequest TakeoverSample() {
    dir::TakeoverRequest request;
    request.code = "ABC234";
    request.nextHash = kZeroHash;
    request.draft = ExampleDraft();
    return request;
}

// Returns text with the first occurrence of from replaced by to. The occurrence must exist, or the test is wrong.
std::string With(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// A response with an answer from the directory, as the transport reports it.
dir::Response Json(int status, std::string body, std::string retryAfter = "") {
    dir::Response response;
    response.ok = true;
    response.status = status;
    response.body = std::move(body);
    response.retryAfter = std::move(retryAfter);
    return response;
}

// No HTTP answer at all: connect, TLS or timeout.
dir::Response Down() {
    return dir::Response{};
}

dir::Response Refusal(int status, const std::string& token, const std::string& retryAfter = "") {
    return Json(status, "{\"error\":\"" + token + "\"}", retryAfter);
}

// A transport that answers from a script per base URL. Each base has its own queue. A base with an empty queue
// answers like a dead host, so a test can see every call the client makes.
class Stub {
public:
    void Script(const std::string& base, dir::Response response) {
        queues_[base].push_back(std::move(response));
    }

    dir::Transport MakeTransport() {
        return [this](const dir::Request& request) -> dir::Response {
            seen.push_back(request);
            const std::size_t cut = request.url.rfind("/v1/");
            const std::string base = cut == std::string::npos ? request.url : request.url.substr(0, cut);
            const auto found = queues_.find(base);
            if (found == queues_.end() || found->second.empty()) {
                return Down();
            }
            dir::Response response = found->second.front();
            found->second.pop_front();
            return response;
        };
    }

    std::vector<dir::Request> seen;

private:
    std::map<std::string, std::deque<dir::Response>> queues_;
};

std::array<uint8_t, 32> BytesOf32(const std::string& hex) {
    std::array<uint8_t, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    }
    return out;
}

std::string HexText(const uint8_t* bytes, std::size_t count) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] & 0x0F)]);
    }
    return out;
}

// Signs as the host whose Ed25519 seed is seedHex, through Monocypher, as the game does (CONTRACT.md §8a).
dir::Signer SeedSigner(const std::string& seedHex) {
    return [seedHex](std::string_view message, dir::Signature& out) {
        std::array<uint8_t, 32> seed = BytesOf32(seedHex);
        uint8_t secretKey[64];
        uint8_t publicKey[32];
        crypto_ed25519_key_pair(secretKey, publicKey, seed.data());
        crypto_ed25519_sign(out.data(), secretKey, reinterpret_cast<const uint8_t*>(message.data()), message.size());
        crypto_wipe(secretKey, sizeof secretKey);
        return true;
    };
}

dir::Client ClientOver(Stub& stub, std::vector<std::string> bases) {
    dir::Client client(std::move(bases), stub.MakeTransport());
    client.SetSigner(SeedSigner(kHostSeed));
    return client;
}

// A signed body with its sig taken off, so a test can compare the rest byte for byte. Empty when the body does not
// end with a sig member.
std::string Unsigned(const std::string& body) {
    constexpr std::size_t kTail = 8 + 128 + 2;  // ,"sig":" then 128 hex then "}
    if (body.size() < kTail + 1 || body.compare(body.size() - kTail, 8, ",\"sig\":\"") != 0) {
        return "";
    }
    return body.substr(0, body.size() - kTail) + "}";
}

// True when the request's sig is the host key's signature over its path and the rest of its body.
bool SignedByHost(const dir::Request& request, const std::string& path) {
    const std::string rest = Unsigned(request.body);
    if (rest.empty()) {
        return false;
    }
    const std::string sigHex = request.body.substr(request.body.size() - 130, 128);
    uint8_t signature[64];
    for (std::size_t i = 0; i < 64; ++i) {
        signature[i] = static_cast<uint8_t>(std::stoul(sigHex.substr(2 * i, 2), nullptr, 16));
    }
    const std::array<uint8_t, 32> key = BytesOf32(kHostKey);
    const std::string message = dir::SigningMessage(path, rest);
    return crypto_ed25519_check(signature, key.data(), reinterpret_cast<const uint8_t*>(message.data()),
                                message.size()) == 0;
}

// The request the client sent as the i-th call, or an empty request when it sent fewer.
dir::Request Nth(const Stub& stub, std::size_t i) {
    return i < stub.seen.size() ? stub.seen[i] : dir::Request{};
}

std::string Sha256Hex(const char* data, std::size_t length) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    gdx::hash::Sha256 state{};
    gdx::hash::sha256_init(&state);
    gdx::hash::sha256_update(&state, data, length);
    unsigned char digest[32];
    gdx::hash::sha256_final(&state, digest);
    std::string out;
    for (unsigned char byte : digest) {
        out.push_back(kDigits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(byte & 0x0F)]);
    }
    return out;
}

// Random helpers. Every fuzz loop takes a fixed seed, so a failure reproduces.

std::size_t Below(std::mt19937_64& rng, std::size_t n) {
    return static_cast<std::size_t>(rng() % n);
}

std::string RandomText(std::mt19937_64& rng, std::size_t maxLength) {
    std::string out;
    const std::size_t length = Below(rng, maxLength + 1);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(static_cast<char>(0x20 + Below(rng, 0x5F)));
    }
    return out;
}

// Writes a string with the two escapes the grammar allows (CONTRACT.md §2).
void AppendString(const std::string& text, std::string& out) {
    out.push_back('"');
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
}

// Adds random whitespace of the four kinds the grammar allows, when rng is given. Otherwise adds nothing.
void Gap(std::mt19937_64* rng, std::string& out) {
    if (rng == nullptr) {
        return;
    }
    constexpr char kSpace[] = " \t\r\n";
    const std::size_t count = Below(*rng, 3);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kSpace[Below(*rng, 4)]);
    }
}

void WriteValue(const dir::JsonValue& value, std::mt19937_64* rng, std::string& out) {
    using Kind = dir::JsonValue::Kind;
    switch (value.kind) {
        case Kind::Null:
            out += "null";
            return;
        case Kind::Bool:
            out += value.boolean ? "true" : "false";
            return;
        case Kind::Number:
            out += std::to_string(value.number);
            return;
        case Kind::String:
            AppendString(value.text, out);
            return;
        case Kind::Array:
            out.push_back('[');
            Gap(rng, out);
            for (std::size_t i = 0; i < value.items.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                    Gap(rng, out);
                }
                WriteValue(value.items[i], rng, out);
                Gap(rng, out);
            }
            out.push_back(']');
            return;
        case Kind::Object:
            out.push_back('{');
            Gap(rng, out);
            for (std::size_t i = 0; i < value.keys.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                    Gap(rng, out);
                }
                AppendString(value.keys[i], out);
                Gap(rng, out);
                out.push_back(':');
                Gap(rng, out);
                WriteValue(value.values[i], rng, out);
                Gap(rng, out);
            }
            out.push_back('}');
            return;
    }
}

// The one spelling a parsed value has: no whitespace, members in parse order.
std::string Canonical(const dir::JsonValue& value) {
    std::string out;
    WriteValue(value, nullptr, out);
    return out;
}

dir::JsonValue RandomScalar(std::mt19937_64& rng) {
    dir::JsonValue value;
    switch (Below(rng, 4)) {
        case 0:
            value.kind = dir::JsonValue::Kind::Null;
            break;
        case 1:
            value.kind = dir::JsonValue::Kind::Bool;
            value.boolean = Below(rng, 2) == 1;
            break;
        case 2:
            value.kind = dir::JsonValue::Kind::Number;
            value.number = rng() % 9007199254740992ull;  // 0 to 2^53 - 1, the whole accepted range
            break;
        default:
            value.kind = dir::JsonValue::Kind::String;
            value.text = RandomText(rng, 8);
            break;
    }
    return value;
}

dir::JsonValue RandomValue(std::mt19937_64& rng, std::size_t depth) {
    dir::JsonValue value;
    const std::size_t roll = Below(rng, 10);
    if (depth >= 4 || roll < 5) {
        return RandomScalar(rng);
    }
    const std::size_t count = Below(rng, 4);
    if (roll < 7) {
        value.kind = dir::JsonValue::Kind::Array;
        for (std::size_t i = 0; i < count; ++i) {
            value.items.push_back(RandomValue(rng, depth + 1));
        }
        return value;
    }
    value.kind = dir::JsonValue::Kind::Object;
    for (std::size_t i = 0; i < count; ++i) {
        // The index prefix keeps every key distinct, as the reader requires.
        value.keys.push_back("k" + std::to_string(i) + RandomText(rng, 4));
        value.values.push_back(RandomValue(rng, depth + 1));
    }
    return value;
}

std::string Nest(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

std::string NestObjects(std::size_t depth) {
    std::string text = "0";
    for (std::size_t i = 0; i < depth; ++i) {
        text = "{\"a\":" + text + "}";
    }
    return text;
}

std::vector<std::string> SplitOn(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

// ---------------------------------------------------------------------------------------------------------------------

void TestConstants() {
    CHECK(dir::kBodyMax == 16384);
    CHECK(dir::kTtlSeconds == 180);
    CHECK(dir::kListMax == 10);
    CHECK(dir::kEndpointsMax == 8);
    CHECK(dir::kHumansMax == 12);
    CHECK(dir::kJsonDepthMax == 8);
    CHECK(dir::kJsonIntegerMax == 9007199254740991ull);
}

void TestErrorNames() {
    CHECK(std::string(dir::ErrorName(dir::Error::None)) == "none");
    CHECK(std::string(dir::ErrorName(dir::Error::BadConfig)) == "bad_config");
    CHECK(std::string(dir::ErrorName(dir::Error::CodeTaken)) == "code_taken");
    CHECK(std::string(dir::ErrorName(dir::Error::RateLimited)) == "rate_limited");
    CHECK(std::string(dir::ErrorName(dir::Error::Internal)) == "internal");
    CHECK(std::string(dir::ErrorName(dir::Error::BadSignature)) == "bad_signature");
    CHECK(std::string(dir::ErrorName(dir::Error::TurnUpstream)) == "turn_upstream");
    CHECK(std::string(dir::ErrorName(dir::Error::TurnUnavailable)) == "turn_unavailable");
}

// CONTRACT.md §3, code.
void TestCodes() {
    for (const char* text : {"ABC234", "ZZZZZZ", "0000H0"}) {
        CHECK(dir::IsCode(text));
    }
    for (const char* text : {"abc234", "ABC23I", "ABC23L", "ABC23O", "ABC23U", "ABC23", "ABC2345", "ABC 23", "",
                             "ABC23\xC3"}) {
        CHECK(!dir::IsCode(text));
    }
}

// CONTRACT.md §3, host key.
void TestHostKeys() {
    CHECK(dir::IsHostKey(kHostKey));
    CHECK(!dir::IsHostKey(kHostKey + "0"));
    CHECK(!dir::IsHostKey(kHostKey.substr(1)));
    CHECK(!dir::IsHostKey("A" + kHostKey.substr(1)));
    CHECK(!dir::IsHostKey("g" + kHostKey.substr(1)));
    CHECK(!dir::IsHostKey(""));
}

// CONTRACT.md §3 and §9, addresses.
void TestAddresses() {
    for (const char* addr : {"0.0.0.1", "203.0.113.7", "255.255.255.255", "10.0.0.0"}) {
        CHECK(dir::IsIpv4(addr));
    }
    for (const char* addr : {"0.0.0.0", "01.2.3.4", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.", "1.2.3.04", "",
                             "1..2.3", "-1.2.3.4", "1.2.3.4 ", "1.2.3.-4", "1.2.3.0256"}) {
        CHECK(!dir::IsIpv4(addr));
    }
    for (const char* addr : {"::1", "2001:db8::1", "2001:db8:0:0:0:0:0:1", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8",
                             "1111:2222:3333:4444:5555:6666:7777:8888"}) {
        CHECK(dir::IsIpv6(addr));
    }
    for (const char* addr : {"::", "0:0:0:0:0:0:0:0", "1::2::3", ":1::2", "1:::2", "2001:DB8::1", "1:2:3:4:5:6:7:8:9",
                             "1:2:3:4:5:6:7:8::", "12345::1", "fe80::1%eth0", "::ffff:1.2.3.4", "1:2:3:4:5:6:7",
                             "1:2:3:4:5:6:7::8", "1:", "g::1", "", "1", "::0", "0::"}) {
        CHECK(!dir::IsIpv6(addr));
    }
}

// CONTRACT.md §3, the draft: each rule is broken once.
void TestDraftRules() {
    const dir::Draft good = ExampleDraft();
    CHECK(dir::DraftValid(good));

    dir::Draft draft = good;
    draft.humans = 13;
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.humans = 7;
    draft.freeSeats = 6;
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.humans = 6;
    draft.freeSeats = 6;
    CHECK(dir::DraftValid(draft));  // 12 is allowed

    draft = good;
    draft.protocol = 0;
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.hostKey = "XYZ";
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.endpoints.clear();
    CHECK(!dir::DraftValid(draft));

    draft = good;
    while (draft.endpoints.size() < 8) {
        draft.endpoints.push_back(good.endpoints[0]);
    }
    CHECK(dir::DraftValid(draft));  // 8 endpoints is the limit
    draft.endpoints.push_back(good.endpoints[0]);
    CHECK(!dir::DraftValid(draft));  // 9 is refused

    draft = good;
    draft.endpoints[0].family = dir::Family::V6;  // the address is still IPv4
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.endpoints[0].port = 0;
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.endpoints[0].addr = "0.0.0.0";
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.endpoints[0].how = static_cast<dir::How>(9);
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.endpoints[0].family = static_cast<dir::Family>(5);
    CHECK(!dir::DraftValid(draft));

    draft = good;
    draft.type = static_cast<dir::RoomType>(7);
    CHECK(!dir::DraftValid(draft));
}

struct Case {
    std::string text;
    std::string canonical;
};

// CONTRACT.md §2: the accept and refuse lists, the same bytes as test/json.test.ts.
void TestStrictJson() {
    const std::vector<Case> accepted = {
        {"0", "0"},
        {"7", "7"},
        {"9007199254740991", "9007199254740991"},
        {"true", "true"},
        {"false", "false"},
        {"null", "null"},
        {"\"\"", "\"\""},
        {"\"ABC234\"", "\"ABC234\""},
        {"\"a\\\"b\"", "\"a\\\"b\""},
        {"\"a\\\\b\"", "\"a\\\\b\""},
        {"\" ~\"", "\" ~\""},
        {"[]", "[]"},
        {"{}", "{}"},
        {"{\"\":1}", "{\"\":1}"},
        {"{\"a\\\"b\":1}", "{\"a\\\"b\":1}"},
        {"{\"a\":[1,{\"b\":null}]}", "{\"a\":[1,{\"b\":null}]}"},
        {" \t\r\n{ \"k\" : [ 0 , 1 ] } \n", "{\"k\":[0,1]}"},
        {Nest(8), Nest(8)},
        {NestObjects(8), NestObjects(8)},
    };
    for (const Case& c : accepted) {
        dir::JsonValue value;
        CHECK(dir::ParseStrictJson(c.text, value));
        if (value.kind != dir::JsonValue::Kind::Null || c.text == "null") {
            CHECK(Canonical(value) == c.canonical);
        }
    }

    const std::vector<std::string> refused = {
        "",
        " ",
        "-1",
        "+1",
        "01",
        "-0",
        "1.0",
        "0.5",
        "1e3",
        "1E3",
        "00",
        "9007199254740992",
        "99999999999999999999999",
        "18446744073709551616",
        "\"\\/\"",
        "\"\\n\"",
        "\"\\u0041\"",
        "\"\\t\"",
        "\"\\b\"",
        "\"caf\xC3\xA9\"",
        "\"\xC2\x80\"",
        "\"\x7F\"",
        "\"tab\there\"",
        "{\"a\":1,\"a\":2}",
        "{\"a\":1,\"b\":2,\"a\":3}",
        "{} x",
        std::string("{}\0", 3),
        "{}\f",
        "[1 2]",
        "[1,]",
        "{,}",
        "{\"a\" 1}",
        "{\"a\":}",
        "{1:2}",
        "\"abc",
        "tru",
        "nul",
        "fals",
        "[",
        "{",
        "]",
        "\xEF\xBB\xBF{}",
        "\xEF\xBB\xBF" "0",
        Nest(9),
        "{\"a\":" + Nest(8) + "}",
        NestObjects(9),
    };
    for (const std::string& text : refused) {
        dir::JsonValue value;
        CHECK(!dir::ParseStrictJson(text, value));
    }

    dir::JsonValue value;
    CHECK(dir::ParseStrictJson("9007199254740991", value) && value.number == 9007199254740991ull);
    CHECK(!dir::ParseStrictJson("9007199254740992", value));
    CHECK(dir::ParseStrictJson("[ ]\r\n", value));
    CHECK(dir::ParseStrictJson("\t\n[]", value));

    CHECK(dir::ParseStrictJson("{\"a\":1,\"b\":true}", value));
    CHECK(value.Find("a") != nullptr && value.Find("a")->number == 1);
    CHECK(value.Find("b") != nullptr && value.Find("b")->boolean);
    CHECK(value.Find("c") == nullptr);
    dir::JsonValue array;
    CHECK(dir::ParseStrictJson("[1]", array));
    CHECK(array.Find("a") == nullptr);

    // A prefix of a valid object is never a valid document, so every prefix is refused.
    for (std::size_t n = 0; n < kRegisterBytes.size(); ++n) {
        dir::JsonValue prefix;
        CHECK(!dir::ParseStrictJson(kRegisterBytes.substr(0, n), prefix));
    }

    // A refused document leaves the output alone.
    dir::JsonValue keep;
    keep.kind = dir::JsonValue::Kind::Number;
    keep.number = 77;
    CHECK(!dir::ParseStrictJson("01", keep));
    CHECK(keep.kind == dir::JsonValue::Kind::Number && keep.number == 77);
}

// CONTRACT.md §3 and §8: the takeover secret and its hash.
void TestSecrets() {
    dir::Secret zero{};
    CHECK(dir::SecretHex(zero) == kZeroHex);
    CHECK(dir::TakeoverHashOf(zero) == kZeroHash);
    // The hash is over the 32 raw bytes, not over the 64 characters of hex text.
    CHECK(dir::TakeoverHashOf(zero) != Sha256Hex(kZeroHex.data(), kZeroHex.size()));

    dir::Secret sample{};
    for (std::size_t i = 0; i < sample.size(); ++i) {
        sample[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    const std::string hex = dir::SecretHex(sample);
    CHECK(hex.size() == 64);
    dir::Secret back{};
    CHECK(dir::ParseSecretHex(hex, back));
    CHECK(back == sample);

    std::string upper = hex;
    upper[0] = 'F';
    dir::Secret keep = sample;
    dir::Secret other{};
    other[0] = 0xEE;
    keep = other;
    CHECK(!dir::ParseSecretHex(upper, keep));
    CHECK(keep == other);
    CHECK(!dir::ParseSecretHex(hex.substr(1), keep));
    CHECK(!dir::ParseSecretHex(hex + "0", keep));
    CHECK(!dir::ParseSecretHex("zz", keep));
    CHECK(keep == other);
}

// SHA-256 vectors: CONTRACT.md §9, and a message whose length lands on the padding boundary.
void TestSha256Vectors() {
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex(kZeroHex.data(), 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex(std::string(32, '\0').data(), 32) == kZeroHash);
    const std::string boundary = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(boundary.size() == 56);
    CHECK(Sha256Hex(boundary.data(), boundary.size()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// CONTRACT.md §4, §5 and §9: the bytes each request encodes to, and refusal of what the directory would refuse.
void TestEncoders() {
    std::string body;
    CHECK(dir::EncodeRegister(RegisterSample(), body));
    CHECK(body == kRegisterBytes);

    CHECK(dir::EncodeHeartbeat(HeartbeatSample(), body));
    CHECK(body == "{\"secret\":\"" + kZeroHex + "\"," + kDraftBytes);

    CHECK(dir::EncodeTakeover(TakeoverSample(), body));
    CHECK(body == "{\"secret\":\"" + kZeroHex + "\",\"nextHash\":\"" + kZeroHash + "\"," + kDraftBytes);

    // Every encoded body is a document the strict reader accepts, with the same bytes back.
    dir::JsonValue value;
    CHECK(dir::ParseStrictJson(body, value));
    CHECK(Canonical(value) == body);

    std::string keep = "keep";
    dir::RegisterRequest request = RegisterSample();
    request.code = "abc234";
    CHECK(!dir::EncodeRegister(request, keep));
    request = RegisterSample();
    request.takeoverHash = "66687AADF862BD776C8FC18B8E9F8E20089714856EE233B3902A591D0D5F2925";
    CHECK(!dir::EncodeRegister(request, keep));
    request = RegisterSample();
    request.draft.humans = 13;
    CHECK(!dir::EncodeRegister(request, keep));
    CHECK(keep == "keep");

    dir::HeartbeatRequest heartbeat = HeartbeatSample();
    heartbeat.code = "ABC23I";
    CHECK(!dir::EncodeHeartbeat(heartbeat, keep));
    dir::TakeoverRequest takeover = TakeoverSample();
    takeover.nextHash = "short";
    CHECK(!dir::EncodeTakeover(takeover, keep));
    CHECK(keep == "keep");
}

// The canonical accept and refuse rules are the same on both sides. This checks the parts a parser can get wrong
// quietly: a string that is not printable ASCII, and a number at the edge.
void TestRecordStructure() {
    dir::JsonValue room;
    CHECK(dir::ParseStrictJson(kRoomObject, room));
    CHECK(room.Find("ttl") != nullptr && room.Find("ttl")->number == 138);
    CHECK(room.Find("takeoverHash") == nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// Client: configuration, input checks and the answers.

void TestClientConfig() {
    const std::vector<std::vector<std::string>> bad = {
        {},
        {"http://a.example"},
        {"https://a.example/"},
        {"https://"},
        {"https://a example"},
        {kBaseA, "http://b.example"},
    };
    for (const std::vector<std::string>& bases : bad) {
        Stub stub;
        const dir::Client client = ClientOver(stub, bases);
        CHECK(client.Lookup("ABC234").error == dir::Error::BadConfig);
        CHECK(stub.seen.empty());
    }

    const dir::Client noTransport({kBaseA}, dir::Transport{});
    CHECK(noTransport.Lookup("ABC234").error == dir::Error::BadConfig);

    Stub stub;
    const dir::Client dead = ClientOver(stub, {kBaseA});
    CHECK(dead.Lookup("ABC234").error == dir::Error::Unreachable);
    CHECK(stub.seen.size() == 1);
}

void TestInvalidInputIsNotSent() {
    Stub stub;
    const dir::Client client = ClientOver(stub, {kBaseA});
    CHECK(client.Lookup("abc234").error == dir::Error::Invalid);
    CHECK(client.OpenRooms(0).error == dir::Error::Invalid);
    CHECK(client.QuickRace(0).error == dir::Error::Invalid);
    CHECK(client.HostRooms("ABC", 3).error == dir::Error::Invalid);
    CHECK(client.HostRooms(kHostKey, 0).error == dir::Error::Invalid);

    dir::RegisterRequest registration = RegisterSample();
    registration.draft.humans = 13;
    CHECK(client.Register(registration).error == dir::Error::Invalid);

    dir::HeartbeatRequest heartbeat = HeartbeatSample();
    heartbeat.code = "ABC23I";
    CHECK(client.Heartbeat(heartbeat).error == dir::Error::Invalid);

    dir::TakeoverRequest takeover = TakeoverSample();
    takeover.nextHash = "x";
    CHECK(client.Takeover(takeover).error == dir::Error::Invalid);

    CHECK(stub.seen.empty());
}

void TestRegisterAnswers() {
    {
        Stub stub;
        stub.Script(kBaseA, Json(201, "{\"ttl\":180}"));
        const auto answer = ClientOver(stub, {kBaseA}).Register(RegisterSample());
        CHECK(answer.error == dir::Error::None);
        CHECK(answer.value == 180);
        CHECK(stub.seen.size() == 1);
        CHECK(Nth(stub, 0).method == "POST");
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/rooms");
        CHECK(Nth(stub, 0).contentType == "application/json");
        CHECK(Nth(stub, 0).body == kRegisterBytes.substr(0, kRegisterBytes.size() - 1) + ",\"sig\":\"" +
                                       kRegisterSig + "\"}");
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(409, "code_taken"));
        const auto answer = ClientOver(stub, {kBaseA}).Register(RegisterSample());
        CHECK(answer.error == dir::Error::CodeTaken);
        CHECK(answer.value == 0);
    }
    // A 2xx that is not the contract's exact shape is Malformed.
    const std::vector<dir::Response> wrong = {
        Json(201, "{\"ttl\":179}"),
        Json(200, "{\"ttl\":180}"),
        Json(201, "{\"ttl\":180,}"),
        Json(201, "{\"ttl\":180,\"x\":1}"),
        Json(201, "{\"ttl\":\"180\"}"),
        Json(201, "{}"),
    };
    for (const dir::Response& response : wrong) {
        Stub stub;
        stub.Script(kBaseA, response);
        CHECK(ClientOver(stub, {kBaseA}).Register(RegisterSample()).error == dir::Error::Malformed);
    }
    // The body cap: a valid body at exactly 16384 bytes is read, and one byte more is refused.
    const std::string head = "{\"ttl\":180";
    const std::string atCap = head + std::string(dir::kBodyMax - head.size() - 1, ' ') + "}";
    CHECK(atCap.size() == dir::kBodyMax);
    {
        Stub stub;
        stub.Script(kBaseA, Json(201, atCap));
        CHECK(ClientOver(stub, {kBaseA}).Register(RegisterSample()).error == dir::Error::None);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(201, atCap + " "));
        CHECK(ClientOver(stub, {kBaseA}).Register(RegisterSample()).error == dir::Error::Malformed);
    }
}

void TestHeartbeatAndTakeoverAnswers() {
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"ttl\":180}"));
        const auto answer = ClientOver(stub, {kBaseA}).Heartbeat(HeartbeatSample());
        CHECK(answer.error == dir::Error::None && answer.value == 180);
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/rooms/ABC234/heartbeat");
        CHECK(Unsigned(Nth(stub, 0).body) == "{\"secret\":\"" + kZeroHex + "\"," + kDraftBytes);
        CHECK(SignedByHost(Nth(stub, 0), "/v1/rooms/ABC234/heartbeat"));
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(403, "bad_secret"));
        CHECK(ClientOver(stub, {kBaseA}).Heartbeat(HeartbeatSample()).error == dir::Error::BadSecret);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(404, "not_found"));
        CHECK(ClientOver(stub, {kBaseA}).Heartbeat(HeartbeatSample()).error == dir::Error::NotFound);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"ttl\":180}"));
        const auto answer = ClientOver(stub, {kBaseA}).Takeover(TakeoverSample());
        CHECK(answer.error == dir::Error::None && answer.value == 180);
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/rooms/ABC234/takeover");
        CHECK(SignedByHost(Nth(stub, 0), "/v1/rooms/ABC234/takeover"));
        CHECK(Nth(stub, 0).body.find("\"nextHash\":\"" + kZeroHash + "\"") != std::string::npos);
    }
    // A token under the wrong status is a contract breach, not a directory answer.
    {
        Stub stub;
        stub.Script(kBaseA, Json(404, "{\"error\":\"bad_secret\"}"));
        CHECK(ClientOver(stub, {kBaseA}).Heartbeat(HeartbeatSample()).error == dir::Error::Malformed);
    }
}

// Signed writes and delist (CONTRACT.md §5.8, §8a, §9). Monocypher's Ed25519 is RFC 8032 and deterministic, so the
// game's signature over a §9 body is the contract's vector, byte for byte.
void TestSigning() {
    const std::string registerMessage = dir::SigningMessage("/v1/rooms", kRegisterBytes);
    CHECK(registerMessage == "G-NET directory v1\nPOST /v1/rooms\n" + kRegisterBytes);
    dir::Signature signature{};
    CHECK(SeedSigner(kHostSeed)(registerMessage, signature));
    CHECK(HexText(signature.data(), signature.size()) == kRegisterSig);

    std::string delistBody;
    dir::DelistRequest delist;
    delist.code = "ABC234";
    CHECK(dir::EncodeDelist(delist, delistBody));
    CHECK(delistBody == "{\"secret\":\"" + kZeroHex + "\"}");
    CHECK(SeedSigner(kHostSeed)(dir::SigningMessage("/v1/rooms/ABC234/delist", delistBody), signature));
    CHECK(HexText(signature.data(), signature.size()) == kDelistSig);

    std::string keep = "keep";
    delist.code = "abc234";
    CHECK(!dir::EncodeDelist(delist, keep));
    CHECK(keep == "keep");

    std::string notBody = "x";
    CHECK(!dir::AppendSignature(notBody, signature));
    CHECK(notBody == "x");
    std::string empty;
    CHECK(!dir::AppendSignature(empty, signature));

    // The delist call: its route, its exact body and its answer.
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"ttl\":0}"));
        dir::DelistRequest request;
        request.code = "ABC234";
        const auto answer = ClientOver(stub, {kBaseA}).Delist(request);
        CHECK(answer.error == dir::Error::None && answer.value == 0);
        CHECK(Nth(stub, 0).method == "POST");
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/rooms/ABC234/delist");
        CHECK(Nth(stub, 0).contentType == "application/json");
        CHECK(Nth(stub, 0).body == "{\"secret\":\"" + kZeroHex + "\",\"sig\":\"" + kDelistSig + "\"}");
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"ttl\":180}"));  // a delist answers 0, nothing else
        dir::DelistRequest request;
        request.code = "ABC234";
        CHECK(ClientOver(stub, {kBaseA}).Delist(request).error == dir::Error::Malformed);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(403, "bad_signature"));
        dir::DelistRequest request;
        request.code = "ABC234";
        CHECK(ClientOver(stub, {kBaseA}).Delist(request).error == dir::Error::BadSignature);
    }

    // A write with no signer, or a signer that refuses, sends nothing. A read needs no signer.
    {
        Stub stub;
        const dir::Client unsigned_(std::vector<std::string>{kBaseA}, stub.MakeTransport());
        CHECK(unsigned_.Register(RegisterSample()).error == dir::Error::Invalid);
        CHECK(unsigned_.Heartbeat(HeartbeatSample()).error == dir::Error::Invalid);
        CHECK(unsigned_.Takeover(TakeoverSample()).error == dir::Error::Invalid);
        dir::DelistRequest request;
        request.code = "ABC234";
        CHECK(unsigned_.Delist(request).error == dir::Error::Invalid);
        CHECK(stub.seen.empty());
        CHECK(unsigned_.Lookup("ABC234").error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        dir::Client refusing(std::vector<std::string>{kBaseA}, stub.MakeTransport());
        refusing.SetSigner([](std::string_view, dir::Signature&) { return false; });
        CHECK(refusing.Register(RegisterSample()).error == dir::Error::Invalid);
        CHECK(stub.seen.empty());
    }
}

// The record reader. Every rule of CONTRACT.md §3 is broken once.
dir::Error LookupRoomError(const std::string& room) {
    Stub stub;
    stub.Script(kBaseA, Json(200, "{\"room\":" + room + "}"));
    return ClientOver(stub, {kBaseA}).Lookup("ABC234").error;
}

void TestLookupAndRecords() {
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":" + kRoomObject + "}"));
        const auto answer = ClientOver(stub, {kBaseA}).Lookup("ABC234");
        CHECK(answer.error == dir::Error::None);
        CHECK(answer.value.has_value());
        if (answer.value.has_value()) {
            const dir::Record& record = *answer.value;
            CHECK(record.code == "ABC234");
            CHECK(record.ttl == 138);
            CHECK(record.draft.type == dir::RoomType::Open);
            CHECK(record.draft.humans == 2);
            CHECK(record.draft.freeSeats == 4);
            CHECK(record.draft.reachable);
            CHECK(record.draft.protocol == 3);
            CHECK(record.draft.hostKey == kHostKey);
            CHECK(record.draft.endpoints.size() == 1);
            CHECK(record.draft.endpoints[0].addr == "203.0.113.7");
            CHECK(record.draft.endpoints[0].port == 24302);
            CHECK(record.draft.endpoints[0].how == dir::How::Upnp);
        }
        CHECK(Nth(stub, 0).method == "GET");
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/rooms/ABC234");
        CHECK(Nth(stub, 0).contentType.empty());
        CHECK(Nth(stub, 0).body.empty());
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(404, "not_found"));
        CHECK(ClientOver(stub, {kBaseA}).Lookup("ABC234").error == dir::Error::NotFound);
    }
    CHECK(LookupRoomError("null") == dir::Error::Malformed);  // Lookup needs a room
    CHECK(LookupRoomError(With(kRoomObject, "\"ttl\":138}", "\"ttl\":138,\"takeoverHash\":\"" + kZeroHash + "\"}")) ==
          dir::Error::Malformed);  // the takeover hash never leaves the directory

    const std::vector<std::pair<std::string, std::string>> mutations = {
        {"\"ttl\":138", "\"ttl\":0"},
        {"\"ttl\":138", "\"ttl\":181"},
        {"\"ttl\":138", "\"ttl\":\"138\""},
        {"\"humans\":2,\"freeSeats\":4", "\"humans\":13,\"freeSeats\":0"},
        {"\"humans\":2,\"freeSeats\":4", "\"humans\":7,\"freeSeats\":6"},
        {"\"reachable\":true", "\"reachable\":\"true\""},
        {"\"protocol\":3", "\"protocol\":0"},
        {"\"protocol\":3", "\"protocol\":65536"},
        {"\"code\":\"ABC234\"", "\"code\":\"abc234\""},
        {"\"type\":\"open\"", "\"type\":\"lan\""},
        {"\"hostKey\":\"d75a980182b10ab7", "\"hostKey\":\"D75A980182B10AB7"},
        {"\"addr\":\"203.0.113.7\"", "\"addr\":\"203.0.113.07\""},
        {"\"family\":\"v4\"", "\"family\":\"v6\""},
        {"\"port\":24302", "\"port\":0"},
        {"\"how\":\"upnp\"", "\"how\":\"lan\""},
        {"\"endpoints\":[{\"family\":\"v4\",\"addr\":\"203.0.113.7\",\"port\":24302,\"how\":\"upnp\"}]",
         "\"endpoints\":[]"},
        {"\"ttl\":138}", "\"ttl\":138,\"ttl\":138}"},
        {"\"ttl\":138}", "\"ttl\":138"},
    };
    for (const auto& mutation : mutations) {
        CHECK(LookupRoomError(With(kRoomObject, mutation.first, mutation.second)) == dir::Error::Malformed);
    }
}

void TestListsAndQuick() {
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"rooms\":[]}"));
        const auto answer = ClientOver(stub, {kBaseA}).OpenRooms(3);
        CHECK(answer.error == dir::Error::None && answer.value.empty());
        CHECK(Nth(stub, 0).method == "GET");
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/open?protocol=3");
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"rooms\":[" + kRoomObject + "]}"));
        const auto answer = ClientOver(stub, {kBaseA}).OpenRooms(3);
        CHECK(answer.error == dir::Error::None);
        CHECK(answer.value.size() == 1 && answer.value[0].code == "ABC234");
    }
    {
        // Ten rooms fit the list cap, eleven do not.
        std::string ten = "{\"rooms\":[";
        std::string eleven = "{\"rooms\":[";
        for (std::size_t i = 0; i < 11; ++i) {
            if (i > 0) {
                eleven += ',';
            }
            if (i < 10) {
                ten += (i > 0 ? "," : "") + kRoomObject;
            }
            eleven += kRoomObject;
        }
        ten += "]}";
        eleven += "]}";
        Stub stub;
        stub.Script(kBaseA, Json(200, ten));
        CHECK(ClientOver(stub, {kBaseA}).OpenRooms(3).value.size() == 10);
        Stub over;
        over.Script(kBaseA, Json(200, eleven));
        CHECK(ClientOver(over, {kBaseA}).OpenRooms(3).error == dir::Error::Malformed);
    }
    for (const std::string& body : {std::string("{\"rooms\":null}"), std::string("{}"),
                                    std::string("{\"rooms\":[],\"x\":1}"), std::string("{\"rooms\":[null]}")}) {
        Stub stub;
        stub.Script(kBaseA, Json(200, body));
        CHECK(ClientOver(stub, {kBaseA}).OpenRooms(3).error == dir::Error::Malformed);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"rooms\":[]}"));
        const auto answer = ClientOver(stub, {kBaseA}).HostRooms(kHostKey, 3);
        CHECK(answer.error == dir::Error::None && answer.value.empty());
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/hosts/" + kHostKey + "/rooms?protocol=3");
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":null}"));
        const auto answer = ClientOver(stub, {kBaseA}).QuickRace(3);
        CHECK(answer.error == dir::Error::None && !answer.value.has_value());
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/quick?protocol=3");
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":" + kRoomObject + "}"));
        const auto answer = ClientOver(stub, {kBaseA}).QuickRace(3);
        CHECK(answer.error == dir::Error::None && answer.value.has_value());
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":[]}"));
        CHECK(ClientOver(stub, {kBaseA}).QuickRace(3).error == dir::Error::Malformed);
    }
}

// Every token of CONTRACT.md §6 with its status, and the same token under a status it does not carry.
void TestErrorTokens() {
    struct Row {
        const char* token;
        int status;
        dir::Error error;
    };
    const Row rows[] = {
        {"bad_json", 400, dir::Error::BadJson},
        {"bad_field", 400, dir::Error::BadField},
        {"bad_code", 400, dir::Error::BadCode},
        {"bad_host_key", 400, dir::Error::BadHostKey},
        {"bad_query", 400, dir::Error::BadQuery},
        {"unsupported_media", 415, dir::Error::UnsupportedMedia},
        {"too_large", 413, dir::Error::TooLarge},
        {"bad_secret", 403, dir::Error::BadSecret},
        {"bad_signature", 403, dir::Error::BadSignature},
        {"not_found", 404, dir::Error::NotFound},
        {"no_route", 404, dir::Error::NoRoute},
        {"method_not_allowed", 405, dir::Error::MethodNotAllowed},
        {"code_taken", 409, dir::Error::CodeTaken},
        {"rate_limited", 429, dir::Error::RateLimited},
        {"internal", 500, dir::Error::Internal},
    };
    for (const Row& row : rows) {
        Stub stub;
        dir::Response response = Refusal(row.status, row.token);
        response.retryAfter = "5";
        stub.Script(kBaseA, response);
        // A write, not a read: a read 5xx moves on to the next base, so only a write can report Internal
        // (CONTRACT.md §6).
        const auto answer = ClientOver(stub, {kBaseA}).Register(RegisterSample());
        CHECK(answer.error == row.error);
        CHECK(std::string(dir::ErrorName(answer.error)) == row.token);

        Stub wrong;
        wrong.Script(kBaseA, Refusal(row.status == 400 ? 401 : 400, row.token));
        CHECK(ClientOver(wrong, {kBaseA}).Register(RegisterSample()).error == dir::Error::Malformed);
    }
}

// Retry-After: whole seconds, nothing else (CONTRACT.md §6). A 429 without a usable header is a contract breach.
void TestRetryAfter() {
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited", "42"));
        const auto answer = ClientOver(stub, {kBaseA}).Register(RegisterSample());
        CHECK(answer.error == dir::Error::RateLimited);
        CHECK(answer.retryAfterSeconds == 42);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited", "999999999"));
        // Nine digits parse, and the wait is held to an hour so its milliseconds cannot wrap a deadline.
        CHECK(ClientOver(stub, {kBaseA}).Lookup("ABC234").retryAfterSeconds == 3600);
    }
    for (const char* header : {"", "4x", "-1", "1.5", " 42", "9999999999", "42 "}) {
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited", header));
        const auto answer = ClientOver(stub, {kBaseA}).Lookup("ABC234");
        CHECK(answer.error == dir::Error::Malformed);
        CHECK(answer.retryAfterSeconds == 0);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited"));
        CHECK(ClientOver(stub, {kBaseA}).Lookup("ABC234").error == dir::Error::Malformed);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Relay credentials (CONTRACT.md §5.9).

const std::string kGrant = "{\"username\":\"u-0f1e2d3c4b5a\",\"password\":\"p/9a8b+7c6d=\",\"ttl\":7200,"
                           "\"servers\":[\"turn.cloudflare.com:3478\",\"turn.cloudflare.com:443\"]}";

void TestTurnServers() {
    std::string host = "kept";
    uint16_t port = 7;
    CHECK(dir::SplitTurnServer("turn.cloudflare.com:3478", host, port) && host == "turn.cloudflare.com" && port == 3478);
    CHECK(dir::SplitTurnServer("198.51.100.1:65535", host, port) && host == "198.51.100.1" && port == 65535);
    CHECK(dir::SplitTurnServer("a:1", host, port) && host == "a" && port == 1);
    CHECK(dir::SplitTurnServer(std::string(253, 'a') + ":3478", host, port));
    host = "kept";
    port = 7;
    for (const std::string bad : {"", "turn.cloudflare.com", ":3478", "turn.cloudflare.com:", "turn.cloudflare.com:0",
                                  "turn.cloudflare.com:03478", "turn.cloudflare.com:65536", "turn.cloudflare.com:123456",
                                  "turn.cloudflare.com:34x8", ".bad:3478", "-bad:3478", "bad-:3478", "a b:3478",
                                  "[2001:db8::1]:3478", "2001:db8::1:3478", "under_score:3478"}) {
        CHECK(!dir::SplitTurnServer(bad, host, port));
    }
    CHECK(!dir::SplitTurnServer(std::string(254, 'a') + ":3478", host, port));
    CHECK(host == "kept" && port == 7);
}

void TestTurnCredentials() {
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, kGrant));
        const auto answer = ClientOver(stub, {kBaseA}).TurnCredentials();
        CHECK(answer.error == dir::Error::None);
        CHECK(answer.value.username == "u-0f1e2d3c4b5a");
        CHECK(answer.value.password == "p/9a8b+7c6d=");
        CHECK(answer.value.ttl == 7200);
        CHECK(answer.value.servers == std::vector<std::string>({"turn.cloudflare.com:3478", "turn.cloudflare.com:443"}));
        CHECK(stub.seen.size() == 1);
        CHECK(Nth(stub, 0).method == "POST");
        CHECK(Nth(stub, 0).url == kBaseA + "/v1/turn");
        CHECK(Nth(stub, 0).contentType == "application/json");
        CHECK(Nth(stub, 0).body == "{}"); // nothing is written, so nothing is signed
    }
    {
        // No signer is needed.
        Stub stub;
        stub.Script(kBaseA, Json(200, kGrant));
        const dir::Client unsigned_(std::vector<std::string>{kBaseA}, stub.MakeTransport());
        CHECK(unsigned_.TurnCredentials().error == dir::Error::None);
    }
    struct Case {
        dir::Response a;
        dir::Response b;
        dir::Error want;
        std::size_t calls;
    };
    const Case cases[] = {
        {Refusal(503, "turn_unavailable"), Down(), dir::Error::TurnUnavailable, 2},
        {Refusal(502, "turn_upstream"), Down(), dir::Error::TurnUpstream, 2},
        {Down(), Refusal(503, "turn_unavailable"), dir::Error::TurnUnavailable, 2},
        {Refusal(503, "turn_unavailable"), Refusal(502, "turn_upstream"), dir::Error::TurnUpstream, 2},
        {Refusal(503, "turn_unavailable"), Json(200, kGrant), dir::Error::None, 2},
        {Refusal(502, "turn_upstream"), Json(200, kGrant), dir::Error::None, 2},
        {Down(), Down(), dir::Error::Unreachable, 2},
        {Refusal(500, "internal"), Down(), dir::Error::Unreachable, 2},
        // A token on a status it does not carry is not a relay answer.
        {Refusal(503, "turn_upstream"), Down(), dir::Error::Unreachable, 2},
        {Refusal(502, "internal"), Down(), dir::Error::Unreachable, 2},
        {Refusal(429, "rate_limited", "17"), Json(200, kGrant), dir::Error::RateLimited, 1},
        {Refusal(400, "bad_field"), Json(200, kGrant), dir::Error::BadField, 1},
        {Refusal(503, "turn_unavailable"), Refusal(400, "bad_json"), dir::Error::BadJson, 2},
    };
    for (const Case& c : cases) {
        Stub stub;
        stub.Script(kBaseA, c.a);
        stub.Script(kBaseB, c.b);
        const auto answer = ClientOver(stub, {kBaseA, kBaseB}).TurnCredentials();
        CHECK(answer.error == c.want);
        CHECK(stub.seen.size() == c.calls);
        CHECK((answer.error == dir::Error::None) == !answer.value.username.empty());
        CHECK(answer.error != dir::Error::RateLimited || answer.retryAfterSeconds == 17);
    }
    // Answers that break §5.9 are Malformed, and their value stays empty.
    const std::string servers = "\"servers\":[\"turn.cloudflare.com:3478\"]";
    std::string nine = "\"servers\":[";
    for (int i = 0; i < 9; i++) {
        nine += (i ? ",\"t" : "\"t") + std::to_string(i) + ".example:3478\"";
    }
    nine += "]";
    const std::string malformed[] = {
        "{}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7199," + servers + "}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200," + servers + ",\"x\":1}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200}",
        "{\"username\":\"\",\"password\":\"p\",\"ttl\":7200," + servers + "}",
        "{\"username\":\"u\",\"password\":\"" + std::string(513, 'p') + "\",\"ttl\":7200," + servers + "}",
        "{\"username\":1,\"password\":\"p\",\"ttl\":7200," + servers + "}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200,\"servers\":[]}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200," + nine + "}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200,\"servers\":[\"turn.cloudflare.com\"]}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200,\"servers\":[3478]}",
        "{\"username\":\"u\",\"password\":\"p\",\"ttl\":7200,\"servers\":\"turn.cloudflare.com:3478\"}",
    };
    for (const std::string& body : malformed) {
        Stub stub;
        stub.Script(kBaseA, Json(200, body));
        const auto answer = ClientOver(stub, {kBaseA}).TurnCredentials();
        CHECK(answer.error == dir::Error::Malformed);
        CHECK(answer.value.username.empty() && answer.value.servers.empty());
    }
    {
        // The limits themselves pass: 512 characters, 8 servers.
        std::string eight = "\"servers\":[";
        for (int i = 0; i < 8; i++) {
            eight += (i ? ",\"t" : "\"t") + std::to_string(i) + ".example:3478\"";
        }
        eight += "]";
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"username\":\"" + std::string(512, 'u') + "\",\"password\":\"p\",\"ttl\":7200," +
                                          eight + "}"));
        const auto answer = ClientOver(stub, {kBaseA}).TurnCredentials();
        CHECK(answer.error == dir::Error::None && answer.value.servers.size() == 8);
    }
    // Reads still go out without a content type, writes with one.
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":null}"));
        ClientOver(stub, {kBaseA}).QuickRace(3);
        CHECK(stub.seen.size() == 1 && Nth(stub, 0).contentType.empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Failover (the policy in gdx_net_directory.h).

void TestFailover() {
    const std::string room = "{\"room\":" + kRoomObject + "}";
    const std::string roomPath = "/v1/rooms/ABC234";

    // Reads: a transport failure or a 5xx moves on to the next base.
    {
        Stub stub;
        stub.Script(kBaseA, Down());
        stub.Script(kBaseB, Json(200, room));
        const auto answer = ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234");
        CHECK(answer.error == dir::Error::None && answer.value.has_value());
        CHECK(stub.seen.size() == 2);
        CHECK(Nth(stub, 0).url == kBaseA + roomPath);
        CHECK(Nth(stub, 1).url == kBaseB + roomPath);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(503, "<html>busy</html>"));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::None);
        CHECK(stub.seen.size() == 2);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(500, "internal"));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::None);
        CHECK(stub.seen.size() == 2);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(503, "busy"));
        stub.Script(kBaseB, Json(502, "bad gateway"));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 2);
    }
    {
        Stub stub;
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 2);
    }
    // A directory answer that is final: a 4xx, a 2xx that breaks the contract, or a status the contract does not name.
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(404, "not_found"));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::NotFound);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(200, "{"));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::Malformed);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(302, ""));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::Malformed);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(0, ""));
        stub.Script(kBaseB, Json(200, room));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234").error == dir::Error::Malformed);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited", "60"));
        stub.Script(kBaseB, Json(200, room));
        const auto answer = ClientOver(stub, {kBaseA, kBaseB}).Lookup("ABC234");
        CHECK(answer.error == dir::Error::RateLimited && answer.retryAfterSeconds == 60);
        CHECK(stub.seen.size() == 1);
    }

    // Writes: a transport failure moves on, because the failed attempt never reached a directory that could apply it.
    // A 5xx does not move on, because the write may already be applied.
    {
        Stub stub;
        stub.Script(kBaseA, Down());
        stub.Script(kBaseB, Json(201, "{\"ttl\":180}"));
        const auto answer = ClientOver(stub, {kBaseA, kBaseB}).Register(RegisterSample());
        CHECK(answer.error == dir::Error::None && answer.value == 180);
        CHECK(stub.seen.size() == 2);
        CHECK(Unsigned(Nth(stub, 1).body) == kRegisterBytes);
        CHECK(Nth(stub, 1).body == Nth(stub, 0).body);  // the same signed bytes go to every base
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(500, "internal"));
        stub.Script(kBaseB, Json(201, "{\"ttl\":180}"));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Register(RegisterSample()).error == dir::Error::Internal);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Json(503, "<html>busy</html>"));
        stub.Script(kBaseB, Json(200, "{\"ttl\":180}"));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Heartbeat(HeartbeatSample()).error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(503, "internal"));
        stub.Script(kBaseB, Json(200, "{\"ttl\":180}"));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Takeover(TakeoverSample()).error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        stub.Script(kBaseA, Refusal(403, "bad_secret"));
        stub.Script(kBaseB, Json(200, "{\"ttl\":180}"));
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Heartbeat(HeartbeatSample()).error == dir::Error::BadSecret);
        CHECK(stub.seen.size() == 1);
    }
    {
        Stub stub;
        CHECK(ClientOver(stub, {kBaseA, kBaseB}).Takeover(TakeoverSample()).error == dir::Error::Unreachable);
        CHECK(stub.seen.size() == 2);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Seeded fuzz. Each loop checks the decision against a second reading of the rule, or checks a property that holds
// for every accepted input.

void TestJsonFuzz() {
    std::mt19937_64 rng(0x51ed0001);
    const std::string alphabet = "{}[]\",:0123456789.-+eEtrufalsn \t\r\n\\/bu";
    std::size_t accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::size_t length = Below(rng, 24);
        std::string text;
        for (std::size_t j = 0; j < length; ++j) {
            if (Below(rng, 100) < 85) {
                text.push_back(alphabet[Below(rng, alphabet.size())]);
            } else {
                text.push_back(static_cast<char>(Below(rng, 256)));
            }
        }
        dir::JsonValue value;
        if (dir::ParseStrictJson(text, value)) {
            ++accepted;
            // An accepted text has one canonical spelling, and that spelling parses to the same value.
            const std::string canonical = Canonical(value);
            dir::JsonValue again;
            CHECK(dir::ParseStrictJson(canonical, again));
            CHECK(Canonical(again) == canonical);
        }
    }
    CHECK(accepted > 0);
}

void TestJsonRoundTrip() {
    std::mt19937_64 rng(0xc0ffee01);
    for (int i = 0; i < 2000; ++i) {
        const dir::JsonValue tree = RandomValue(rng, 0);
        const std::string text = Canonical(tree);
        dir::JsonValue back;
        CHECK(dir::ParseStrictJson(text, back));
        CHECK(Canonical(back) == text);

        // The same tree with whitespace between tokens reads back the same.
        std::string spaced;
        WriteValue(tree, &rng, spaced);
        dir::JsonValue spacedBack;
        CHECK(dir::ParseStrictJson(spaced, spacedBack));
        CHECK(Canonical(spacedBack) == text);
    }
}

// A single flipped byte in a valid document is refused, or it still reads as a document in the grammar.
void TestJsonByteFlips() {
    std::mt19937_64 rng(0xbeef0001);
    for (int i = 0; i < 5000; ++i) {
        std::string text = kRegisterBytes;
        text[Below(rng, text.size())] = static_cast<char>(Below(rng, 256));
        dir::JsonValue value;
        if (dir::ParseStrictJson(text, value)) {
            const std::string canonical = Canonical(value);
            dir::JsonValue again;
            CHECK(dir::ParseStrictJson(canonical, again));
            CHECK(Canonical(again) == canonical);
        }
    }
}

// Code: a fresh reading of CONTRACT.md §3, by character class rather than by the table the client uses.
bool ReferenceCode(const std::string& text) {
    if (text.size() != 6) {
        return false;
    }
    for (char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O' && c != 'U';
        if (!digit && !letter) {
            return false;
        }
    }
    return true;
}

void TestCodeFuzz() {
    std::mt19937_64 rng(0xc0de0001);
    const std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabc-";
    for (int i = 0; i < 20000; ++i) {
        std::string text;
        const std::size_t length = i % 4 == 0 ? 5 + Below(rng, 3) : 6;
        for (std::size_t j = 0; j < length; ++j) {
            text.push_back(alphabet[Below(rng, alphabet.size())]);
        }
        CHECK(dir::IsCode(text) == ReferenceCode(text));
    }
}

void TestSecretFuzz() {
    std::mt19937_64 rng(0x5ec20001);
    // Half the draws use only lower-case hex, so accepted secrets occur. The other half mix in refused characters.
    const std::string hex = "0123456789abcdef";
    const std::string mixed = "0123456789abcdefABCDEFgG-";
    std::size_t accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::string& alphabet = i % 2 == 0 ? hex : mixed;
        const std::size_t length = i % 3 == 0 ? 60 + Below(rng, 9) : 64;
        std::string text;
        for (std::size_t j = 0; j < length; ++j) {
            text.push_back(alphabet[Below(rng, alphabet.size())]);
        }
        bool reference = text.size() == 64;
        for (char c : text) {
            reference = reference && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        dir::Secret secret{};
        const bool parsed = dir::ParseSecretHex(text, secret);
        CHECK(parsed == reference);
        if (parsed) {
            ++accepted;
            CHECK(dir::SecretHex(secret) == text);
        }
    }
    CHECK(accepted > 0);
}

// IPv4: four octets, each of one to three digits with no leading zero, each at most 255, and not 0.0.0.0.
bool ReferenceIpv4(const std::string& text) {
    if (text == "0.0.0.0") {
        return false;
    }
    const std::vector<std::string> parts = SplitOn(text, '.');
    if (parts.size() != 4) {
        return false;
    }
    for (const std::string& part : parts) {
        if (part.empty() || part.size() > 3) {
            return false;
        }
        for (char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        if (part.size() > 1 && part[0] == '0') {
            return false;
        }
        if (std::stoi(part) > 255) {
            return false;
        }
    }
    return true;
}

std::string RandomIpv4(std::mt19937_64& rng) {
    std::string out;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i > 0) {
            out.push_back('.');
        }
        std::string octet;
        if (Below(rng, 5) == 0) {
            octet = std::to_string(Below(rng, 1000));
        } else {
            const std::size_t length = Below(rng, 4);
            for (std::size_t j = 0; j < length; ++j) {
                octet.push_back(static_cast<char>('0' + Below(rng, 10)));
            }
        }
        out += octet;
    }
    if (Below(rng, 10) == 0) {
        out.push_back('.');
    }
    return out;
}

void TestIpv4Fuzz() {
    std::mt19937_64 rng(0x1f40001);
    for (int i = 0; i < 20000; ++i) {
        const std::string text = RandomIpv4(rng);
        CHECK(dir::IsIpv4(text) == ReferenceIpv4(text));
    }
}

// IPv6, read a second way: split on "::" first, then count the groups on each side. Everything else is CONTRACT.md §3.
bool RefGroups(const std::string& text, std::vector<std::string>& out) {
    if (text.empty()) {
        return true;
    }
    for (const std::string& group : SplitOn(text, ':')) {
        if (group.empty() || group.size() > 4) {
            return false;
        }
        for (char c : group) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        out.push_back(group);
    }
    return true;
}

bool ReferenceIpv6(const std::string& text) {
    if (text.size() < 2 || text.size() > 39 || text.find(":::") != std::string::npos) {
        return false;
    }
    std::size_t gaps = 0;
    std::size_t first = std::string::npos;
    for (std::size_t at = text.find("::"); at != std::string::npos; at = text.find("::", at + 2)) {
        if (first == std::string::npos) {
            first = at;
        }
        ++gaps;
    }
    std::vector<std::string> groups;
    if (gaps == 0) {
        if (!RefGroups(text, groups) || groups.size() != 8) {
            return false;
        }
    } else if (gaps == 1) {
        if (!RefGroups(text.substr(0, first), groups) || !RefGroups(text.substr(first + 2), groups)) {
            return false;
        }
        if (groups.size() > 7) {
            return false;
        }
    } else {
        return false;
    }
    for (const std::string& group : groups) {
        if (group.find_first_not_of('0') != std::string::npos) {
            return true;  // one non-zero group is enough
        }
    }
    return false;  // no groups, or every group is zero
}

std::string RandomIpv6(std::mt19937_64& rng) {
    constexpr char kHex[] = "0123456789abcdef";
    std::vector<std::string> parts;
    const std::size_t count = Below(rng, 10);
    for (std::size_t i = 0; i < count; ++i) {
        std::string group;
        const std::size_t length = Below(rng, 5);
        for (std::size_t j = 0; j < length; ++j) {
            group.push_back(kHex[Below(rng, 16)]);
        }
        if (Below(rng, 4) == 0) {
            group = "0";
        }
        parts.push_back(group);
    }
    std::string out;
    const bool gap = Below(rng, 2) == 1;
    const std::size_t gapAt = Below(rng, parts.size() + 1);
    if (parts.empty()) {
        out = gap ? "::" : "";
    } else {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                out += (gap && i == gapAt) ? "::" : ":";
            }
            out += parts[i];
        }
        if (gap && gapAt == 0) {
            out = "::" + out;
        } else if (gap && gapAt == parts.size()) {
            out += "::";
        }
    }
    if (Below(rng, 5) == 0 && !out.empty()) {
        constexpr char kNoise[] = "0:gA.";
        out[Below(rng, out.size())] = kNoise[Below(rng, 5)];
    }
    return out;
}

void TestIpv6Fuzz() {
    std::mt19937_64 rng(0x6fed0001);
    std::size_t accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::string text = RandomIpv6(rng);
        const bool got = dir::IsIpv6(text);
        CHECK(got == ReferenceIpv6(text));
        if (got) {
            ++accepted;
        }
    }
    CHECK(accepted > 0);
}

// The record reader: a random edit to a valid record either reads back as a record that meets CONTRACT.md §3, or is
// Malformed. It never yields a value that breaks a rule.
void TestRecordFuzz() {
    std::mt19937_64 rng(0x4ec0d001);
    std::size_t accepted = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string room = kRoomObject;
        const std::size_t edits = 1 + Below(rng, 2);
        for (std::size_t e = 0; e < edits; ++e) {
            const std::size_t at = Below(rng, room.size());
            room[at] = static_cast<char>(Below(rng, 2) == 0 ? 0x20 + Below(rng, 95) : Below(rng, 256));
        }
        Stub stub;
        stub.Script(kBaseA, Json(200, "{\"room\":" + room + "}"));
        const auto answer = ClientOver(stub, {kBaseA}).Lookup("ABC234");
        if (answer.error == dir::Error::None && answer.value.has_value()) {
            ++accepted;
            CHECK(dir::IsCode(answer.value->code));
            CHECK(answer.value->ttl >= 1 && answer.value->ttl <= dir::kTtlSeconds);
            CHECK(dir::DraftValid(answer.value->draft));
        } else {
            CHECK(answer.error == dir::Error::Malformed);
        }
    }
    CHECK(accepted > 0);
}

// The Retry-After header: digits only, one to nine of them. Anything else on a 429 is Malformed.
void TestRetryAfterFuzz() {
    std::mt19937_64 rng(0x7e7a0001);
    const std::string alphabet = "0123456789 -+x.";
    for (int i = 0; i < 2000; ++i) {
        std::string header;
        const std::size_t length = Below(rng, 12);
        for (std::size_t j = 0; j < length; ++j) {
            header.push_back(alphabet[Below(rng, alphabet.size())]);
        }
        bool digits = !header.empty() && header.size() <= 9;
        for (char c : header) {
            digits = digits && c >= '0' && c <= '9';
        }
        Stub stub;
        stub.Script(kBaseA, Refusal(429, "rate_limited", header));
        const auto answer = ClientOver(stub, {kBaseA}).Lookup("ABC234");
        if (digits) {
            CHECK(answer.error == dir::Error::RateLimited);
            CHECK(answer.retryAfterSeconds == (std::min)(static_cast<uint32_t>(std::stoul(header)), 3600u));
        } else {
            CHECK(answer.error == dir::Error::Malformed);
            CHECK(answer.retryAfterSeconds == 0);
        }
    }
}

}  // namespace

int main() {
    TestConstants();
    TestErrorNames();
    TestCodes();
    TestHostKeys();
    TestAddresses();
    TestDraftRules();
    TestStrictJson();
    TestSecrets();
    TestSha256Vectors();
    TestEncoders();
    TestRecordStructure();
    TestClientConfig();
    TestInvalidInputIsNotSent();
    TestRegisterAnswers();
    TestHeartbeatAndTakeoverAnswers();
    TestSigning();
    TestLookupAndRecords();
    TestListsAndQuick();
    TestErrorTokens();
    TestRetryAfter();
    TestTurnServers();
    TestTurnCredentials();
    TestFailover();
    TestJsonFuzz();
    TestJsonRoundTrip();
    TestJsonByteFlips();
    TestCodeFuzz();
    TestSecretFuzz();
    TestIpv4Fuzz();
    TestIpv6Fuzz();
    TestRecordFuzz();
    TestRetryAfterFuzz();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
