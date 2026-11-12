// port/net/gdx_net_directory.cpp: room directory client, headless: strict reader, encoders and failover (G-NET O5.5).
// Copyright (c) Zorkats
//
// The rules are CONTRACT.md's. Each rule names its section where it is applied. The reader follows src/json.ts in the
// Worker, so both ends refuse the same bytes. Nothing here opens a socket or reads a clock: the caller's Transport
// makes the exchange.

#include "gdx_net_directory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gdx_sha256.h"

namespace gdx::net::directory {

namespace {

// The enum wire names live in these tables and nowhere else (CONTRACT.md §3).
constexpr RoomType kTypes[] = {RoomType::Code, RoomType::Open, RoomType::Friends};
constexpr Family kFamilies[] = {Family::V4, Family::V6};
constexpr How kHows[] = {How::Manual, How::Upnp, How::Natpmp, How::Pcp, How::Stun, How::Ipv6};

// Crockford base32: upper case, with I, L, O and U left out (CONTRACT.md §3).
constexpr std::string_view kCrockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string_view TypeName(RoomType type) {
    switch (type) {
        case RoomType::Code:
            return "code";
        case RoomType::Open:
            return "open";
        case RoomType::Friends:
            return "friends";
    }
    return "";
}

std::string_view FamilyName(Family family) {
    switch (family) {
        case Family::V4:
            return "v4";
        case Family::V6:
            return "v6";
    }
    return "";
}

std::string_view HowName(How how) {
    switch (how) {
        case How::Manual:
            return "manual";
        case How::Upnp:
            return "upnp";
        case How::Natpmp:
            return "natpmp";
        case How::Pcp:
            return "pcp";
        case How::Stun:
            return "stun";
        case How::Ipv6:
            return "ipv6";
    }
    return "";
}

// Finds the value whose wire name is text. out is written only on a match.
template <typename T, std::size_t N>
bool FromName(std::string_view text, const T (&all)[N], std::string_view (*nameOf)(T), T& out) {
    for (T value : all) {
        if (nameOf(value) == text) {
            out = value;
            return true;
        }
    }
    return false;
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsLowerHex(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f');
}

// A host key and a secret are both 64 lower-case hex characters (CONTRACT.md §3).
bool IsHex64(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (char c : text) {
        if (!IsLowerHex(c)) {
            return false;
        }
    }
    return true;
}

// One IPv4 octet: decimal, no sign, no leading zero except the single digit 0, and at most 255 (CONTRACT.md §3).
bool IsOctet(std::string_view text) {
    if (text.empty() || text.size() > 3 || (text.size() > 1 && text[0] == '0')) {
        return false;
    }
    unsigned value = 0;
    for (char c : text) {
        if (!IsDigit(c)) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value <= 255;
}

// Counts the groups of one side of an IPv6 "::" and reports whether they are all zero. An empty side has no groups.
// Every group is one to four lower-case hex digits (CONTRACT.md §3).
bool CountGroups(std::string_view run, std::size_t& count, bool& allZero) {
    count = 0;
    allZero = true;
    if (run.empty()) {
        return true;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t colon = run.find(':', start);
        const std::size_t length = colon == std::string_view::npos ? colon : colon - start;
        const std::string_view group = run.substr(start, length);
        if (group.empty() || group.size() > 4) {
            return false;
        }
        for (char c : group) {
            if (!IsLowerHex(c)) {
                return false;
            }
            if (c != '0') {
                allZero = false;
            }
        }
        ++count;
        if (colon == std::string_view::npos) {
            return true;
        }
        start = colon + 1;
    }
}

bool EndpointValid(const Endpoint& endpoint) {
    if (FamilyName(endpoint.family).empty() || HowName(endpoint.how).empty() || endpoint.port == 0) {
        return false;
    }
    return endpoint.family == Family::V4 ? IsIpv4(endpoint.addr) : IsIpv6(endpoint.addr);
}

// Strict JSON reader for CONTRACT.md §2. It follows src/json.ts rule for rule: whitespace is allowed around the
// value, a repeated key is refused, and a nesting level above 8 is refused. The ceiling is checked as the digits
// accumulate, so a number too large for the integer type is refused without wrapping.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    // The text must be one value with nothing but whitespace around it.
    bool Document(JsonValue& out) {
        if (!ReadValue(0, out)) {
            return false;
        }
        SkipSpace();
        return pos_ == text_.size();
    }

private:
    bool ReadValue(std::size_t depth, JsonValue& out) {
        SkipSpace();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '{') {
            return ReadObject(depth + 1, out);
        }
        if (c == '[') {
            return ReadArray(depth + 1, out);
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return ReadString(out.text);
        }
        if (c == 't') {
            return ReadLiteral("true", JsonValue::Kind::Bool, true, out);
        }
        if (c == 'f') {
            return ReadLiteral("false", JsonValue::Kind::Bool, false, out);
        }
        if (c == 'n') {
            return ReadLiteral("null", JsonValue::Kind::Null, false, out);
        }
        if (IsDigit(c)) {
            return ReadNumber(out);
        }
        return false;
    }

    bool ReadLiteral(std::string_view word, JsonValue::Kind kind, bool flag, JsonValue& out) {
        if (text_.substr(pos_, word.size()) != word) {
            return false;
        }
        pos_ += word.size();
        out.kind = kind;
        out.boolean = flag;
        return true;
    }

    // 0, or a nonzero digit and digits. A leading zero followed by a digit leaves that digit behind, which the caller
    // then refuses (01). A sign, point or exponent is refused the same way (-1, 1.0, 1e3).
    bool ReadNumber(JsonValue& out) {
        uint64_t value = 0;
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && IsDigit(text_[pos_])) {
                const auto digit = static_cast<uint64_t>(text_[pos_] - '0');
                // value * 10 + digit must stay at or below the ceiling, so the test is done before the multiply.
                if (value > (kJsonIntegerMax - digit) / 10) {
                    return false;
                }
                value = value * 10 + digit;
                ++pos_;
            }
        }
        out.kind = JsonValue::Kind::Number;
        out.number = value;
        return true;
    }

    // Printable ASCII only. The only escapes are \" and \\, so nothing else has to be decoded.
    bool ReadString(std::string& out) {
        ++pos_;  // the opening quote; the caller has checked it is there
        out.clear();
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                if (pos_ + 1 >= text_.size()) {
                    return false;
                }
                const char next = text_[pos_ + 1];
                if (next != '"' && next != '\\') {
                    return false;
                }
                out.push_back(next);
                pos_ += 2;
                continue;
            }
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
        return false;  // the string never closed
    }

    bool ReadArray(std::size_t depth, JsonValue& out) {
        if (depth > kJsonDepthMax) {
            return false;
        }
        ++pos_;
        out.kind = JsonValue::Kind::Array;
        SkipSpace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!ReadValue(depth, item)) {
                return false;
            }
            out.items.push_back(std::move(item));
            SkipSpace();
            if (Consume(',')) {
                continue;
            }
            return Consume(']');
        }
    }

    bool ReadObject(std::size_t depth, JsonValue& out) {
        if (depth > kJsonDepthMax) {
            return false;
        }
        ++pos_;
        out.kind = JsonValue::Kind::Object;
        SkipSpace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return false;
            }
            std::string key;
            if (!ReadString(key)) {
                return false;
            }
            // A repeated key is refused. JSON.parse keeps the last one silently, and the two sides must not differ.
            for (const std::string& seen : out.keys) {
                if (seen == key) {
                    return false;
                }
            }
            SkipSpace();
            if (!Consume(':')) {
                return false;
            }
            JsonValue member;
            if (!ReadValue(depth, member)) {
                return false;
            }
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(member));
            SkipSpace();
            if (Consume(',')) {
                continue;
            }
            return Consume('}');
        }
    }

    void SkipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                return;
            }
            ++pos_;
        }
    }

    bool Consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// Writes the draft keys in the order of CONTRACT.md §3 and §4, then closes the request object. The draft is the last
// member of every request body, so the closing brace belongs here. The caller has checked the draft with DraftValid,
// so no string here needs escaping: every one is a code, a hex string, an address or a fixed word.
void AppendDraft(const Draft& draft, std::string& out) {
    out += "\"type\":\"";
    out += TypeName(draft.type);
    out += "\",\"humans\":";
    out += std::to_string(draft.humans);
    out += ",\"freeSeats\":";
    out += std::to_string(draft.freeSeats);
    out += ",\"reachable\":";
    out += draft.reachable ? "true" : "false";
    out += ",\"protocol\":";
    out += std::to_string(draft.protocol);
    out += ",\"hostKey\":\"";
    out += draft.hostKey;
    out += "\",\"endpoints\":[";
    for (std::size_t i = 0; i < draft.endpoints.size(); ++i) {
        const Endpoint& endpoint = draft.endpoints[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"family\":\"";
        out += FamilyName(endpoint.family);
        out += "\",\"addr\":\"";
        out += endpoint.addr;
        out += "\",\"port\":";
        out += std::to_string(endpoint.port);
        out += ",\"how\":\"";
        out += HowName(endpoint.how);
        out += "\"}";
    }
    out += "]}";
}

// The encoders refuse a body the directory would refuse with 413. The draft limits keep a valid body far below the
// cap, so this is a backstop for a future change to a limit, not a path a valid draft reaches.
bool FitsBody(std::string body, std::string& out) {
    if (body.size() > kBodyMax) {
        return false;
    }
    out = std::move(body);
    return true;
}

// The error tokens of CONTRACT.md §6, with their statuses. The rows with status 0 are the errors this client raises
// itself; the directory never sends those tokens.
struct ErrorRow {
    Error error;
    const char* token;
    int status;
};

constexpr ErrorRow kErrorRows[] = {
    {Error::BadConfig, "bad_config", 0},
    {Error::Invalid, "invalid", 0},
    {Error::Unreachable, "unreachable", 0},
    {Error::Malformed, "malformed", 0},
    {Error::BadJson, "bad_json", 400},
    {Error::BadField, "bad_field", 400},
    {Error::BadCode, "bad_code", 400},
    {Error::BadHostKey, "bad_host_key", 400},
    {Error::BadQuery, "bad_query", 400},
    {Error::UnsupportedMedia, "unsupported_media", 415},
    {Error::TooLarge, "too_large", 413},
    {Error::BadSecret, "bad_secret", 403},
    {Error::BadSignature, "bad_signature", 403},
    {Error::NotFound, "not_found", 404},
    {Error::NoRoute, "no_route", 404},
    {Error::MethodNotAllowed, "method_not_allowed", 405},
    {Error::CodeTaken, "code_taken", 409},
    {Error::RateLimited, "rate_limited", 429},
    {Error::Internal, "internal", 500},
    {Error::TurnUpstream, "turn_upstream", 502},
    {Error::TurnUnavailable, "turn_unavailable", 503},
};

// A body is read only when it is within the cap (CONTRACT.md §2). On failure out is left alone.
bool ParseBody(const std::string& text, JsonValue& out) {
    return text.size() <= kBodyMax && ParseStrictJson(text, out);
}

// True when the object has exactly these keys. The reader refuses repeated keys, so equal counts and found names are
// the same set.
bool HasExactKeys(const JsonValue& value, std::initializer_list<std::string_view> names) {
    if (value.kind != JsonValue::Kind::Object || value.keys.size() != names.size()) {
        return false;
    }
    for (std::string_view name : names) {
        if (value.Find(name) == nullptr) {
            return false;
        }
    }
    return true;
}

// The directory's error for a body of the form {"error":"<token>"}, or null when the body is anything else.
const ErrorRow* DirectoryError(const std::string& body) {
    JsonValue value;
    if (!ParseBody(body, value) || !HasExactKeys(value, {"error"})) {
        return nullptr;
    }
    const JsonValue* token = value.Find("error");
    if (token == nullptr || token->kind != JsonValue::Kind::String) {
        return nullptr;
    }
    for (const ErrorRow& row : kErrorRows) {
        if (row.status != 0 && token->text == row.token) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (std::string_view(keys[i]) == key) {
            return &values[i];
        }
    }
    return nullptr;
}

bool ParseStrictJson(std::string_view text, JsonValue& out) {
    JsonValue value;
    Reader reader(text);
    if (!reader.Document(value)) {
        return false;
    }
    out = std::move(value);
    return true;
}

const char* ErrorName(Error error) {
    if (error == Error::None) {
        return "none";
    }
    for (const ErrorRow& row : kErrorRows) {
        if (row.error == error) {
            return row.token;
        }
    }
    return "unknown";
}

bool IsCode(std::string_view text) {
    if (text.size() != 6) {
        return false;
    }
    for (char c : text) {
        if (kCrockford.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool IsHostKey(std::string_view text) {
    return IsHex64(text);
}

bool IsIpv4(std::string_view addr) {
    if (addr == "0.0.0.0") {
        return false;
    }
    std::size_t parts = 0;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = addr.find('.', start);
        const std::size_t length = dot == std::string_view::npos ? dot : dot - start;
        if (!IsOctet(addr.substr(start, length))) {
            return false;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts == 4;
}

bool IsIpv6(std::string_view addr) {
    if (addr.size() < 2 || addr.size() > 39) {
        return false;
    }
    for (char c : addr) {
        if (!IsLowerHex(c) && c != ':') {
            return false;
        }
    }
    const std::size_t gap = addr.find("::");
    std::size_t head = 0;
    std::size_t tail = 0;
    bool headZero = true;
    bool tailZero = true;
    if (gap == std::string_view::npos) {
        // Without "::" the address is exactly eight groups, and not the all-zero address (CONTRACT.md §3).
        return CountGroups(addr, head, headZero) && head == 8 && !headZero;
    }
    const std::string_view rest = addr.substr(gap + 2);
    if (rest.find("::") != std::string_view::npos) {
        return false;
    }
    // With "::" there are at most seven explicit groups, so the "::" always stands for at least one group.
    return CountGroups(addr.substr(0, gap), head, headZero) && CountGroups(rest, tail, tailZero) &&
           head + tail <= 7 && !(headZero && tailZero);
}

bool DraftValid(const Draft& draft) {
    if (TypeName(draft.type).empty() || draft.humans > kHumansMax || draft.freeSeats > kHumansMax ||
        draft.humans + draft.freeSeats > kHumansMax || draft.protocol == 0 || !IsHostKey(draft.hostKey) ||
        draft.endpoints.empty() || draft.endpoints.size() > kEndpointsMax) {
        return false;
    }
    for (const Endpoint& endpoint : draft.endpoints) {
        if (!EndpointValid(endpoint)) {
            return false;
        }
    }
    return true;
}

namespace {

int Nibble(char c) {
    if (IsDigit(c)) {
        return c - '0';
    }
    return c - 'a' + 10;  // the caller has checked IsHex64
}

std::string HexOf(const uint8_t* bytes, std::size_t count) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] >> 4)]);
        out.push_back(kDigits[static_cast<std::size_t>(bytes[i] & 0x0F)]);
    }
    return out;
}

}  // namespace

std::string SecretHex(const Secret& secret) {
    return HexOf(secret.data(), secret.size());
}

bool ParseSecretHex(std::string_view text, Secret& out) {
    if (!IsHex64(text)) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>((Nibble(text[2 * i]) << 4) | Nibble(text[2 * i + 1]));
    }
    return true;
}

std::string TakeoverHashOf(const Secret& secret) {
    // The hash is over the 32 raw bytes, not over the hex text (CONTRACT.md §3, §8, §9).
    gdx::hash::Sha256 state{};
    gdx::hash::sha256_init(&state);
    gdx::hash::sha256_update(&state, secret.data(), secret.size());
    unsigned char digest[32];
    gdx::hash::sha256_final(&state, digest);
    return HexOf(digest, sizeof digest);
}

// Encoders: the keys are written in the order of CONTRACT.md §4 and §5 so that the same logical body has the same
// bytes on both sides (CONTRACT.md §2, §9).

bool EncodeRegister(const RegisterRequest& request, std::string& out) {
    if (!IsCode(request.code) || !IsHex64(request.takeoverHash) || !DraftValid(request.draft)) {
        return false;
    }
    std::string body = "{\"code\":\"" + request.code + "\",\"takeoverHash\":\"" + request.takeoverHash + "\",";
    AppendDraft(request.draft, body);
    return FitsBody(std::move(body), out);
}

bool EncodeHeartbeat(const HeartbeatRequest& request, std::string& out) {
    if (!IsCode(request.code) || !DraftValid(request.draft)) {
        return false;
    }
    std::string body = "{\"secret\":\"" + SecretHex(request.secret) + "\",";
    AppendDraft(request.draft, body);
    return FitsBody(std::move(body), out);
}

bool EncodeTakeover(const TakeoverRequest& request, std::string& out) {
    if (!IsCode(request.code) || !IsHex64(request.nextHash) || !DraftValid(request.draft)) {
        return false;
    }
    std::string body = "{\"secret\":\"" + SecretHex(request.secret) + "\",\"nextHash\":\"" + request.nextHash + "\",";
    AppendDraft(request.draft, body);
    return FitsBody(std::move(body), out);
}

bool EncodeDelist(const DelistRequest& request, std::string& out) {
    if (!IsCode(request.code)) {
        return false;
    }
    out = "{\"secret\":\"" + SecretHex(request.secret) + "\"}";
    return true;
}

std::string SigningMessage(std::string_view path, std::string_view unsignedBody) {
    std::string message;
    message.reserve(kSignLabel.size() + path.size() + unsignedBody.size() + 7);
    message += kSignLabel;
    message += "\nPOST ";
    message += path;
    message += '\n';
    message += unsignedBody;
    return message;
}

bool AppendSignature(std::string& body, const Signature& signature) {
    if (body.empty() || body.back() != '}') {
        return false;
    }
    body.pop_back();
    body += ",\"sig\":\"";
    body += HexOf(signature.data(), signature.size());
    body += "\"}";
    return true;
}

bool SplitTurnServer(std::string_view text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view name = text.substr(0, colon);
    const std::string_view digits = text.substr(colon + 1);
    if (name.empty() || name.size() > 253 || name.front() == '.' || name.front() == '-' || name.back() == '-') {
        return false;
    }
    for (char c : name) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter && !IsDigit(c) && c != '.' && c != '-') {
            return false;
        }
    }
    if (digits.empty() || digits.size() > 5 || digits.front() == '0') {
        return false;
    }
    uint32_t value = 0;
    for (char c : digits) {
        if (!IsDigit(c)) {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value > 65535) {
        return false;
    }
    host = std::string(name);
    port = static_cast<uint16_t>(value);
    return true;
}

namespace {

// What one exchange produced: a directory error, or the parsed body of the expected 2xx answer (CONTRACT.md §6).
struct Reply {
    Error error = Error::None;
    uint32_t retryAfterSeconds = 0;
    JsonValue body;
};

bool TextField(const JsonValue& object, std::string_view key, std::string& out) {
    const JsonValue* field = object.Find(key);
    if (field == nullptr || field->kind != JsonValue::Kind::String) {
        return false;
    }
    out = field->text;
    return true;
}

bool FlagField(const JsonValue& object, std::string_view key, bool& out) {
    const JsonValue* field = object.Find(key);
    if (field == nullptr || field->kind != JsonValue::Kind::Bool) {
        return false;
    }
    out = field->boolean;
    return true;
}

bool NumberField(const JsonValue& object, std::string_view key, uint64_t low, uint64_t high, uint64_t& out) {
    const JsonValue* field = object.Find(key);
    if (field == nullptr || field->kind != JsonValue::Kind::Number || field->number < low || field->number > high) {
        return false;
    }
    out = field->number;
    return true;
}

bool ReadEndpoint(const JsonValue& value, Endpoint& out) {
    if (!HasExactKeys(value, {"family", "addr", "port", "how"})) {
        return false;
    }
    Endpoint endpoint;
    std::string text;
    uint64_t number = 0;
    if (!TextField(value, "family", text) || !FromName(text, kFamilies, FamilyName, endpoint.family)) {
        return false;
    }
    if (!TextField(value, "addr", endpoint.addr)) {
        return false;
    }
    if (!NumberField(value, "port", 1, 65535, number)) {
        return false;
    }
    endpoint.port = static_cast<uint16_t>(number);
    if (!TextField(value, "how", text) || !FromName(text, kHows, HowName, endpoint.how)) {
        return false;
    }
    if (!EndpointValid(endpoint)) {
        return false;
    }
    out = std::move(endpoint);
    return true;
}

// Reads the seven draft keys from an object. The caller checks which other keys the object may carry.
bool ReadDraft(const JsonValue& object, Draft& out) {
    Draft draft;
    std::string text;
    uint64_t number = 0;
    if (!TextField(object, "type", text) || !FromName(text, kTypes, TypeName, draft.type)) {
        return false;
    }
    if (!NumberField(object, "humans", 0, kHumansMax, number)) {
        return false;
    }
    draft.humans = static_cast<uint32_t>(number);
    if (!NumberField(object, "freeSeats", 0, kHumansMax, number)) {
        return false;
    }
    draft.freeSeats = static_cast<uint32_t>(number);
    if (!FlagField(object, "reachable", draft.reachable)) {
        return false;
    }
    if (!NumberField(object, "protocol", 1, 65535, number)) {
        return false;
    }
    draft.protocol = static_cast<uint16_t>(number);
    if (!TextField(object, "hostKey", draft.hostKey)) {
        return false;
    }
    const JsonValue* endpoints = object.Find("endpoints");
    if (endpoints == nullptr || endpoints->kind != JsonValue::Kind::Array || endpoints->items.empty() ||
        endpoints->items.size() > kEndpointsMax) {
        return false;
    }
    for (const JsonValue& item : endpoints->items) {
        Endpoint endpoint;
        if (!ReadEndpoint(item, endpoint)) {
            return false;
        }
        draft.endpoints.push_back(endpoint);
    }
    if (!DraftValid(draft)) {
        return false;
    }
    out = std::move(draft);
    return true;
}

// A record is the draft, the code and the remaining lifetime, and it never carries a takeover hash (CONTRACT.md §3).
bool ReadRecord(const JsonValue& value, Record& out) {
    if (!HasExactKeys(value, {"code", "type", "humans", "freeSeats", "reachable", "protocol", "hostKey", "endpoints",
                              "ttl"})) {
        return false;
    }
    Record record;
    uint64_t ttl = 0;
    if (!TextField(value, "code", record.code) || !IsCode(record.code)) {
        return false;
    }
    if (!ReadDraft(value, record.draft)) {
        return false;
    }
    if (!NumberField(value, "ttl", 1, kTtlSeconds, ttl)) {
        return false;
    }
    record.ttl = static_cast<uint32_t>(ttl);
    out = std::move(record);
    return true;
}

// {"rooms":[Record,...]} with at most kListMax records (CONTRACT.md §4, §5.5).
bool ReadRoomList(const JsonValue& body, std::vector<Record>& out) {
    if (!HasExactKeys(body, {"rooms"})) {
        return false;
    }
    const JsonValue* rooms = body.Find("rooms");
    if (rooms == nullptr || rooms->kind != JsonValue::Kind::Array || rooms->items.size() > kListMax) {
        return false;
    }
    std::vector<Record> records;
    for (const JsonValue& item : rooms->items) {
        Record record;
        if (!ReadRecord(item, record)) {
            return false;
        }
        records.push_back(std::move(record));
    }
    out = std::move(records);
    return true;
}

// {"room":Record} or {"room":null}. The null form is an answer, for Quick race only (CONTRACT.md §5.6).
bool ReadRoomAnswer(const JsonValue& body, std::optional<Record>& out) {
    if (!HasExactKeys(body, {"room"})) {
        return false;
    }
    const JsonValue* room = body.Find("room");
    if (room == nullptr) {
        return false;
    }
    if (room->kind == JsonValue::Kind::Null) {
        out.reset();
        return true;
    }
    Record record;
    if (!ReadRecord(*room, record)) {
        return false;
    }
    out = std::move(record);
    return true;
}

// Lookup needs a room. A null room is not an answer that CONTRACT.md §5.4 allows.
bool ReadLookup(const JsonValue& body, std::optional<Record>& out) {
    return ReadRoomAnswer(body, out) && out.has_value();
}

// {"ttl":180} on register, heartbeat and takeover (CONTRACT.md §4). The value is fixed, so any other is a contract
// breach.
bool ReadTtl(const JsonValue& body, uint32_t& out) {
    uint64_t ttl = 0;
    if (!HasExactKeys(body, {"ttl"}) || !NumberField(body, "ttl", kTtlSeconds, kTtlSeconds, ttl)) {
        return false;
    }
    out = static_cast<uint32_t>(ttl);
    return true;
}

// {"ttl":0} on delist (CONTRACT.md §5.8).
bool ReadTtlZero(const JsonValue& body, uint32_t& out) {
    uint64_t ttl = 1;
    if (!HasExactKeys(body, {"ttl"}) || !NumberField(body, "ttl", 0, 0, ttl)) {
        return false;
    }
    out = 0;
    return true;
}

// {"username","password","ttl":7200,"servers":[...]} (CONTRACT.md §5.9). The strict reader already holds every string
// to printable ASCII (§2).
bool ReadTurnGrant(const JsonValue& body, TurnGrant& out) {
    TurnGrant grant;
    uint64_t ttl = 0;
    if (!HasExactKeys(body, {"username", "password", "ttl", "servers"}) || !TextField(body, "username", grant.username) ||
        !TextField(body, "password", grant.password) ||
        !NumberField(body, "ttl", kTurnTtlSeconds, kTurnTtlSeconds, ttl)) {
        return false;
    }
    for (const std::string* text : {&grant.username, &grant.password}) {
        if (text->empty() || text->size() > kTurnTextMax) {
            return false;
        }
    }
    const JsonValue* servers = body.Find("servers");
    if (servers == nullptr || servers->kind != JsonValue::Kind::Array || servers->items.empty() ||
        servers->items.size() > kTurnServersMax) {
        return false;
    }
    for (const JsonValue& item : servers->items) {
        std::string host;
        uint16_t port = 0;
        if (item.kind != JsonValue::Kind::String || !SplitTurnServer(item.text, host, port)) {
            return false;
        }
        grant.servers.push_back(item.text);
    }
    grant.ttl = static_cast<uint32_t>(ttl);
    out = std::move(grant);
    return true;
}

// Retry-After is whole seconds (CONTRACT.md §6). Anything else is not a value the directory sends.
constexpr uint32_t kRetryAfterMaxSeconds = 3600;
bool ParseRetryAfter(const std::string& text, uint32_t& out) {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    uint32_t value = 0;
    for (char c : text) {
        if (!IsDigit(c)) {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    // A wait past an hour is not one the directory sends, and its milliseconds would pass the int32_t deadlines.
    out = (std::min)(value, kRetryAfterMaxSeconds);
    return true;
}

// Sets the reply from one response that came back with an answer from the directory: the expected 2xx, or a 4xx
// with the token and status CONTRACT.md §6 pairs. Anything else is Malformed and is never retried.
void Classify(const Response& response, int expected, Reply& reply) {
    if (response.status == expected) {
        // Set on every 2xx, so the Unreachable that Exchange starts with never survives a real answer.
        reply.error = ParseBody(response.body, reply.body) ? Error::None : Error::Malformed;
        return;
    }
    if (response.status >= 400 && response.status <= 499) {
        const ErrorRow* row = DirectoryError(response.body);
        if (row == nullptr || row->status != response.status) {
            reply.error = Error::Malformed;
            return;
        }
        reply.error = row->error;
        if (row->error == Error::RateLimited && !ParseRetryAfter(response.retryAfter, reply.retryAfterSeconds)) {
            reply.error = Error::Malformed;
        }
        return;
    }
    reply.error = Error::Malformed;  // a status the contract does not name
}

// A 500 whose body is the directory's own internal error: the write may have been applied (CONTRACT.md §6).
bool InternalAnswer(const Response& response) {
    const ErrorRow* row = DirectoryError(response.body);
    return response.status == 500 && row != nullptr && row->error == Error::Internal;
}

// The base URLs must all be https:// with no trailing slash and no bytes outside printable ASCII, so a base cannot
// smuggle a space or a control byte into a request line (CONTRACT.md §2).
bool ConfigOk(const std::vector<std::string>& bases, const Transport& transport) {
    if (!transport || bases.empty()) {
        return false;
    }
    constexpr std::string_view kScheme = "https://";
    for (const std::string& base : bases) {
        if (base.size() <= kScheme.size() || std::string_view(base).substr(0, kScheme.size()) != kScheme ||
            base.back() == '/') {
            return false;
        }
        for (char c : base) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte <= 0x20 || byte >= 0x7F) {
                return false;
            }
        }
    }
    return true;
}

// One call, sent to each base in order (CONTRACT.md §6 and the failover policy in the header).
//   - A transport failure moves to the next base, for reads and writes.
//   - A read 5xx moves to the next base.
//   - A write 5xx ends the call. The write may already be applied, and a second base could report code_taken or
//     bad_secret for a write that succeeded.
//   - A 4xx and any 2xx answer end the call.
// A POST carries a JSON body, writes and the relay credentials alike (CONTRACT.md §2). A read that ends with no base
// answering reports the last relay token a base gave (CONTRACT.md §5.9), else Unreachable.
Reply Exchange(const Transport& transport, const std::vector<std::string>& bases, bool write, std::string_view method,
               const std::string& path, const std::string& body, int expected) {
    Reply reply;
    if (!ConfigOk(bases, transport)) {
        reply.error = Error::BadConfig;
        return reply;
    }
    reply.error = Error::Unreachable;  // what the call reports when no base gives an answer
    for (const std::string& base : bases) {
        const Request request{std::string(method), base + path,
                              method == "POST" ? std::string("application/json") : std::string(), body};
        const Response response = transport(request);
        if (!response.ok) {
            continue;
        }
        if (response.status >= 500 && response.status <= 599) {
            if (!write) {
                const ErrorRow* row = DirectoryError(response.body);
                if (row != nullptr && row->status == response.status &&
                    (row->error == Error::TurnUpstream || row->error == Error::TurnUnavailable)) {
                    reply.error = row->error;
                }
                continue;
            }
            reply.error = InternalAnswer(response) ? Error::Internal : Error::Unreachable;
            return reply;
        }
        Classify(response, expected, reply);
        return reply;
    }
    return reply;
}

// Turns an exchange into a call's answer. A directory error passes through. A 2xx body that the reader refuses, or
// that does not match the contract's shape, becomes Malformed. The value is empty on any error.
template <typename T>
Answer<T> Finish(const Reply& reply, bool (*read)(const JsonValue&, T&)) {
    Answer<T> answer;
    answer.error = reply.error;
    answer.retryAfterSeconds = reply.retryAfterSeconds;
    if (reply.error != Error::None) {
        return answer;
    }
    if (!read(reply.body, answer.value)) {
        answer.error = Error::Malformed;
        answer.value = T{};
    }
    return answer;
}

// A call refused before anything was sent: the caller's input breaks the contract (CONTRACT.md §2, §3).
template <typename T>
Answer<T> Refused() {
    Answer<T> answer;
    answer.error = Error::Invalid;
    return answer;
}

std::string ProtocolQuery(std::string_view path, uint16_t protocol) {
    return std::string(path) + "?protocol=" + std::to_string(protocol);
}

}  // namespace

Client::Client(std::vector<std::string> bases, Transport transport)
    : bases_(std::move(bases)), transport_(std::move(transport)) {}

void Client::SetSigner(Signer signer) {
    signer_ = std::move(signer);
}

Answer<uint32_t> Client::Write(const std::string& path, std::string body, int expected,
                               bool (*read)(const JsonValue&, uint32_t&)) const {
    Signature signature{};
    if (!signer_ || !signer_(SigningMessage(path, body), signature) || !AppendSignature(body, signature) ||
        body.size() > kBodyMax) {
        return Refused<uint32_t>();
    }
    return Finish(Exchange(transport_, bases_, true, "POST", path, body, expected), read);
}

Answer<uint32_t> Client::Register(const RegisterRequest& request) const {
    std::string body;
    if (!EncodeRegister(request, body)) {
        return Refused<uint32_t>();
    }
    return Write("/v1/rooms", std::move(body), 201, ReadTtl);
}

Answer<uint32_t> Client::Heartbeat(const HeartbeatRequest& request) const {
    std::string body;
    if (!EncodeHeartbeat(request, body)) {
        return Refused<uint32_t>();
    }
    return Write("/v1/rooms/" + request.code + "/heartbeat", std::move(body), 200, ReadTtl);
}

Answer<uint32_t> Client::Takeover(const TakeoverRequest& request) const {
    std::string body;
    if (!EncodeTakeover(request, body)) {
        return Refused<uint32_t>();
    }
    return Write("/v1/rooms/" + request.code + "/takeover", std::move(body), 200, ReadTtl);
}

Answer<uint32_t> Client::Delist(const DelistRequest& request) const {
    std::string body;
    if (!EncodeDelist(request, body)) {
        return Refused<uint32_t>();
    }
    return Write("/v1/rooms/" + request.code + "/delist", std::move(body), 200, ReadTtlZero);
}

Answer<std::optional<Record>> Client::Lookup(std::string_view code) const {
    if (!IsCode(code)) {
        return Refused<std::optional<Record>>();
    }
    return Finish(Exchange(transport_, bases_, false, "GET", "/v1/rooms/" + std::string(code), "", 200), ReadLookup);
}

Answer<std::vector<Record>> Client::OpenRooms(uint16_t protocol) const {
    if (protocol == 0) {
        return Refused<std::vector<Record>>();
    }
    return Finish(Exchange(transport_, bases_, false, "GET", ProtocolQuery("/v1/open", protocol), "", 200),
                  ReadRoomList);
}

Answer<std::optional<Record>> Client::QuickRace(uint16_t protocol) const {
    if (protocol == 0) {
        return Refused<std::optional<Record>>();
    }
    return Finish(Exchange(transport_, bases_, false, "GET", ProtocolQuery("/v1/quick", protocol), "", 200),
                  ReadRoomAnswer);
}

Answer<std::vector<Record>> Client::HostRooms(std::string_view hostKey, uint16_t protocol) const {
    if (!IsHostKey(hostKey) || protocol == 0) {
        return Refused<std::vector<Record>>();
    }
    const std::string path = "/v1/hosts/" + std::string(hostKey) + "/rooms";
    return Finish(Exchange(transport_, bases_, false, "GET", ProtocolQuery(path, protocol), "", 200), ReadRoomList);
}

Answer<TurnGrant> Client::TurnCredentials() const {
    // Nothing is written, so nothing is signed, and a 5xx moves on to the next base as a read's does (§5.9).
    return Finish(Exchange(transport_, bases_, false, "POST", "/v1/turn", "{}", 200), ReadTurnGrant);
}

}  // namespace gdx::net::directory
