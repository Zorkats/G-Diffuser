// port/net/gdx_net_lobby.cpp: LAN room wire format, discovery and chat filter (gdx_net_lobby.h).

#include "gdx_net_lobby.h"

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

    // u8 length + bytes; a string above 255 bytes is cut, which the decoder's own limit then refuses.
    void Str8(const std::string& s) {
        const size_t n = s.size() > 255 ? 255 : s.size();
        U8(static_cast<uint8_t>(n));
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
    case RejectReason::RaceSeatsFull:
        return "Every race seat is taken. You can still join to watch.";
    case RejectReason::WatchSeatsFull:
        return "Both watch seats are taken. You can still join to race.";
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
    }
    return "";
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

std::vector<uint8_t> EncodeRoomState(const RoomState& room) {
    Writer w;
    w.U32(room.roomId);
    w.U16(room.raceIndex);
    w.U8(static_cast<uint8_t>(room.phase));
    w.U32(room.phaseLeftMs);
    w.U32(room.phaseTotalMs);
    w.U8(room.racerCap);
    w.U32(room.pool);
    w.U8(room.track);
    w.U8(room.lastTrack);
    w.U8(room.picker);
    w.U8(room.locked);
    w.U8(room.racers);
    w.U8(room.cpusOut ? 1 : 0);
    return w.Take();
}

std::vector<uint8_t> EncodeVote(uint8_t track) {
    Writer w;
    w.U8(track);
    return w.Take();
}

namespace {

void WriteBallots(Writer& w, const std::vector<Ballot>& ballots) {
    w.U8(static_cast<uint8_t>(ballots.size() > 255 ? 255 : ballots.size()));
    for (const Ballot& b : ballots) {
        w.U8(b.id);
        w.U8(b.track);
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
        b.track = r.U8();
        if (!r.Ok() || b.id >= kMaxPlayers || (!out.empty() && b.id <= out.back().id) ||
            !TrackInPool(b.track, kPoolAll)) {
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
    w.U8(lottery.track);
    return w.Take();
}

std::vector<uint8_t> EncodePick(uint8_t track) {
    Writer w;
    w.U8(track);
    return w.Take();
}

std::vector<uint8_t> EncodePicked(const Picked& picked) {
    Writer w;
    w.U8(picked.track);
    w.U8(picked.drawn ? 1 : 0);
    w.U8(picked.by);
    return w.Take();
}

std::vector<uint8_t> EncodeLock(const LockRequest& lock) {
    Writer w;
    w.U8(lock.machine);
    w.U8(lock.skin);
    w.U8(lock.engine);
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

bool RulesOk(const RaceRules& rules) {
    // Every comparison is false for a NaN, so one never passes.
    return rules.laps >= 1 && rules.laps <= kLapsMax && rules.magnetRange >= kMagnetRangeMin &&
           rules.magnetRange <= kMagnetRangeMax && rules.magnetStrength >= kMagnetStrengthMin &&
           rules.magnetStrength <= kMagnetStrengthMax;
}

namespace {

constexpr uint8_t kRuleIgnoreTrackSettings = 1 << 0;
constexpr uint8_t kRuleLavaNoKill = 1 << 1;
constexpr uint8_t kRuleFlagsAll = kRuleIgnoreTrackSettings | kRuleLavaNoKill;

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

} // namespace

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
    w.U8(grid.rules.laps);
    w.U8(static_cast<uint8_t>((grid.rules.ignoreTrackSettings ? kRuleIgnoreTrackSettings : 0) |
                              (grid.rules.lavaNoKill ? kRuleLavaNoKill : 0)));
    w.U32(FloatBits(grid.rules.magnetRange));
    w.U32(FloatBits(grid.rules.magnetStrength));
    w.U8(grid.cpus);
    w.U8(static_cast<uint8_t>(grid.humans.size() > 255 ? 255 : grid.humans.size()));
    for (const GridEntry& e : grid.humans) {
        w.U8(e.id);
        w.U8(e.machine);
        w.U8(e.skin);
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
    if (!r.Done() || !BuildOk(join.build) || !NameOk(join.name) || !ColourOk(join.colour) ||
        !MachineOk(join.machine) || seat > static_cast<uint8_t>(Seat::Watch)) {
        return false;
    }
    join.seat = static_cast<Seat>(seat);
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
        value > static_cast<uint8_t>(RejectReason::WatchSeatsFull)) {
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
    room.track = r.U8();
    room.lastTrack = r.U8();
    room.picker = r.U8();
    room.locked = r.U8();
    room.racers = r.U8();
    const uint8_t cpusOut = r.U8();
    const auto trackOrNone = [](uint8_t t) { return t < kStockTracks || t == kTrackNone; };
    if (!r.Done() || phase >= kPhaseCount || room.phaseLeftMs > room.phaseTotalMs ||
        room.racerCap < kDefaultRacers || room.racerCap > kMaxRacers || !PoolOk(room.pool) ||
        !trackOrNone(room.track) || !trackOrNone(room.lastTrack) ||
        (room.picker >= kMaxPlayers && room.picker != kSystemId) || room.racers > kMaxRacers ||
        room.locked > room.racers || cpusOut > 1) {
        return false;
    }
    room.phase = static_cast<Phase>(phase);
    room.cpusOut = cpusOut == 1;
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
    lottery.track = r.U8();
    if (!r.Done() || lottery.track >= kStockTracks) {
        return false;
    }
    // The winner is one of the ballots, and a ballot that named a track won with that track.
    if (lottery.ballots.empty()) {
        if (lottery.winner != kNoWinner) {
            return false;
        }
    } else {
        if (lottery.winner >= lottery.ballots.size()) {
            return false;
        }
        const uint8_t voted = lottery.ballots[lottery.winner].track;
        if (voted != kTrackRandom && voted != lottery.track) {
            return false;
        }
    }
    out = lottery;
    return true;
}

bool DecodePick(const uint8_t* data, size_t len, uint8_t& out) {
    return DecodeVote(data, len, out);
}

bool DecodePicked(const uint8_t* data, size_t len, Picked& out) {
    Reader r(data, len);
    Picked picked;
    picked.track = r.U8();
    const uint8_t drawn = r.U8();
    picked.by = r.U8();
    if (!r.Done() || picked.track >= kStockTracks || drawn > 1 ||
        (picked.by >= kMaxPlayers && picked.by != kSystemId)) {
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
    if (!r.Done() || lock.machine >= kRosterMachines || lock.skin >= kMachineSkins || lock.engine > kEngineMax) {
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
    grid.rules.laps = r.U8();
    const uint8_t flags = r.U8();
    grid.rules.magnetRange = BitsFloat(r.U32());
    grid.rules.magnetStrength = BitsFloat(r.U32());
    grid.cpus = r.U8();
    const size_t count = r.U8();
    if (!r.Ok() || count == 0 || count > kMaxRacers || count + grid.cpus > kGridSlots ||
        (flags & ~kRuleFlagsAll) != 0) {
        return false;
    }
    grid.rules.ignoreTrackSettings = (flags & kRuleIgnoreTrackSettings) != 0;
    grid.rules.lavaNoKill = (flags & kRuleLavaNoKill) != 0;
    if (!RulesOk(grid.rules)) {
        return false;
    }
    uint32_t seen = 0;
    for (size_t i = 0; i < count; i++) {
        GridEntry e;
        e.id = r.U8();
        e.machine = r.U8();
        e.skin = r.U8();
        if (!r.Ok() || e.id >= kMaxPlayers || ((seen >> e.id) & 1u) != 0 || e.machine >= kRosterMachines ||
            e.skin >= kMachineSkins) {
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
