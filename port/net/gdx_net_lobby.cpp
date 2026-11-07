// port/net/gdx_net_lobby.cpp: O2 LAN lobby wire format, discovery and chat filter (gdx_net_lobby.h).

#include "gdx_net_lobby.h"

#include <array>

namespace gdx::net::lobby {

namespace {

constexpr size_t kRosterMachines = 30;
constexpr uint8_t kNoMachine = 0xFF;
constexpr uint8_t kFlagHost = 1;
constexpr uint8_t kFlagReady = 2;

constexpr uint8_t kDiscoveryMagic[4] = { 'G', 'D', 'X', 'L' };
constexpr uint8_t kKindProbe = 1;
constexpr uint8_t kKindRoom = 2;

class Writer {
public:
    void U8(uint8_t v) {
        bytes_.push_back(v);
    }

    void U16(uint16_t v) {
        U8(static_cast<uint8_t>(v & 0xFF));
        U8(static_cast<uint8_t>(v >> 8));
    }

    void U32(uint32_t v) {
        U16(static_cast<uint16_t>(v & 0xFFFF));
        U16(static_cast<uint16_t>(v >> 16));
    }

    // u8 length + bytes; a string above 255 bytes is cut, which the decoder's own limit then refuses.
    void Str8(const std::string& s) {
        const size_t n = s.size() > 255 ? 255 : s.size();
        U8(static_cast<uint8_t>(n));
        bytes_.insert(bytes_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }

    void Str16(const std::string& s) {
        const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        U16(static_cast<uint16_t>(n));
        bytes_.insert(bytes_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }

    void Raw(const uint8_t* data, size_t n) {
        bytes_.insert(bytes_.end(), data, data + n);
    }

    std::vector<uint8_t> Take() {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

// Every read checks the length first. After a failed read Ok() stays false and every later read returns 0.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(data != nullptr ? len : 0), ok_(data != nullptr) {
    }

    uint8_t U8() {
        if (!Need(1)) {
            return 0;
        }
        return data_[pos_++];
    }

    uint16_t U16() {
        const uint16_t lo = U8();
        const uint16_t hi = U8();
        return static_cast<uint16_t>(lo | (hi << 8));
    }

    uint32_t U32() {
        const uint32_t lo = U16();
        const uint32_t hi = U16();
        return lo | (hi << 16);
    }

    std::string Bytes(size_t n) {
        if (!Need(n)) {
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

    // A string of at most max bytes.
    bool Str8(size_t max, std::string& out) {
        const size_t n = U8();
        if (!ok_ || n > max) {
            ok_ = false;
            return false;
        }
        out = Bytes(n);
        return ok_;
    }

    bool Str16(size_t max, std::string& out) {
        const size_t n = U16();
        if (!ok_ || n > max) {
            ok_ = false;
            return false;
        }
        out = Bytes(n);
        return ok_;
    }

    bool Ok() const {
        return ok_;
    }

    bool Done() const {
        return ok_ && pos_ == len_;
    }

private:
    bool Need(size_t n) {
        if (!ok_ || len_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_;
};

bool Printable(char c) {
    return c >= 0x20 && c <= 0x7E;
}

bool BuildOk(const std::string& build) {
    if (build.empty() || build.size() > kBuildMax) {
        return false;
    }
    for (char c : build) {
        if (!Printable(c)) {
            return false;
        }
    }
    return true;
}

// The letter a character stands for in the filter, or 0 when it is not part of a word.
char FilterLetter(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
        return c;
    }
    switch (c) {
    case '0':
        return 'o';
    case '1':
        return 'i';
    case '3':
        return 'e';
    case '4':
    case '@':
        return 'a';
    case '5':
    case '$':
        return 's';
    case '7':
        return 't';
    default:
        return 0;
    }
}

enum class Match : uint8_t { Start, Anywhere, Whole };

struct Rule {
    std::string word; // lowercase letters
    Match match = Match::Start;
};

// "*word" anywhere, "word|" whole word, "word" at a word start. The word itself goes through FilterLetter,
// so "sh1t" in a player's list means "shit". false for anything else.
bool ParseRule(const std::string& entry, Rule& out) {
    std::string body = entry;
    Match match = Match::Start;
    if (!body.empty() && body.front() == '*') {
        match = Match::Anywhere;
        body.erase(body.begin());
    } else if (!body.empty() && body.back() == '|') {
        match = Match::Whole;
        body.pop_back();
    }
    if (body.empty() || body.size() > 32) {
        return false;
    }
    std::string word;
    for (char c : body) {
        const char letter = FilterLetter(c);
        if (letter == 0) {
            return false;
        }
        word.push_back(letter);
    }
    out.word = word;
    out.match = match;
    return true;
}

// Built in. Kept short and plain: the player's own list (chat-filter.txt) adds the rest.
const char* const kBuiltinWords[] = {
    // Only words that never sit inside an innocent word match anywhere; "cunt" would hit Scunthorpe.
    "*fuck", "*shit", "cunt",   "nigger", "nigga", "faggot", "bitch", "asshole", "bastard", "pussy", "whore",
    "slut",  "retard", "wanker", "dick|",  "cock|", "fag|",   "twat|", "ass|",    "tits|",   "cum|",  "rape|",
    "kys|",
};

std::vector<Rule> BuildRules(const std::vector<std::string>& extra) {
    std::vector<Rule> rules;
    for (const char* word : kBuiltinWords) {
        Rule rule;
        if (ParseRule(word, rule)) {
            rules.push_back(rule);
        }
    }
    for (const std::string& word : extra) {
        Rule rule;
        if (ParseRule(word, rule)) {
            rules.push_back(rule);
        }
    }
    return rules;
}

} // namespace

const char* RejectReasonText(RejectReason reason) {
    switch (reason) {
    case RejectReason::Build:
        return "The host runs a different G-Diffuser build. Both players need the same version.";
    case RejectReason::Full:
        return "The room is full.";
    case RejectReason::BadProfile:
        return "The host could not accept your profile name.";
    case RejectReason::Kicked:
        return "The host removed you from the room.";
    }
    return "The host refused the join.";
}

bool NameOk(const std::string& name) {
    if (name.empty() || name.size() > kNameMax || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    for (char c : name) {
        if (!Printable(c)) {
            return false;
        }
    }
    return true;
}

bool MachineOk(uint8_t machine) {
    return machine < kRosterMachines || machine == kNoMachine;
}

bool ColourOk(uint32_t colour) {
    return colour <= 0xFFFFFFu;
}

std::string SanitizeChat(const std::string& raw) {
    std::string out;
    bool pendingSpace = false;
    for (char c : raw) {
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
            pendingSpace = !out.empty();
            continue;
        }
        if (!Printable(c)) {
            continue;
        }
        if (pendingSpace) {
            if (out.size() + 1 >= kChatMax) {
                break;
            }
            out.push_back(' ');
            pendingSpace = false;
        }
        if (out.size() >= kChatMax) {
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::string FilterChat(const std::string& text, const std::vector<std::string>& extra) {
    const std::vector<Rule> rules = BuildRules(extra);
    std::string letters(text.size(), '\0');
    for (size_t i = 0; i < text.size(); i++) {
        letters[i] = FilterLetter(text[i]);
    }
    std::string out = text;
    const auto wordChar = [&](size_t i) { return i < letters.size() && letters[i] != 0; };

    size_t wordBegin = 0;
    for (size_t start = 0; start < letters.size(); start++) {
        if (!wordChar(start)) {
            continue;
        }
        const bool wordStart = start == 0 || !wordChar(start - 1);
        if (wordStart) {
            wordBegin = start;
        }
        for (const Rule& rule : rules) {
            if (rule.match != Match::Anywhere && !wordStart) {
                continue;
            }
            if (letters.compare(start, rule.word.size(), rule.word) != 0) {
                continue;
            }
            size_t end = start + rule.word.size();
            if (rule.match == Match::Whole && wordChar(end)) {
                continue;
            }
            while (wordChar(end)) {
                end++;
            }
            // The whole word goes, so "motherfucker" does not keep half of itself.
            for (size_t i = wordBegin; i < end; i++) {
                out[i] = '*';
            }
            break;
        }
    }
    return out;
}

std::vector<uint8_t> EncodeJoin(const JoinRequest& join) {
    Writer w;
    w.Str8(join.build);
    w.Str8(join.name);
    w.U32(join.colour);
    w.U8(join.machine);
    return w.Take();
}

std::vector<uint8_t> EncodeWelcome(uint8_t id) {
    Writer w;
    w.U8(id);
    return w.Take();
}

std::vector<uint8_t> EncodeReject(RejectReason reason) {
    Writer w;
    w.U8(static_cast<uint8_t>(reason));
    return w.Take();
}

std::vector<uint8_t> EncodeRoster(const std::vector<PlayerInfo>& roster) {
    Writer w;
    w.U8(static_cast<uint8_t>(roster.size() > 255 ? 255 : roster.size()));
    for (const PlayerInfo& p : roster) {
        w.U8(p.id);
        w.U8(static_cast<uint8_t>((p.host ? kFlagHost : 0) | (p.ready ? kFlagReady : 0)));
        w.U32(p.colour);
        w.U8(p.machine);
        w.Str8(p.name);
    }
    return w.Take();
}

std::vector<uint8_t> EncodeReady(bool ready) {
    Writer w;
    w.U8(ready ? 1 : 0);
    return w.Take();
}

std::vector<uint8_t> EncodeChat(const std::string& text) {
    Writer w;
    w.Str16(text);
    return w.Take();
}

std::vector<uint8_t> EncodeChatLine(const ChatLine& line) {
    Writer w;
    w.U8(line.from);
    w.Str16(line.text);
    return w.Take();
}

bool DecodeJoin(const uint8_t* data, size_t len, JoinRequest& out) {
    Reader r(data, len);
    JoinRequest join;
    if (!r.Str8(kBuildMax, join.build) || !r.Str8(kNameMax, join.name)) {
        return false;
    }
    join.colour = r.U32();
    join.machine = r.U8();
    if (!r.Done() || !BuildOk(join.build) || !NameOk(join.name) || !ColourOk(join.colour) ||
        !MachineOk(join.machine)) {
        return false;
    }
    out = join;
    return true;
}

bool DecodeWelcome(const uint8_t* data, size_t len, uint8_t& out) {
    Reader r(data, len);
    const uint8_t id = r.U8();
    if (!r.Done() || id == kHostId || id >= kMaxPlayers) {
        return false;
    }
    out = id;
    return true;
}

bool DecodeReject(const uint8_t* data, size_t len, RejectReason& out) {
    Reader r(data, len);
    const uint8_t value = r.U8();
    if (!r.Done() || value < static_cast<uint8_t>(RejectReason::Build) ||
        value > static_cast<uint8_t>(RejectReason::Kicked)) {
        return false;
    }
    out = static_cast<RejectReason>(value);
    return true;
}

bool DecodeRoster(const uint8_t* data, size_t len, std::vector<PlayerInfo>& out) {
    Reader r(data, len);
    const size_t count = r.U8();
    if (!r.Ok() || count == 0 || count > kMaxPlayers) {
        return false;
    }
    std::vector<PlayerInfo> roster;
    std::array<bool, kMaxPlayers> seen{};
    size_t hosts = 0;
    for (size_t i = 0; i < count; i++) {
        PlayerInfo p;
        p.id = r.U8();
        const uint8_t flags = r.U8();
        p.colour = r.U32();
        p.machine = r.U8();
        if (!r.Str8(kNameMax, p.name)) {
            return false;
        }
        if (p.id >= kMaxPlayers || seen[p.id] || (flags & ~(kFlagHost | kFlagReady)) != 0 || !NameOk(p.name) ||
            !ColourOk(p.colour) || !MachineOk(p.machine)) {
            return false;
        }
        seen[p.id] = true;
        p.host = (flags & kFlagHost) != 0;
        p.ready = (flags & kFlagReady) != 0;
        if (p.host != (p.id == kHostId)) {
            return false;
        }
        hosts += p.host ? 1 : 0;
        roster.push_back(p);
    }
    if (!r.Done() || hosts != 1) {
        return false;
    }
    out = roster;
    return true;
}

bool DecodeReady(const uint8_t* data, size_t len, bool& out) {
    Reader r(data, len);
    const uint8_t value = r.U8();
    if (!r.Done() || value > 1) {
        return false;
    }
    out = value == 1;
    return true;
}

bool DecodeChat(const uint8_t* data, size_t len, std::string& out) {
    Reader r(data, len);
    std::string text;
    if (!r.Str16(kChatMax, text) || !r.Done() || text.empty() || SanitizeChat(text) != text) {
        return false;
    }
    out = text;
    return true;
}

bool DecodeChatLine(const uint8_t* data, size_t len, ChatLine& out) {
    Reader r(data, len);
    ChatLine line;
    line.from = r.U8();
    if (!r.Str16(kChatMax, line.text) || !r.Done() || line.text.empty() || SanitizeChat(line.text) != line.text ||
        (line.from >= kMaxPlayers && line.from != kSystemId)) {
        return false;
    }
    out = line;
    return true;
}

bool IsDiscovery(const uint8_t* data, size_t len) {
    return data != nullptr && len >= 5 && data[0] == kDiscoveryMagic[0] && data[1] == kDiscoveryMagic[1] &&
           data[2] == kDiscoveryMagic[2] && data[3] == kDiscoveryMagic[3];
}

std::vector<uint8_t> EncodeProbe(uint32_t nonce) {
    Writer w;
    w.Raw(kDiscoveryMagic, sizeof(kDiscoveryMagic));
    w.U8(kKindProbe);
    w.U16(kLobbyVersion);
    w.U32(nonce);
    return w.Take();
}

std::vector<uint8_t> EncodeRoom(uint32_t nonce, const RoomInfo& room) {
    Writer w;
    w.Raw(kDiscoveryMagic, sizeof(kDiscoveryMagic));
    w.U8(kKindRoom);
    w.U16(kLobbyVersion);
    w.U32(nonce);
    w.U16(room.port);
    w.U8(room.players);
    w.U8(room.cap);
    w.U8(room.open ? 1 : 0);
    w.Str8(room.build);
    w.Str8(room.hostName);
    return w.Take();
}

bool DecodeProbe(const uint8_t* data, size_t len, uint32_t& nonce) {
    if (!IsDiscovery(data, len)) {
        return false;
    }
    Reader r(data + 4, len - 4);
    const uint8_t kind = r.U8();
    const uint16_t version = r.U16();
    const uint32_t value = r.U32();
    if (!r.Done() || kind != kKindProbe || version != kLobbyVersion) {
        return false;
    }
    nonce = value;
    return true;
}

bool DecodeRoom(const uint8_t* data, size_t len, uint32_t& nonce, RoomInfo& out) {
    if (!IsDiscovery(data, len)) {
        return false;
    }
    Reader r(data + 4, len - 4);
    const uint8_t kind = r.U8();
    const uint16_t version = r.U16();
    const uint32_t value = r.U32();
    RoomInfo room;
    room.port = r.U16();
    room.players = r.U8();
    room.cap = r.U8();
    const uint8_t open = r.U8();
    if (!r.Str8(kBuildMax, room.build) || !r.Str8(kNameMax, room.hostName) || !r.Done()) {
        return false;
    }
    if (kind != kKindRoom || version != kLobbyVersion || room.port == 0 || open > 1 || room.cap == 0 ||
        room.cap > kMaxPlayers || room.players == 0 || room.players > room.cap || !BuildOk(room.build) ||
        !NameOk(room.hostName)) {
        return false;
    }
    room.open = open == 1;
    nonce = value;
    out = room;
    return true;
}

} // namespace gdx::net::lobby
