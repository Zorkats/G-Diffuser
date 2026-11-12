// port/net/gdx_net_lobby.cpp: LAN room wire format, discovery and chat filter (gdx_net_lobby.h).

#include "gdx_net_lobby.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace gdx::net::lobby {

namespace {

constexpr uint8_t kFlagHost = 1;
constexpr uint8_t kFlagReady = 2;
constexpr uint8_t kFlagWatcher = 4;
constexpr uint8_t kFlagRaceNext = 8;
constexpr uint8_t kFlagInRace = 16;
constexpr uint8_t kFlagLocked = 32;
constexpr uint8_t kFlagsAll = 63;

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

    void U64(uint64_t v) {
        U32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
        U32(static_cast<uint32_t>(v >> 32));
    }

    // u8 length + bytes; a string above 255 bytes is cut, which the decoder's own limit then refuses.
    void Str8(const std::string& s) {
        const size_t n = s.size() > 255 ? 255 : s.size();
        U8(static_cast<uint8_t>(n));
        bytes_.insert(bytes_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }

    void Raw(const uint8_t* data, size_t n) {
        bytes_.insert(bytes_.end(), data, data + n);
    }

    // 16 bytes in network order, as NetAddress holds them.
    void Ip(const IpBytes& ip) {
        Raw(ip.data(), ip.size());
    }

    void B32(const Bytes32& bytes) {
        Raw(bytes.data(), bytes.size());
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

    uint64_t U64() {
        const uint64_t lo = U32();
        const uint64_t hi = U32();
        return lo | (hi << 32);
    }

    IpBytes Ip() {
        IpBytes ip{};
        if (Need(ip.size())) {
            std::memcpy(ip.data(), data_ + pos_, ip.size());
            pos_ += ip.size();
        }
        return ip;
    }

    Bytes32 B32() {
        Bytes32 bytes{};
        if (Need(bytes.size())) {
            std::memcpy(bytes.data(), data_ + pos_, bytes.size());
            pos_ += bytes.size();
        }
        return bytes;
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

// A track byte and, after kTrackCustom, the identity and the title (the header's "track").
void WriteTrack(Writer& w, uint8_t track, const CustomTrack& custom) {
    w.U8(track);
    if (track == kTrackCustom) {
        w.Raw(custom.id.data(), custom.id.size());
        w.Str8(custom.title);
    }
}

// false on a short read or a bad title. custom is left empty for any track but kTrackCustom, so two
// decodes of one stock track compare equal.
bool ReadTrack(Reader& r, uint8_t& track, CustomTrack& custom) {
    track = r.U8();
    custom = CustomTrack();
    if (!r.Ok()) {
        return false;
    }
    if (track != kTrackCustom) {
        return true;
    }
    for (uint8_t& b : custom.id) {
        b = r.U8();
    }
    return r.Str8(kTrackTitleMax, custom.title) && TrackTitleOk(custom.title);
}

// A u8 0 or 1, and after a 1 the 32 bytes of the record.
void WriteRecord(Writer& w, bool hasRecord, const MachineRecord& record) {
    w.U8(hasRecord ? 1 : 0);
    if (hasRecord) {
        w.Raw(record.data(), record.size());
    }
}

bool ReadRecord(Reader& r, bool& hasRecord, MachineRecord& record) {
    const uint8_t flag = r.U8();
    record = MachineRecord{};
    if (!r.Ok() || flag > 1) {
        return false;
    }
    hasRecord = flag == 1;
    if (hasRecord) {
        for (uint8_t& b : record) {
            b = r.U8();
        }
    }
    return r.Ok() && (!hasRecord || MachineRecordOk(record));
}

// What a ballot or a pick may name on the wire: a stock track, Random, or a custom track.
bool BallotTrackOk(uint8_t track) {
    return TrackInPool(track, kPoolAll) || track == kTrackCustom;
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
    case RejectReason::RaceSeatsFull:
        return "Every race seat is taken. You can still join to watch.";
    case RejectReason::WatchSeatsFull:
        return "Both watch seats are taken. You can still join to race.";
    case RejectReason::Hack:
        return "The host runs a different game: a ROM hack on one copy and not the other, or two different hacks. "
               "Both players need the same one.";
    case RejectReason::Rejoin:
        return "The new host could not take you back into the room.";
    case RejectReason::Banned:
        return "The host banned you from this room.";
    case RejectReason::NotFriend:
        return "This room is for the friends of its host, and your friend code is not on their list.";
    case RejectReason::BadProof:
        return "The host could not check your profile key.";
    }
    return "The host refused the join.";
}

const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Lobby:
        return "Lobby";
    case Phase::Voting:
        return "Voting";
    case Phase::Lottery:
        return "Lottery";
    case Phase::Pick:
        return "Track pick";
    case Phase::Machine:
        return "Machine select";
    case Phase::GridSync:
        return "Loading";
    case Phase::Countdown:
        return "Countdown";
    case Phase::Racing:
        return "Racing";
    case Phase::Results:
        return "Results";
    case Phase::Assets:
        return "Getting the track";
    }
    return "";
}

const char* RoomTypeName(RoomType type) {
    switch (type) {
    case RoomType::Lan:
        return "LAN";
    case RoomType::Code:
        return "Code";
    case RoomType::Open:
        return "Open";
    case RoomType::Friends:
        return "Friends";
    }
    return "";
}

bool TrackTitleOk(const std::string& title) {
    if (title.empty() || title.size() > kTrackTitleMax) {
        return false;
    }
    for (char c : title) {
        if (!Printable(c)) {
            return false;
        }
    }
    return true;
}

bool HackOk(const HackIdentity& hack) {
    if (hack.name.empty()) {
        for (uint8_t b : hack.hash) {
            if (b != 0) {
                return false;
            }
        }
        return true;
    }
    if (hack.name.size() > kHackNameMax) {
        return false;
    }
    for (char c : hack.name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return false;
        }
    }
    return true;
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

bool TrackInPool(uint8_t track, uint32_t pool) {
    return track == kTrackRandom || (track < kStockTracks && ((pool >> track) & 1u) != 0);
}

bool PoolOk(uint32_t pool) {
    return pool != 0 && (pool & ~kPoolAll) == 0;
}

const char* EmoteText(uint8_t emote) {
    static const char* const kText[kEmoteCount] = {
        "Hello!", "Good luck!", "Good race!", "Nice one!", "Again?", "Thanks!", "Sorry!", "I have to go.",
    };
    return emote < kEmoteCount ? kText[emote] : "";
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
    w.U8(static_cast<uint8_t>(join.seat));
    w.Str8(join.hack.name);
    w.Raw(join.hack.hash.data(), join.hack.hash.size());
    w.Raw(join.key.data(), join.key.size());
    w.Raw(join.proof.data(), join.proof.size());
    w.U8(join.reachable ? 1 : 0);
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
        w.U8(static_cast<uint8_t>((p.host ? kFlagHost : 0) | (p.ready ? kFlagReady : 0) |
                                  (p.watcher ? kFlagWatcher : 0) | (p.raceNext ? kFlagRaceNext : 0) |
                                  (p.inRace ? kFlagInRace : 0) | (p.locked ? kFlagLocked : 0)));
        w.U32(p.colour);
        w.U8(p.machine);
        w.U8(p.skin);
        w.U16(p.pingMs);
        w.U16(p.points);
        w.Str8(p.name);
    }
    return w.Take();
}

std::vector<uint8_t> EncodeReady(bool ready) {
    Writer w;
    w.U8(ready ? 1 : 0);
    return w.Take();
}

namespace {

constexpr uint8_t kRuleIgnoreTrackSettings = 1 << 0;
constexpr uint8_t kRuleLavaNoKill = 1 << 1;
constexpr uint8_t kRuleFlagsAll = kRuleIgnoreTrackSettings | kRuleLavaNoKill;

constexpr uint8_t kCpuFill = 1 << 0;
constexpr uint8_t kCpuTrackHazards = 1 << 1;
constexpr uint8_t kCpuFlagsAll = kCpuFill | kCpuTrackHazards;

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// u8 laps, u8 rule flags, u32 magnet range, u32 magnet strength (float bits), u32 boost frames: the rules
// as Grid and Room both carry them.
void WriteRules(Writer& w, const RaceRules& rules) {
    w.U8(rules.laps);
    w.U8(static_cast<uint8_t>((rules.ignoreTrackSettings ? kRuleIgnoreTrackSettings : 0) |
                              (rules.lavaNoKill ? kRuleLavaNoKill : 0)));
    w.U32(FloatBits(rules.magnetRange));
    w.U32(FloatBits(rules.magnetStrength));
    w.U32(rules.boostFrames);
}

// False on a short read, an unknown flag or rules outside their limits.
bool ReadRules(Reader& r, RaceRules& rules) {
    rules.laps = r.U8();
    const uint8_t flags = r.U8();
    rules.magnetRange = BitsFloat(r.U32());
    rules.magnetStrength = BitsFloat(r.U32());
    rules.boostFrames = r.U32();
    if (!r.Ok() || (flags & ~kRuleFlagsAll) != 0) {
        return false;
    }
    rules.ignoreTrackSettings = (flags & kRuleIgnoreTrackSettings) != 0;
    rules.lavaNoKill = (flags & kRuleLavaNoKill) != 0;
    return RulesOk(rules);
}

// A member other than the host with a port to reach it on, or nobody with no address at all.
bool SuccessorOk(uint8_t successor, const IpBytes& ip, uint16_t port) {
    if (successor == kSystemId) {
        return IsZeroIp(ip) && port == 0;
    }
    return successor != kHostId && successor < kMaxPlayers && port != 0;
}

} // namespace

std::vector<uint8_t> EncodeRoomState(const RoomState& room) {
    Writer w;
    w.U32(room.roomId);
    w.U16(room.raceIndex);
    w.U8(static_cast<uint8_t>(room.phase));
    w.U32(room.phaseLeftMs);
    w.U32(room.phaseTotalMs);
    w.U8(room.racerCap);
    w.U32(room.pool);
    WriteTrack(w, room.track, room.custom);
    w.U8(room.lastTrack);
    w.U8(room.picker);
    w.U8(room.locked);
    w.U8(room.racers);
    w.U8(room.cpusOut ? 1 : 0);
    w.U8(room.customTracks ? 1 : 0);
    w.U8(static_cast<uint8_t>((room.cpuFill ? kCpuFill : 0) | (room.cpuTrackHazards ? kCpuTrackHazards : 0)));
    w.U16(room.aiAggression);
    WriteRules(w, room.rules);
    w.U8(room.successor);
    w.Ip(room.successorIp);
    w.U16(room.successorPort);
    w.U8(static_cast<uint8_t>(room.type));
    w.B32(room.commitment);
    w.U32(room.code);
    return w.Take();
}

std::vector<uint8_t> EncodeVote(uint8_t track) {
    Writer w;
    w.U8(track);
    return w.Take();
}

std::vector<uint8_t> EncodeVote(const TrackChoice& choice) {
    Writer w;
    WriteTrack(w, choice.track, choice.custom);
    return w.Take();
}

namespace {

void WriteBallots(Writer& w, const std::vector<Ballot>& ballots) {
    w.U8(static_cast<uint8_t>(ballots.size() > 255 ? 255 : ballots.size()));
    for (const Ballot& b : ballots) {
        w.U8(b.id);
        WriteTrack(w, b.track, b.custom);
    }
}

// Ballots as DecodeVotes documents them. false leaves out in an unspecified state.
bool ReadBallots(Reader& r, std::vector<Ballot>& out) {
    const size_t count = r.U8();
    if (!r.Ok() || count > kMaxRacers) {
        return false;
    }
    out.clear();
    for (size_t i = 0; i < count; i++) {
        Ballot b;
        b.id = r.U8();
        if (!ReadTrack(r, b.track, b.custom) || b.id >= kMaxPlayers || (!out.empty() && b.id <= out.back().id) ||
            !BallotTrackOk(b.track)) {
            return false;
        }
        out.push_back(b);
    }
    return true;
}

} // namespace

std::vector<uint8_t> EncodeVotes(const std::vector<Ballot>& ballots) {
    Writer w;
    WriteBallots(w, ballots);
    return w.Take();
}

std::vector<uint8_t> EncodeLottery(const LotteryResult& lottery) {
    Writer w;
    WriteBallots(w, lottery.ballots);
    w.U8(lottery.winner);
    WriteTrack(w, lottery.track, lottery.custom);
    w.B32(lottery.salt);
    return w.Take();
}

std::vector<uint8_t> EncodePick(uint8_t track) {
    Writer w;
    w.U8(track);
    return w.Take();
}

std::vector<uint8_t> EncodePick(const TrackChoice& choice) {
    return EncodeVote(choice);
}

std::vector<uint8_t> EncodePicked(const Picked& picked) {
    Writer w;
    WriteTrack(w, picked.track, picked.custom);
    w.U8(picked.drawn ? 1 : 0);
    w.U8(picked.by);
    return w.Take();
}

std::vector<uint8_t> EncodeLock(const LockRequest& lock) {
    Writer w;
    w.U8(lock.machine);
    w.U8(lock.skin);
    w.U8(lock.engine);
    WriteRecord(w, lock.hasRecord, lock.record);
    return w.Take();
}

std::vector<uint8_t> EncodeEmote(uint8_t emote) {
    Writer w;
    w.U8(emote);
    return w.Take();
}

std::vector<uint8_t> EncodeEmoteLine(const EmoteLine& line) {
    Writer w;
    w.U8(line.from);
    w.U8(line.emote);
    return w.Take();
}

std::vector<uint8_t> EncodeSeatNext(bool raceNext) {
    Writer w;
    w.U8(raceNext ? 1 : 0);
    return w.Take();
}

// The offsets and limits come from the decomp: the layout of CustomMachine (unk_structs.h) and the tables
// func_8008D33C indexes with each field (racer.c).
bool MachineRecordOk(const MachineRecord& record) {
    constexpr size_t kBody = 0x00, kBoost = 0x01, kGrip = 0x02;
    constexpr size_t kFront = 0x03, kRear = 0x04, kWing = 0x05;
    constexpr size_t kLogo = 0x06, kDecal = 0x08;
    constexpr size_t kName = 0x15, kNameBytes = 9;
    // Grades A to E are 0 to 4: the game stores BODY_E - grade (racer.c). Machine Create refuses a machine
    // whose weightings sum past 13 (machine_create_stats.c, sMachineStatWeightings and
    // MACHINE_STAT_MAX_WEIGHTING), and so does the room.
    constexpr uint8_t kGradeMax = 4;
    constexpr uint8_t kWeight[3][kGradeMax + 1] = { { 0, 1, 2, 4, 5 }, { 0, 1, 2, 6, 8 }, { 0, 1, 2, 6, 8 } };
    constexpr int kWeightMax = 13;
    // Seven of each part: the weight tables D_800CF488, D_800CF498 and D_800CF4A8, and the shadow table
    // indexed front * 7 + rear.
    constexpr uint8_t kPartMax = 6;
    // Stored one above the option (MACHINE_LOGO, MACHINE_DECAL in fzx_machine.h): 8 logos and 5 decals.
    constexpr uint8_t kLogoMax = 8, kDecalMax = 5;
    // Not checked: the colours, where every byte is a colour; the number, which the fill takes from the
    // stock machine instead; and the checksum, which the game writes before it changes the number
    // (machine_create_update.c), so a slot the game made itself can fail it.
    const uint8_t body = record[kBody], boost = record[kBoost], grip = record[kGrip];
    if (body > kGradeMax || boost > kGradeMax || grip > kGradeMax ||
        kWeight[0][body] + kWeight[1][boost] + kWeight[2][grip] > kWeightMax) {
        return false;
    }
    if (record[kFront] > kPartMax || record[kRear] > kPartMax || record[kWing] > kPartMax) {
        return false;
    }
    if (record[kLogo] < 1 || record[kLogo] > kLogoMax || record[kDecal] < 1 || record[kDecal] > kDecalMax) {
        return false;
    }
    const auto name = record.begin() + kName;
    return std::find(name, name + kNameBytes, uint8_t{ 0 }) != name + kNameBytes;
}

bool RulesOk(const RaceRules& rules) {
    // Every comparison is false for a NaN, so one never passes.
    return rules.laps >= 1 && rules.laps <= kLapsMax && rules.magnetRange >= kMagnetRangeMin &&
           rules.magnetRange <= kMagnetRangeMax && rules.magnetStrength >= kMagnetStrengthMin &&
           rules.magnetStrength <= kMagnetStrengthMax && rules.boostFrames <= kBoostFramesMax;
}

std::vector<uint8_t> EncodeReadyToRace(const ReadyToRace& ready) {
    Writer w;
    w.U16(ready.raceIndex);
    w.Raw(ready.digest.data(), ready.digest.size());
    w.U16(ready.loadMs);
    return w.Take();
}

std::vector<uint8_t> EncodeGrid(const Grid& grid) {
    Writer w;
    w.U16(grid.raceIndex);
    w.U32(grid.seed);
    WriteRules(w, grid.rules);
    WriteTrack(w, grid.track.track, grid.track.custom);
    w.U8(grid.cpus);
    w.U8(static_cast<uint8_t>(grid.humans.size() > 255 ? 255 : grid.humans.size()));
    for (const GridEntry& e : grid.humans) {
        w.U8(e.id);
        w.U8(e.machine);
        w.U8(e.skin);
        WriteRecord(w, e.hasRecord, e.record);
    }
    return w.Take();
}

std::vector<uint8_t> EncodeStart(const StartSignal& start) {
    Writer w;
    w.U16(start.raceIndex);
    w.U32(start.startAtMs);
    return w.Take();
}

std::vector<uint8_t> EncodeClockPing(uint32_t clientMs) {
    Writer w;
    w.U32(clientMs);
    return w.Take();
}

std::vector<uint8_t> EncodeClockPong(const ClockPong& pong) {
    Writer w;
    w.U32(pong.clientMs);
    w.U32(pong.hostMs);
    return w.Take();
}

bool StatePoseOk(const StatePose& pose) {
    const auto within = [](const std::array<float, 3>& v, float limit) {
        for (float value : v) {
            if (!std::isfinite(value) || std::fabs(value) > limit) {
                return false;
            }
        }
        return true;
    };
    return pose.slot < kGridSlots && within(pose.pos, kStatePosMax) && within(pose.vel, kStateVelMax) &&
           within(pose.forward, kStateAxisMax) && within(pose.up, kStateAxisMax) && pose.lap >= kStateLapMin &&
           pose.lap <= kStateLapMax && pose.attack <= kStateAttackMax;
}

std::vector<uint8_t> EncodeState(const RaceState& state) {
    const size_t count = state.poses.size() > kStatePoses ? kStatePoses : state.poses.size();
    Writer w;
    w.U16(state.raceIndex);
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        const StatePose& pose = state.poses[i];
        w.U8(pose.slot);
        w.U32(pose.tick);
        w.U32(pose.timeMs);
        for (float value : pose.pos) {
            w.U32(FloatBits(value));
        }
        for (float value : pose.vel) {
            w.U32(FloatBits(value));
        }
        for (float value : pose.forward) {
            w.U32(FloatBits(value));
        }
        for (float value : pose.up) {
            w.U32(FloatBits(value));
        }
        w.U16(static_cast<uint16_t>(pose.lap));
        w.U32(pose.flags);
        w.U8(pose.attack);
    }
    return w.Take();
}

uint8_t PlacePoints(size_t place) {
    static const uint8_t kPoints[kMaxRacers] = { 100, 93, 87, 81, 76, 71, 66, 62, 58, 54, 50, 47 };
    return place >= 1 && place <= kMaxRacers ? kPoints[place - 1] : static_cast<uint8_t>(0);
}

std::vector<uint8_t> EncodeFinish(const FinishReport& report) {
    Writer w;
    w.U16(report.raceIndex);
    w.U8(static_cast<uint8_t>(report.kind));
    w.U32(report.timeMs);
    return w.Take();
}

std::vector<uint8_t> EncodeResults(const RaceResults& results) {
    const size_t count = results.entries.size() > 255 ? 255 : results.entries.size();
    Writer w;
    w.U16(results.raceIndex);
    w.U8(results.final ? 1 : 0);
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        const ResultEntry& e = results.entries[i];
        w.U8(e.id);
        w.U8(static_cast<uint8_t>(e.kind));
        w.U32(e.timeMs);
        w.U8(e.points);
    }
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
    const uint8_t seat = r.U8();
    if (!r.Str8(kHackNameMax, join.hack.name)) {
        return false;
    }
    for (uint8_t& b : join.hack.hash) {
        b = r.U8();
    }
    join.key = r.B32();
    bool proofZero = true;
    for (uint8_t& b : join.proof) {
        b = r.U8();
        proofZero = proofZero && b == 0;
    }
    const uint8_t reachable = r.U8();
    if (!r.Done() || reachable > 1 || !BuildOk(join.build) || !NameOk(join.name) || !ColourOk(join.colour) ||
        !MachineOk(join.machine) || seat > static_cast<uint8_t>(Seat::Watch) || !HackOk(join.hack)) {
        return false;
    }
    if (IsZero32(join.key) && !proofZero) {
        return false; // a proof with no key to check it under
    }
    join.seat = static_cast<Seat>(seat);
    join.reachable = reachable == 1;
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
        value > static_cast<uint8_t>(RejectReason::BadProof)) {
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
    size_t racers = 0;
    size_t watchers = 0;
    for (size_t i = 0; i < count; i++) {
        PlayerInfo p;
        p.id = r.U8();
        const uint8_t flags = r.U8();
        p.colour = r.U32();
        p.machine = r.U8();
        p.skin = r.U8();
        p.pingMs = r.U16();
        p.points = r.U16();
        if (!r.Str8(kNameMax, p.name)) {
            return false;
        }
        if (p.id >= kMaxPlayers || seen[p.id] || (flags & ~kFlagsAll) != 0 || !NameOk(p.name) ||
            !ColourOk(p.colour) || !MachineOk(p.machine) || p.skin >= kMachineSkins) {
            return false;
        }
        seen[p.id] = true;
        p.host = (flags & kFlagHost) != 0;
        p.ready = (flags & kFlagReady) != 0;
        p.watcher = (flags & kFlagWatcher) != 0;
        p.raceNext = (flags & kFlagRaceNext) != 0;
        p.inRace = (flags & kFlagInRace) != 0;
        p.locked = (flags & kFlagLocked) != 0;
        if (p.host != (p.id == kHostId) || (p.host && p.watcher) || (p.raceNext && !p.watcher) ||
            (p.inRace && p.watcher) || (p.locked && !p.inRace)) {
            return false;
        }
        hosts += p.host ? 1 : 0;
        racers += p.watcher ? 0 : 1;
        watchers += p.watcher ? 1 : 0;
        roster.push_back(p);
    }
    if (!r.Done() || hosts != 1 || racers > kMaxRacers || watchers > kMaxWatchers) {
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

bool DecodeRoomState(const uint8_t* data, size_t len, RoomState& out) {
    Reader r(data, len);
    RoomState room;
    room.roomId = r.U32();
    room.raceIndex = r.U16();
    const uint8_t phase = r.U8();
    room.phaseLeftMs = r.U32();
    room.phaseTotalMs = r.U32();
    room.racerCap = r.U8();
    room.pool = r.U32();
    if (!ReadTrack(r, room.track, room.custom)) {
        return false;
    }
    room.lastTrack = r.U8();
    room.picker = r.U8();
    room.locked = r.U8();
    room.racers = r.U8();
    const uint8_t cpusOut = r.U8();
    const uint8_t customTracks = r.U8();
    const uint8_t cpuFlags = r.U8();
    room.aiAggression = r.U16();
    if (!ReadRules(r, room.rules)) {
        return false;
    }
    room.successor = r.U8();
    room.successorIp = r.Ip();
    room.successorPort = r.U16();
    const uint8_t type = r.U8();
    room.commitment = r.B32();
    room.code = r.U32();
    const auto trackOrNone = [](uint8_t t) { return t < kStockTracks || t == kTrackNone || t == kTrackCustom; };
    if (!r.Done() || phase >= kPhaseCount || room.phaseLeftMs > room.phaseTotalMs ||
        room.racerCap < kDefaultRacers || room.racerCap > kMaxRacers || !PoolOk(room.pool) ||
        !trackOrNone(room.track) || !trackOrNone(room.lastTrack) ||
        (room.picker >= kMaxPlayers && room.picker != kSystemId) || room.racers > kMaxRacers ||
        room.locked > room.racers || cpusOut > 1 || customTracks > 1 || (cpuFlags & ~kCpuFlagsAll) != 0 ||
        room.aiAggression < kAiAggressionMin || room.aiAggression > kAiAggressionMax ||
        !SuccessorOk(room.successor, room.successorIp, room.successorPort) || type >= kRoomTypeCount ||
        (type != static_cast<uint8_t>(RoomType::Open) && !IsZero32(room.commitment)) ||
        (room.code != kNoRoomCode &&
         (room.code >= kRoomCodeValues || type == static_cast<uint8_t>(RoomType::Lan)))) {
        return false;
    }
    room.phase = static_cast<Phase>(phase);
    room.type = static_cast<RoomType>(type);
    room.cpusOut = cpusOut == 1;
    room.customTracks = customTracks == 1;
    room.cpuFill = (cpuFlags & kCpuFill) != 0;
    room.cpuTrackHazards = (cpuFlags & kCpuTrackHazards) != 0;
    out = room;
    return true;
}

bool DecodeVote(const uint8_t* data, size_t len, uint8_t& out) {
    Reader r(data, len);
    const uint8_t track = r.U8();
    if (!r.Done() || !TrackInPool(track, kPoolAll)) {
        return false;
    }
    out = track;
    return true;
}

bool DecodeVote(const uint8_t* data, size_t len, TrackChoice& out) {
    Reader r(data, len);
    TrackChoice choice;
    if (!ReadTrack(r, choice.track, choice.custom) || !r.Done() || !BallotTrackOk(choice.track)) {
        return false;
    }
    out = choice;
    return true;
}

bool DecodeVotes(const uint8_t* data, size_t len, std::vector<Ballot>& out) {
    Reader r(data, len);
    std::vector<Ballot> ballots;
    if (!ReadBallots(r, ballots) || !r.Done()) {
        return false;
    }
    out = ballots;
    return true;
}

bool DecodeLottery(const uint8_t* data, size_t len, LotteryResult& out) {
    Reader r(data, len);
    LotteryResult lottery;
    if (!ReadBallots(r, lottery.ballots)) {
        return false;
    }
    lottery.winner = r.U8();
    const bool trackOk = ReadTrack(r, lottery.track, lottery.custom);
    lottery.salt = r.B32();
    if (!trackOk || !r.Done() ||
        (lottery.track >= kStockTracks && lottery.track != kTrackCustom)) {
        return false;
    }
    // The winner is one of the ballots, and a ballot that named a track won with that track. A draw (no
    // ballot, or a Random one) is always a stock track: the host has no custom track to draw from.
    if (lottery.ballots.empty()) {
        if (lottery.winner != kNoWinner || lottery.track == kTrackCustom) {
            return false;
        }
    } else {
        if (lottery.winner >= lottery.ballots.size()) {
            return false;
        }
        const Ballot& won = lottery.ballots[lottery.winner];
        if (won.track == kTrackRandom ? lottery.track == kTrackCustom
                                      : won.track != lottery.track || won.custom != lottery.custom) {
            return false;
        }
    }
    out = lottery;
    return true;
}

bool DecodePick(const uint8_t* data, size_t len, uint8_t& out) {
    return DecodeVote(data, len, out);
}

bool DecodePick(const uint8_t* data, size_t len, TrackChoice& out) {
    return DecodeVote(data, len, out);
}

bool DecodePicked(const uint8_t* data, size_t len, Picked& out) {
    Reader r(data, len);
    Picked picked;
    if (!ReadTrack(r, picked.track, picked.custom)) {
        return false;
    }
    const uint8_t drawn = r.U8();
    picked.by = r.U8();
    if (!r.Done() || (picked.track >= kStockTracks && picked.track != kTrackCustom) || drawn > 1 ||
        (picked.by >= kMaxPlayers && picked.by != kSystemId)) {
        return false;
    }
    // Only a player picks a custom track: the host draws from the stock courses alone.
    if (picked.track == kTrackCustom && (drawn == 1 || picked.by == kSystemId)) {
        return false;
    }
    picked.drawn = drawn == 1;
    out = picked;
    return true;
}

bool DecodeLock(const uint8_t* data, size_t len, LockRequest& out) {
    Reader r(data, len);
    LockRequest lock;
    lock.machine = r.U8();
    lock.skin = r.U8();
    lock.engine = r.U8();
    if (!ReadRecord(r, lock.hasRecord, lock.record) || !r.Done() || lock.machine >= kRosterMachines ||
        lock.skin >= kMachineSkins || lock.engine > kEngineMax) {
        return false;
    }
    out = lock;
    return true;
}

bool DecodeEmote(const uint8_t* data, size_t len, uint8_t& out) {
    Reader r(data, len);
    const uint8_t emote = r.U8();
    if (!r.Done() || emote >= kEmoteCount) {
        return false;
    }
    out = emote;
    return true;
}

bool DecodeEmoteLine(const uint8_t* data, size_t len, EmoteLine& out) {
    Reader r(data, len);
    EmoteLine line;
    line.from = r.U8();
    line.emote = r.U8();
    if (!r.Done() || line.from >= kMaxPlayers || line.emote >= kEmoteCount) {
        return false;
    }
    out = line;
    return true;
}

bool DecodeSeatNext(const uint8_t* data, size_t len, bool& out) {
    return DecodeReady(data, len, out);
}

bool DecodeReadyToRace(const uint8_t* data, size_t len, ReadyToRace& out) {
    Reader r(data, len);
    ReadyToRace ready;
    ready.raceIndex = r.U16();
    for (uint8_t& b : ready.digest) {
        b = r.U8();
    }
    ready.loadMs = r.U16();
    if (!r.Done()) {
        return false;
    }
    out = ready;
    return true;
}

bool DecodeGrid(const uint8_t* data, size_t len, Grid& out) {
    Reader r(data, len);
    Grid grid;
    grid.raceIndex = r.U16();
    grid.seed = r.U32();
    if (!ReadRules(r, grid.rules)) {
        return false;
    }
    if (!ReadTrack(r, grid.track.track, grid.track.custom) ||
        (grid.track.track >= kStockTracks && grid.track.track != kTrackCustom)) {
        return false;
    }
    grid.cpus = r.U8();
    const size_t count = r.U8();
    if (!r.Ok() || count == 0 || count > kMaxRacers || count + grid.cpus > kGridSlots) {
        return false;
    }
    uint32_t seen = 0;
    for (size_t i = 0; i < count; i++) {
        GridEntry e;
        e.id = r.U8();
        e.machine = r.U8();
        e.skin = r.U8();
        if (!ReadRecord(r, e.hasRecord, e.record) || e.id >= kMaxPlayers || ((seen >> e.id) & 1u) != 0 ||
            e.machine >= kRosterMachines || e.skin >= kMachineSkins) {
            return false;
        }
        seen |= 1u << e.id;
        grid.humans.push_back(e);
    }
    if (!r.Done()) {
        return false;
    }
    out = std::move(grid);
    return true;
}

bool DecodeStart(const uint8_t* data, size_t len, StartSignal& out) {
    Reader r(data, len);
    StartSignal start;
    start.raceIndex = r.U16();
    start.startAtMs = r.U32();
    if (!r.Done()) {
        return false;
    }
    out = start;
    return true;
}

bool DecodeClockPing(const uint8_t* data, size_t len, uint32_t& out) {
    Reader r(data, len);
    const uint32_t clientMs = r.U32();
    if (!r.Done()) {
        return false;
    }
    out = clientMs;
    return true;
}

bool DecodeClockPong(const uint8_t* data, size_t len, ClockPong& out) {
    Reader r(data, len);
    ClockPong pong;
    pong.clientMs = r.U32();
    pong.hostMs = r.U32();
    if (!r.Done()) {
        return false;
    }
    out = pong;
    return true;
}

bool DecodeState(const uint8_t* data, size_t len, RaceState& out) {
    Reader r(data, len);
    RaceState state;
    state.raceIndex = r.U16();
    const size_t count = r.U8();
    // The length follows from the count, so a wrong one is refused before any pose is read.
    if (!r.Ok() || count == 0 || count > kStatePoses || len != kStateHeaderBytes + count * kStatePoseBytes) {
        return false;
    }
    state.poses.reserve(count);
    for (size_t i = 0; i < count; i++) {
        StatePose pose;
        pose.slot = r.U8();
        pose.tick = r.U32();
        pose.timeMs = r.U32();
        for (float& value : pose.pos) {
            value = BitsFloat(r.U32());
        }
        for (float& value : pose.vel) {
            value = BitsFloat(r.U32());
        }
        for (float& value : pose.forward) {
            value = BitsFloat(r.U32());
        }
        for (float& value : pose.up) {
            value = BitsFloat(r.U32());
        }
        pose.lap = static_cast<int16_t>(r.U16());
        pose.flags = r.U32();
        pose.attack = r.U8();
        if (!r.Ok() || !StatePoseOk(pose)) {
            return false;
        }
        state.poses.push_back(pose);
    }
    if (!r.Done()) {
        return false;
    }
    out = std::move(state);
    return true;
}

bool DecodeFinish(const uint8_t* data, size_t len, FinishReport& out) {
    Reader r(data, len);
    FinishReport report;
    report.raceIndex = r.U16();
    const uint8_t kind = r.U8();
    report.timeMs = r.U32();
    if (!r.Done()) {
        return false;
    }
    if (kind == static_cast<uint8_t>(ResultKind::Finished)) {
        if (report.timeMs == 0 || report.timeMs > kRaceTimeMaxMs) {
            return false;
        }
    } else if (kind != static_cast<uint8_t>(ResultKind::Retired) || report.timeMs != 0) {
        return false; // nobody reports that they are still driving
    }
    report.kind = static_cast<ResultKind>(kind);
    out = report;
    return true;
}

bool DecodeResults(const uint8_t* data, size_t len, RaceResults& out) {
    Reader r(data, len);
    RaceResults results;
    results.raceIndex = r.U16();
    const uint8_t final = r.U8();
    const size_t count = r.U8();
    if (!r.Ok() || final > 1 || count > kMaxRacers) {
        return false;
    }
    results.final = final != 0;
    std::array<bool, kMaxPlayers> seen{};
    for (size_t i = 0; i < count; i++) {
        ResultEntry e;
        e.id = r.U8();
        const uint8_t kind = r.U8();
        e.timeMs = r.U32();
        e.points = r.U8();
        if (!r.Ok() || e.id >= kMaxPlayers || seen[e.id] || kind > static_cast<uint8_t>(ResultKind::Retired) ||
            e.points > kPointsMax) {
            return false;
        }
        seen[e.id] = true;
        e.kind = static_cast<ResultKind>(kind);
        if (e.kind == ResultKind::Finished ? (e.timeMs == 0 || e.timeMs > kRaceTimeMaxMs) : e.timeMs != 0) {
            return false;
        }
        if (e.kind == ResultKind::Retired && e.points != 0) {
            return false;
        }
        if (!results.entries.empty()) {
            const ResultEntry& before = results.entries.back();
            if (e.kind < before.kind || (e.kind == ResultKind::Finished && e.timeMs < before.timeMs)) {
                return false; // not in table order
            }
        }
        results.entries.push_back(e);
    }
    if (!r.Done()) {
        return false;
    }
    out = std::move(results);
    return true;
}

std::vector<uint8_t> EncodeHandoverToken(const HandoverToken& token) {
    Writer w;
    w.U64(token.token);
    return w.Take();
}

std::vector<uint8_t> EncodeHandoverTokens(const std::vector<HandoverTokenEntry>& tokens, const Bytes32& takeoverSecret,
                                          const BanList& bans) {
    Writer w;
    w.U8(static_cast<uint8_t>(tokens.size() > 255 ? 255 : tokens.size()));
    for (const HandoverTokenEntry& e : tokens) {
        w.U8(e.id);
        w.U64(e.token);
    }
    w.B32(takeoverSecret);
    // The newest kBanMax of each, so a list that grew past the cap still decodes.
    const size_t keyFrom = bans.keys.size() > kBanMax ? bans.keys.size() - kBanMax : 0;
    w.U8(static_cast<uint8_t>(bans.keys.size() - keyFrom));
    for (size_t i = keyFrom; i < bans.keys.size(); i++) {
        w.B32(bans.keys[i]);
    }
    const size_t prefixFrom = bans.prefixes.size() > kBanMax ? bans.prefixes.size() - kBanMax : 0;
    w.U8(static_cast<uint8_t>(bans.prefixes.size() - prefixFrom));
    for (size_t i = prefixFrom; i < bans.prefixes.size(); i++) {
        w.Ip(bans.prefixes[i]);
    }
    return w.Take();
}

std::vector<uint8_t> EncodeHandover(const Handover& handover) {
    Writer w;
    w.U8(handover.successor);
    w.Ip(handover.ip);
    w.U16(handover.port);
    return w.Take();
}

std::vector<uint8_t> EncodeRejoin(const Rejoin& rejoin) {
    Writer w;
    w.U8(rejoin.id);
    w.U64(rejoin.token);
    return w.Take();
}

bool DecodeHandoverToken(const uint8_t* data, size_t len, HandoverToken& out) {
    Reader r(data, len);
    const uint64_t token = r.U64();
    if (!r.Done() || token == 0) {
        return false;
    }
    out.token = token;
    return true;
}

bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out) {
    Bytes32 secret{};
    return DecodeHandoverTokens(data, len, out, secret);
}

bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out,
                          Bytes32& takeoverSecret) {
    BanList bans;
    return DecodeHandoverTokens(data, len, out, takeoverSecret, bans);
}

namespace {

// What BanPrefix (gdx_net_lobby_session.cpp) makes: a mapped IPv4 address that is not loopback, or an IPv6 /64.
// The IPv6 loopback ::1 has a host part, so it never passes.
bool BanPrefixOk(const IpBytes& ip) {
    if (IsMappedV4(ip)) {
        return ip[12] != 127;
    }
    for (size_t i = 8; i < ip.size(); i++) {
        if (ip[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

bool DecodeHandoverTokens(const uint8_t* data, size_t len, std::vector<HandoverTokenEntry>& out,
                          Bytes32& takeoverSecret, BanList& bans) {
    Reader r(data, len);
    const size_t count = r.U8();
    if (!r.Ok() || count > kMaxPlayers) {
        return false;
    }
    std::vector<HandoverTokenEntry> tokens;
    tokens.reserve(count);
    for (size_t i = 0; i < count; i++) {
        HandoverTokenEntry e;
        e.id = r.U8();
        e.token = r.U64();
        if (!r.Ok() || e.id >= kMaxPlayers || e.token == 0 || (!tokens.empty() && e.id <= tokens.back().id)) {
            return false;
        }
        tokens.push_back(e);
    }
    const Bytes32 secret = r.B32();
    BanList list;
    const size_t keyCount = r.U8();
    if (!r.Ok() || keyCount > kBanMax) {
        return false;
    }
    for (size_t i = 0; i < keyCount; i++) {
        const Bytes32 key = r.B32();
        if (!r.Ok() || IsZero32(key)) {
            return false;
        }
        list.keys.push_back(key);
    }
    const size_t prefixCount = r.U8();
    if (!r.Ok() || prefixCount > kBanMax) {
        return false;
    }
    for (size_t i = 0; i < prefixCount; i++) {
        const IpBytes prefix = r.Ip();
        if (!r.Ok() || !BanPrefixOk(prefix)) {
            return false;
        }
        list.prefixes.push_back(prefix);
    }
    if (!r.Done()) {
        return false;
    }
    out = std::move(tokens);
    takeoverSecret = secret;
    bans = std::move(list);
    return true;
}

bool DecodeHandover(const uint8_t* data, size_t len, Handover& out) {
    Reader r(data, len);
    Handover handover;
    handover.successor = r.U8();
    handover.ip = r.Ip();
    handover.port = r.U16();
    if (!r.Done() || !SuccessorOk(handover.successor, handover.ip, handover.port)) {
        return false;
    }
    out = handover;
    return true;
}

bool DecodeRejoin(const uint8_t* data, size_t len, Rejoin& out) {
    Reader r(data, len);
    Rejoin rejoin;
    rejoin.id = r.U8();
    rejoin.token = r.U64();
    if (!r.Done() || rejoin.id == kHostId || rejoin.id >= kMaxPlayers || rejoin.token == 0) {
        return false;
    }
    out = rejoin;
    return true;
}

std::vector<uint8_t> EncodeNonce(const JoinNonce& nonce) {
    Writer w;
    w.U32(nonce.roomId);
    w.Raw(nonce.hostKey.data(), nonce.hostKey.size());
    w.Raw(nonce.nonce.data(), nonce.nonce.size());
    return w.Take();
}

bool DecodeNonce(const uint8_t* data, size_t len, JoinNonce& out) {
    Reader r(data, len);
    JoinNonce nonce;
    nonce.roomId = r.U32();
    nonce.hostKey = r.B32();
    nonce.nonce = r.B32();
    if (!r.Done()) {
        return false;
    }
    out = nonce;
    return true;
}

std::vector<uint8_t> EncodeKeys(const std::vector<MemberKey>& keys) {
    Writer w;
    w.U8(static_cast<uint8_t>(keys.size() > kMaxPlayers ? kMaxPlayers : keys.size()));
    for (size_t i = 0; i < keys.size() && i < kMaxPlayers; i++) {
        w.U8(keys[i].id);
        w.Raw(keys[i].key.data(), keys[i].key.size());
    }
    return w.Take();
}

bool DecodeKeys(const uint8_t* data, size_t len, std::vector<MemberKey>& out) {
    Reader r(data, len);
    const uint8_t count = r.U8();
    if (count > kMaxPlayers) {
        return false;
    }
    std::vector<MemberKey> keys;
    for (uint8_t i = 0; i < count; i++) {
        MemberKey entry;
        entry.id = r.U8();
        entry.key = r.B32();
        if (entry.id >= kMaxPlayers || IsZero32(entry.key) || (!keys.empty() && entry.id <= keys.back().id)) {
            return false;
        }
        keys.push_back(entry);
    }
    if (!r.Done()) {
        return false;
    }
    out = std::move(keys);
    return true;
}

std::vector<uint8_t> EncodeReach(bool reachable) {
    Writer w;
    w.U8(reachable ? 1 : 0);
    return w.Take();
}

bool DecodeReach(const uint8_t* data, size_t len, bool& out) {
    Reader r(data, len);
    const uint8_t value = r.U8();
    if (!r.Done() || value > 1) {
        return false;
    }
    out = value == 1;
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
    std::vector<uint8_t> out = w.Take();
    out.resize(kProbeBytes, 0);
    return out;
}

std::vector<uint8_t> EncodeRoom(uint32_t nonce, const RoomInfo& room) {
    Writer w;
    w.Raw(kDiscoveryMagic, sizeof(kDiscoveryMagic));
    w.U8(kKindRoom);
    w.U16(kLobbyVersion);
    w.U32(nonce);
    w.U16(room.port);
    w.U8(room.racers);
    w.U8(room.racerCap);
    w.U8(room.watchers);
    w.U8(static_cast<uint8_t>(room.phase));
    w.Str8(room.build);
    w.Str8(room.hostName);
    return w.Take();
}

bool DecodeProbe(const uint8_t* data, size_t len, uint32_t& nonce) {
    if (!IsDiscovery(data, len)) {
        return false;
    }
    if (len != kProbeBytes) {
        return false;
    }
    Reader r(data + 4, len - 4);
    const uint8_t kind = r.U8();
    const uint16_t version = r.U16();
    const uint32_t value = r.U32();
    if (!r.Ok() || kind != kKindProbe || version != kLobbyVersion) {
        return false;
    }
    for (size_t i = 11; i < len; i++) {
        if (data[i] != 0) {
            return false;
        }
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
    room.racers = r.U8();
    room.racerCap = r.U8();
    room.watchers = r.U8();
    const uint8_t phase = r.U8();
    if (!r.Str8(kBuildMax, room.build) || !r.Str8(kNameMax, room.hostName) || !r.Done()) {
        return false;
    }
    // The host always holds a race seat, so a room has at least one racer.
    if (kind != kKindRoom || version != kLobbyVersion || room.port == 0 || phase >= kPhaseCount ||
        room.racerCap < kDefaultRacers || room.racerCap > kMaxRacers || room.racers == 0 ||
        room.racers > room.racerCap || room.watchers > kMaxWatchers || !BuildOk(room.build) ||
        !NameOk(room.hostName)) {
        return false;
    }
    room.phase = static_cast<Phase>(phase);
    nonce = value;
    out = room;
    return true;
}

} // namespace gdx::net::lobby
