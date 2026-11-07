// Copyright (c) Zorkats

#include "gdx_companion_music.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include "decoders.h"
#include "gdx_companion_jobs.h"
#include "gdx_companion_json.h"
#include "gdx_companion_loudness.h"
#include "gdx_companion_paths.h"
#include "gdx_music_files.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

constexpr char kPreviousExt[] = ".previous";
constexpr char kOldBackupExt[] = ".gdx-old";
constexpr char kSwapExt[] = ".gdx-swap";
constexpr size_t kMaxCourseStem = 64;
constexpr uint64_t kReadChunkFrames = 4096;
constexpr uintmax_t kMaxRewriteBytes = 1024ull * 1024ull * 1024ull;  // 1 GiB

std::string Fold(const std::string& text) { return gdx::music::Fold(text); }

std::string Utf8Stem(const fs::path& path) { return PathToUtf8(path.stem()); }

int ExtensionRankUtf8(const fs::path& path) {
    const std::string extension = Fold(PathToUtf8(path.extension()));
    for (int i = 0; i < 3; ++i) {
        if (extension == gdx::music::kExtensions[i]) return i;
    }
    return -1;
}

// Regular files in dir with a playable extension and the stem folded equal to stem.
std::vector<fs::path> SameStem(const fs::path& dir, const std::string& stem) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return out;
    const std::string folded = Fold(stem);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& candidate = it->path();
        std::error_code type_ec;
        if (ExtensionRankUtf8(candidate) >= 0 && Fold(Utf8Stem(candidate)) == folded &&
            it->is_regular_file(type_ec)) {
            out.push_back(candidate);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path WithSuffix(const fs::path& path, const char* suffix) {
    fs::path out = path;
    out += suffix;
    return out;
}

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out, std::string& error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "cannot read " + PathToUtf8(path) + ": " + ec.message();
        return false;
    }
    if (size > kMaxRewriteBytes) {
        error = "the recording is larger than 1 GiB";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    out.resize(static_cast<size_t>(size));
    if (size > 0) file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!file) {
        error = "cannot read " + PathToUtf8(path);
        return false;
    }
    return true;
}

bool WriteAll(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        error = "cannot write " + PathToUtf8(path);
        return false;
    }
    return true;
}

uint32_t GetU32Le(const std::vector<uint8_t>& in, size_t at) {
    return static_cast<uint32_t>(in[at]) | (static_cast<uint32_t>(in[at + 1]) << 8) |
           (static_cast<uint32_t>(in[at + 2]) << 16) | (static_cast<uint32_t>(in[at + 3]) << 24);
}

void PutU32Le(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutU16Le(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void PutTag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

void SetU32Le(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
}

std::string CommentKey(const std::string& comment) {
    const size_t equals = comment.find('=');
    if (equals == std::string::npos) return std::string();
    return Fold(comment.substr(0, equals));
}

bool IsLoopComment(const std::string& comment) {
    const std::string key = CommentKey(comment);
    return key == "loopstart" || key == "looplength" || key == "loopend";
}

bool IsLevelComment(const std::string& comment) {
    const std::string key = CommentKey(comment);
    return key == "replaygain_track_gain" || key == "replaygain_track_peak";
}

// Whether a rewrite under choice and level drops comment from the file.
bool DropsComment(const std::string& comment, const LoopChoice& choice, const MusicLevel& level) {
    return (choice.policy != LoopPolicy::Keep && IsLoopComment(comment)) || (level.set && IsLevelComment(comment));
}

// The comments a rewrite adds: the Custom loop, then the level.
bool AppendComments(std::vector<std::string>& comments, const LoopChoice& choice, const MusicLevel& level,
                    std::string& error) {
    if (choice.policy == LoopPolicy::Custom) {
        if (choice.start >= choice.end) {
            error = "the loop must start before it ends";
            return false;
        }
        comments.push_back("LOOPSTART=" + std::to_string(choice.start));
        comments.push_back("LOOPLENGTH=" + std::to_string(choice.end - choice.start));
    }
    if (level.set) {
        comments.push_back(ReplayGainComment(level.gain_db));
        if (level.peak > 0.0f) comments.push_back(ReplayPeakComment(level.peak));
    }
    return true;
}

// EBU Tech 3285 bext: the fixed part is 602 bytes; version 2 adds loudness fields, each a signed 16-bit
// value times 100, 0x7FFF when not measured.
constexpr size_t kBextBytes = 602;
constexpr size_t kBextVersion = 346;
constexpr size_t kBextLoudness = 412;
constexpr size_t kBextRange = 414;
constexpr size_t kBextTruePeak = 416;
constexpr size_t kBextMomentary = 418;
constexpr size_t kBextShortTerm = 420;
constexpr uint16_t kBextUnset = 0x7FFF;

void SetU16Le(std::vector<uint8_t>& out, size_t at, uint16_t value) {
    out[at] = static_cast<uint8_t>(value);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
}

uint16_t BextHundredths(double value) {
    const long scaled = std::lround(value * 100.0);
    const long clamped = std::min<long>(32766, std::max<long>(-32768, scaled));
    return static_cast<uint16_t>(static_cast<int16_t>(clamped));
}

// Writes level into a bext body of at least kBextBytes, raising it to version 2. A body that was already
// version 2 keeps its range, momentary and short-term values, which the level does not change.
void WriteBextLevel(std::vector<uint8_t>& body, const MusicLevel& level) {
    const uint16_t version = static_cast<uint16_t>(body[kBextVersion] | (body[kBextVersion + 1] << 8));
    if (version < 2) {
        SetU16Le(body, kBextVersion, 2);
        SetU16Le(body, kBextRange, kBextUnset);
        SetU16Le(body, kBextMomentary, kBextUnset);
        SetU16Le(body, kBextShortTerm, kBextUnset);
    }
    SetU16Le(body, kBextLoudness,
             BextHundredths(static_cast<double>(GDX_MUSIC_REPLAYGAIN_REFERENCE_LUFS) - level.gain_db));
    SetU16Le(body, kBextTruePeak,
             level.peak > 0.0f ? BextHundredths(20.0 * std::log10(static_cast<double>(level.peak))) : kBextUnset);
}

std::vector<uint8_t> NewBext(const MusicLevel& level) {
    std::vector<uint8_t> body(kBextBytes, 0);
    static const char kOriginator[] = "G-Diffuser Companion";
    std::memcpy(body.data() + 256, kOriginator, sizeof(kOriginator) - 1);
    WriteBextLevel(body, level);
    return body;
}

// The rename steps of an install or restore, undone in reverse on failure.
class Moves {
public:
    bool Move(const fs::path& from, const fs::path& to, std::string& error) {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (ec) {
            error = "cannot move " + PathToUtf8(from) + " to " + PathToUtf8(to) + ": " + ec.message() +
                    ". If the game is running, close it and try again.";
            return false;
        }
        done_.push_back({from, to});
        return true;
    }

    void Undo() {
        for (auto it = done_.rbegin(); it != done_.rend(); ++it) {
            std::error_code ec;
            fs::rename(it->second, it->first, ec);
        }
        done_.clear();
    }

private:
    std::vector<std::pair<fs::path, fs::path>> done_;
};

// The Ogg page checksum: CRC-32 with polynomial 0x04C11DB7, not reflected, starting at 0, over the page
// with its checksum field read as zero.
uint32_t OggCrc(const uint8_t* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i << 24;
            for (int bit = 0; bit < 8; ++bit) value = (value & 0x80000000u) ? (value << 1) ^ 0x04C11DB7u : value << 1;
            out[i] = value;
        }
        return out;
    }();
    uint32_t crc = 0;
    for (size_t i = 0; i < size; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFFu];
    return crc;
}

constexpr uint8_t kOggContinued = 0x01;
constexpr uint8_t kOggFirst = 0x02;
constexpr size_t kOggHeaderBytes = 27;

struct OggPage {
    size_t offset = 0;  // of "OggS"
    size_t size = 0;    // header, lacing and body
    size_t body = 0;    // offset of the body
    uint8_t flags = 0;
    uint32_t serial = 0;
    std::vector<uint8_t> lacing;
};

void FinishOggPage(std::vector<uint8_t>& page) {
    SetU32Le(page, 22, 0);
    SetU32Le(page, 22, OggCrc(page.data(), page.size()));
}

bool ReadOggPages(const std::vector<uint8_t>& in, std::vector<OggPage>& pages, std::string& error) {
    size_t pos = 0;
    while (pos < in.size()) {
        if (in.size() - pos < kOggHeaderBytes || std::memcmp(in.data() + pos, "OggS", 4) != 0 || in[pos + 4] != 0) {
            error = "the Ogg file is damaged or has data between its pages";
            return false;
        }
        OggPage page;
        page.offset = pos;
        page.flags = in[pos + 5];
        page.serial = GetU32Le(in, pos + 14);
        const size_t segments = in[pos + 26];
        if (in.size() - pos - kOggHeaderBytes < segments) {
            error = "an Ogg page runs past the end of the file";
            return false;
        }
        page.lacing.assign(in.begin() + static_cast<std::ptrdiff_t>(pos + kOggHeaderBytes),
                           in.begin() + static_cast<std::ptrdiff_t>(pos + kOggHeaderBytes + segments));
        size_t body = 0;
        for (const uint8_t value : page.lacing) body += value;
        page.body = pos + kOggHeaderBytes + segments;
        if (in.size() - page.body < body) {
            error = "an Ogg page runs past the end of the file";
            return false;
        }
        page.size = kOggHeaderBytes + segments + body;
        std::vector<uint8_t> copy(in.begin() + static_cast<std::ptrdiff_t>(pos),
                                  in.begin() + static_cast<std::ptrdiff_t>(pos + page.size));
        const uint32_t stored = GetU32Le(copy, 22);
        SetU32Le(copy, 22, 0);
        if (OggCrc(copy.data(), copy.size()) != stored) {
            error = "Ogg page " + std::to_string(pages.size()) + " fails its checksum";
            return false;
        }
        pages.push_back(std::move(page));
        pos += pages.back().size;
    }
    if (pages.empty()) {
        error = "the Ogg file has no pages";
        return false;
    }
    return true;
}

bool DecodeToEnd(const fs::path& path, uint64_t expected, uint32_t channels, const CancelFlag& cancel,
                 ProgressValue& progress, float from, float to, std::string& error) {
    std::string narrow;
    if (!NarrowPathForC(path, narrow)) {
        error = "the decoder cannot open this path";
        return false;
    }
    GdxMusicInfo info{};
    GdxMusicDecoder* decoder = gdx_music_decoder_open(narrow.c_str(), &info);
    if (decoder == nullptr) {
        error = "the staged copy does not open";
        return false;
    }
    std::vector<float> buffer(static_cast<size_t>(kReadChunkFrames * channels));
    uint64_t total = 0;
    for (;;) {
        if (cancel.load()) {
            gdx_music_decoder_close(decoder);
            error = "cancelled";
            return false;
        }
        const uint64_t got = gdx_music_decoder_read(decoder, buffer.data(), kReadChunkFrames);
        total += got;
        if (expected > 0) {
            progress.store(from + (to - from) * static_cast<float>(std::min<uint64_t>(total, expected)) /
                                      static_cast<float>(expected));
        }
        if (got < kReadChunkFrames) break;
    }
    gdx_music_decoder_close(decoder);
    if (total != expected) {
        error = "the staged copy decoded " + std::to_string(total) + " of " + std::to_string(expected) + " frames";
        return false;
    }
    return true;
}

}  // namespace

const std::vector<MusicSlot>& MusicSlots() {
    static const std::vector<MusicSlot> slots = {
        {0, "bgm_mute_city", "Mute City courses", true},
        {1, "bgm_silence", "Silence courses", true},
        {2, "bgm_sand_ocean", "Sand Ocean courses", true},
        {3, "bgm_port_town", "Port Town courses", true},
        {4, "bgm_big_blue", "Big Blue courses", true},
        {5, "bgm_devils_forest", "Devil's Forest courses", true},
        {6, "bgm_red_canyon", "Red Canyon courses", true},
        {7, "bgm_sector", "Sector courses", true},
        {8, "bgm_white_land", "White Land courses", true},
        {9, "bgm_rainbow_road", "Rainbow Road", true},
        {10, "bgm_new_03", "Course Edit song New 03", true},
        {11, "bgm_new_02", "Course Edit song New 02", true},
        {12, "bgm_new_01", "Course Edit song New 01", true},
        {13, "bgm_new_04", "Course Edit song New 04", true},
        {14, "bgm_title", "Title screen", true},
        {15, "bgm_select", "Mode, machine and course select", true},
        {16, "bgm_option", "Options and records", true},
        {17, "bgm_deathrace", "Death Race", true},
        {18, "bgm_course_editor", "Course Edit", true},
        {19, "bgm_machine_editor", "Create Machine", true},
        {20, "bgm_ead_demo", "EAD demo", true},
        {21, "bgm_21", "Finish jingle when you lose", false},
        {22, "bgm_22", "Finish and winner jingle", false},
        {23, "bgm_23", "Retire jingle", false},
        {24, "bgm_24", "Winner jingle after a crash", false},
        {25, "bgm_gameover", "Game Over", false},
        {26, "bgm_end_cs", "Grand Prix ending", false},
        {27, "bgm_credits", "Staff credits", false},
        {28, "bgm_demo", "Race intro fanfare", false},
        {29, "bgm_deathrace2", "Not used", false},
    };
    return slots;
}

AudioFormat FormatOf(const fs::path& path) {
    switch (ExtensionRankUtf8(path)) {
    case 0:
        return AudioFormat::Ogg;
    case 1:
        return AudioFormat::Flac;
    case 2:
        return AudioFormat::Wav;
    default:
        return AudioFormat::Unknown;
    }
}

const char* FormatExtension(AudioFormat format) {
    switch (format) {
    case AudioFormat::Ogg:
        return ".ogg";
    case AudioFormat::Flac:
        return ".flac";
    case AudioFormat::Wav:
        return ".wav";
    case AudioFormat::Unknown:
        break;
    }
    return "";
}

const char* FormatName(AudioFormat format) {
    switch (format) {
    case AudioFormat::Ogg:
        return "Ogg Vorbis";
    case AudioFormat::Flac:
        return "FLAC";
    case AudioFormat::Wav:
        return "WAV";
    case AudioFormat::Unknown:
        break;
    }
    return "unsupported";
}

bool InspectAudio(const fs::path& path, AudioInfo& info, std::string& error) {
    info = AudioInfo{};
    info.format = FormatOf(path);
    if (info.format == AudioFormat::Unknown) {
        error = "the game plays .ogg, .flac and .wav recordings only";
        return false;
    }
    std::string narrow;
    if (!NarrowPathForC(path, narrow)) {
        error = "the path has characters the decoder cannot open; move the file to a folder with a plain name";
        return false;
    }
    GdxMusicInfo raw{};
    GdxMusicDecoder* decoder = gdx_music_decoder_open(narrow.c_str(), &raw);
    if (decoder == nullptr) {
        error = "the game's decoder cannot open this file, or it has more than two channels";
        return false;
    }
    gdx_music_decoder_close(decoder);
    if (raw.sampleRate == 0 || raw.totalFrames == 0) {
        error = "the recording is empty";
        return false;
    }
    info.rate = raw.sampleRate;
    info.channels = raw.channels;
    info.frames = raw.totalFrames;
    info.has_loop = raw.hasLoopTags != 0;
    info.loop_start = raw.loopStart;
    info.loop_end = raw.loopEnd;
    info.has_gain = raw.hasGain != 0;
    info.gain_db = raw.gainDb;
    info.has_peak = raw.hasPeak != 0;
    info.peak = raw.peak;
    return true;
}

bool LoopPolicyOffered(AudioFormat format, LoopPolicy policy) {
    (void)policy;
    return format != AudioFormat::Unknown;
}

void EffectiveLoop(const AudioInfo& info, const LoopChoice& choice, uint64_t& start, uint64_t& end) {
    switch (choice.policy) {
    case LoopPolicy::Keep:
        start = info.has_loop ? info.loop_start : 0;
        end = info.has_loop ? info.loop_end : info.frames;
        return;
    case LoopPolicy::Whole:
        start = 0;
        end = info.frames;
        return;
    case LoopPolicy::Custom:
        start = choice.start;
        end = choice.end;
        return;
    }
}

std::string CheckLoop(const AudioInfo& info, const LoopChoice& choice) {
    if (!LoopPolicyOffered(info.format, choice.policy)) {
        return "Choose a recording the game can play.";
    }
    if (choice.policy != LoopPolicy::Custom) return std::string();
    if (choice.end > info.frames) return "The loop ends after the recording does.";
    if (choice.start >= choice.end) return "The loop must start before it ends.";
    if (info.format == AudioFormat::Wav && choice.end - 1 > 0xFFFFFFFFull) {
        return "A WAV loop must end within the first 4294967296 frames.";
    }
    return std::string();
}

bool RewriteWavLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, uint32_t rate,
                    std::vector<uint8_t>& out, std::string& error, const MusicLevel& level) {
    if (in.size() < 12 || std::memcmp(in.data(), "RIFF", 4) != 0 || std::memcmp(in.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF WAVE file";
        return false;
    }
    if (choice.policy == LoopPolicy::Custom && (choice.start >= choice.end || choice.end - 1 > 0xFFFFFFFFull)) {
        error = "the loop does not fit a WAV smpl chunk";
        return false;
    }
    out.clear();
    out.reserve(in.size() + 72 + 8 + kBextBytes);
    out.insert(out.end(), in.begin(), in.begin() + 12);
    bool have_fmt = false;
    bool have_data = false;
    bool level_written = !level.set;
    size_t pos = 12;
    while (pos + 8 <= in.size()) {
        const uint32_t length = GetU32Le(in, pos + 4);
        const size_t body = pos + 8;
        if (length > in.size() - body) {
            error = "a WAV chunk runs past the end of the file";
            return false;
        }
        const bool is_smpl = std::memcmp(in.data() + pos, "smpl", 4) == 0;
        const bool is_data = std::memcmp(in.data() + pos, "data", 4) == 0;
        const bool is_bext = std::memcmp(in.data() + pos, "bext", 4) == 0;
        if (std::memcmp(in.data() + pos, "fmt ", 4) == 0) have_fmt = true;
        if (is_bext && level.set) {
            // The first full-size bext takes the level in place; a short one or a second one is dropped.
            if (!level_written && length >= kBextBytes) {
                std::vector<uint8_t> fields(in.begin() + static_cast<std::ptrdiff_t>(body),
                                            in.begin() + static_cast<std::ptrdiff_t>(body + length));
                WriteBextLevel(fields, level);
                PutTag(out, "bext");
                PutU32Le(out, length);
                out.insert(out.end(), fields.begin(), fields.end());
                if (length & 1u) out.push_back(0);
                level_written = true;
            }
            const size_t padded = static_cast<size_t>(length) + (length & 1u);
            if (padded > in.size() - body) break;
            pos = body + padded;
            continue;
        }
        if (is_data && !have_data) {
            have_data = true;
            if (!level_written) {
                const std::vector<uint8_t> bext = NewBext(level);
                PutTag(out, "bext");
                PutU32Le(out, static_cast<uint32_t>(bext.size()));
                out.insert(out.end(), bext.begin(), bext.end());
                level_written = true;
            }
            if (choice.policy == LoopPolicy::Custom) {
                // One forward loop. The end field is the last frame played, inclusive.
                PutTag(out, "smpl");
                PutU32Le(out, 36 + 24);
                PutU32Le(out, 0);  // manufacturer
                PutU32Le(out, 0);  // product
                PutU32Le(out, rate > 0 ? static_cast<uint32_t>(1000000000ull / rate) : 0);  // sample period, ns
                PutU32Le(out, 60);  // MIDI unity note
                PutU32Le(out, 0);   // pitch fraction
                PutU32Le(out, 0);   // SMPTE format
                PutU32Le(out, 0);   // SMPTE offset
                PutU32Le(out, 1);   // loop count
                PutU32Le(out, 0);   // sampler data
                PutU32Le(out, 0);   // cue point id
                PutU32Le(out, 0);   // forward
                PutU32Le(out, static_cast<uint32_t>(choice.start));
                PutU32Le(out, static_cast<uint32_t>(choice.end - 1));
                PutU32Le(out, 0);  // fraction
                PutU32Le(out, 0);  // play forever
            }
        }
        const size_t padded = static_cast<size_t>(length) + (length & 1u);
        if (!is_smpl || choice.policy == LoopPolicy::Keep) {
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(pos),
                       in.begin() + static_cast<std::ptrdiff_t>(body + length));
            if (length & 1u) out.push_back(0);
        }
        if (padded > in.size() - body) break;  // a missing pad byte at the end of the file
        pos = body + padded;
    }
    if (!have_fmt || !have_data) {
        error = "the WAV has no fmt or data chunk";
        return false;
    }
    const uint64_t riff = out.size() - 8;
    if (riff > 0xFFFFFFFFull) {
        error = "the WAV is too large";
        return false;
    }
    for (int i = 0; i < 4; ++i) out[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(riff >> (8 * i));
    return true;
}

bool RewriteFlacLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, std::vector<uint8_t>& out,
                     std::string& error, const MusicLevel& level) {
    if (in.size() < 8 || std::memcmp(in.data(), "fLaC", 4) != 0) {
        error = "not a FLAC file, or it starts with an ID3 tag the Companion does not rewrite";
        return false;
    }
    struct Block {
        uint8_t type;
        size_t offset;
        size_t length;
    };
    std::vector<Block> blocks;
    size_t pos = 4;
    bool last = false;
    while (!last) {
        if (pos + 4 > in.size()) {
            error = "the FLAC metadata runs past the end of the file";
            return false;
        }
        last = (in[pos] & 0x80u) != 0;
        const uint8_t type = static_cast<uint8_t>(in[pos] & 0x7Fu);
        const size_t length = (static_cast<size_t>(in[pos + 1]) << 16) | (static_cast<size_t>(in[pos + 2]) << 8) |
                              static_cast<size_t>(in[pos + 3]);
        if (length > in.size() - pos - 4) {
            error = "a FLAC metadata block runs past the end of the file";
            return false;
        }
        blocks.push_back({type, pos + 4, length});
        pos += 4 + length;
    }
    if (blocks.empty() || blocks[0].type != 0) {
        error = "the FLAC does not start with its STREAMINFO block";
        return false;
    }
    const size_t audio = pos;

    std::string vendor = "G-Diffuser Companion";
    std::vector<std::string> comments;
    bool had_comments = false;
    for (const Block& block : blocks) {
        if (block.type != 4) continue;
        if (had_comments) {
            error = "the FLAC has more than one comment block";
            return false;
        }
        had_comments = true;
        size_t at = block.offset;
        const size_t end = block.offset + block.length;
        if (end - at < 4) {
            error = "the FLAC comment block is damaged";
            return false;
        }
        const uint32_t vendor_length = GetU32Le(in, at);
        at += 4;
        if (vendor_length > end - at || end - at - vendor_length < 4) {
            error = "the FLAC comment block is damaged";
            return false;
        }
        vendor.assign(reinterpret_cast<const char*>(in.data() + at), vendor_length);
        at += vendor_length;
        const uint32_t count = GetU32Le(in, at);
        at += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (end - at < 4) {
                error = "the FLAC comment block is damaged";
                return false;
            }
            const uint32_t length = GetU32Le(in, at);
            at += 4;
            if (length > end - at) {
                error = "the FLAC comment block is damaged";
                return false;
            }
            std::string comment(reinterpret_cast<const char*>(in.data() + at), length);
            at += length;
            if (!DropsComment(comment, choice, level)) comments.push_back(std::move(comment));
        }
    }
    if (!AppendComments(comments, choice, level, error)) return false;
    std::vector<uint8_t> comment_body;
    PutU32Le(comment_body, static_cast<uint32_t>(vendor.size()));
    comment_body.insert(comment_body.end(), vendor.begin(), vendor.end());
    PutU32Le(comment_body, static_cast<uint32_t>(comments.size()));
    for (const std::string& comment : comments) {
        PutU32Le(comment_body, static_cast<uint32_t>(comment.size()));
        comment_body.insert(comment_body.end(), comment.begin(), comment.end());
    }
    if (comment_body.size() > 0xFFFFFFu) {
        error = "the FLAC comments are too large";
        return false;
    }

    // Kept blocks in order, with the comment block where it was, or after STREAMINFO when there was none.
    struct Piece {
        uint8_t type;
        const uint8_t* data;
        size_t length;
    };
    std::vector<Piece> pieces;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& block = blocks[i];
        if (block.type == 1) continue;  // padding
        if (block.type == 4) {
            pieces.push_back({4, comment_body.data(), comment_body.size()});
        } else {
            pieces.push_back({block.type, in.data() + block.offset, block.length});
        }
        if (i == 0 && !had_comments) pieces.push_back({4, comment_body.data(), comment_body.size()});
    }
    out.clear();
    out.reserve(in.size() + comment_body.size());
    out.insert(out.end(), in.begin(), in.begin() + 4);
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& piece = pieces[i];
        const uint8_t flag = i + 1 == pieces.size() ? 0x80u : 0x00u;
        out.push_back(static_cast<uint8_t>(flag | piece.type));
        out.push_back(static_cast<uint8_t>(piece.length >> 16));
        out.push_back(static_cast<uint8_t>(piece.length >> 8));
        out.push_back(static_cast<uint8_t>(piece.length));
        out.insert(out.end(), piece.data, piece.data + piece.length);
    }
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(audio), in.end());
    return true;
}

bool RewriteOggLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, std::vector<uint8_t>& out,
                    std::string& error, const MusicLevel& level) {
    std::vector<OggPage> pages;
    if (!ReadOggPages(in, pages, error)) return false;
    if ((pages[0].flags & kOggFirst) == 0) {
        error = "the Ogg file does not start with the first page of its stream";
        return false;
    }
    for (const OggPage& page : pages) {
        if (page.serial != pages[0].serial) {
            error = "the Ogg file holds more than one stream; save it as a single Vorbis stream";
            return false;
        }
    }

    // The three Vorbis header packets: identification alone on page 0, then comment and setup, the setup
    // packet ending its page so the audio starts on a fresh one.
    std::vector<std::vector<uint8_t>> packets(1);
    size_t header_pages = 0;
    for (size_t p = 0; p < pages.size() && header_pages == 0; ++p) {
        const OggPage& page = pages[p];
        size_t at = page.body;
        for (size_t i = 0; i < page.lacing.size(); ++i) {
            const uint8_t value = page.lacing[i];
            packets.back().insert(packets.back().end(), in.begin() + static_cast<std::ptrdiff_t>(at),
                                  in.begin() + static_cast<std::ptrdiff_t>(at + value));
            at += value;
            if (value == 255) continue;
            if (packets.size() == 1 && (p != 0 || i + 1 != page.lacing.size())) {
                error = "the Ogg identification header does not sit alone on the first page";
                return false;
            }
            if (packets.size() == 3) {
                if (i + 1 != page.lacing.size()) {
                    error = "the Ogg setup header does not end its page";
                    return false;
                }
                header_pages = p + 1;
                break;
            }
            packets.emplace_back();
        }
    }
    if (header_pages == 0) {
        error = "the Ogg file ends inside its Vorbis headers";
        return false;
    }
    const std::vector<uint8_t>& comment = packets[1];
    if (packets[0].size() < 7 || packets[0][0] != 0x01 || std::memcmp(packets[0].data() + 1, "vorbis", 6) != 0 ||
        comment.size() < 7 || comment[0] != 0x03 || std::memcmp(comment.data() + 1, "vorbis", 6) != 0) {
        error = "the Ogg file is not Ogg Vorbis";
        return false;
    }

    size_t at = 7;
    const size_t end = comment.size();
    if (end - at < 4) {
        error = "the Vorbis comment header is damaged";
        return false;
    }
    const uint32_t vendor_length = GetU32Le(comment, at);
    at += 4;
    if (vendor_length > end - at || end - at - vendor_length < 4) {
        error = "the Vorbis comment header is damaged";
        return false;
    }
    const std::string vendor(reinterpret_cast<const char*>(comment.data() + at), vendor_length);
    at += vendor_length;
    const uint32_t count = GetU32Le(comment, at);
    at += 4;
    std::vector<std::string> comments;
    for (uint32_t i = 0; i < count; ++i) {
        if (end - at < 4) {
            error = "the Vorbis comment header is damaged";
            return false;
        }
        const uint32_t length = GetU32Le(comment, at);
        at += 4;
        if (length > end - at) {
            error = "the Vorbis comment header is damaged";
            return false;
        }
        std::string text(reinterpret_cast<const char*>(comment.data() + at), length);
        at += length;
        if (!DropsComment(text, choice, level)) comments.push_back(std::move(text));
    }
    if (!AppendComments(comments, choice, level, error)) return false;
    std::vector<uint8_t> rebuilt(comment.begin(), comment.begin() + 7);
    PutU32Le(rebuilt, static_cast<uint32_t>(vendor.size()));
    rebuilt.insert(rebuilt.end(), vendor.begin(), vendor.end());
    PutU32Le(rebuilt, static_cast<uint32_t>(comments.size()));
    for (const std::string& text : comments) {
        PutU32Le(rebuilt, static_cast<uint32_t>(text.size()));
        rebuilt.insert(rebuilt.end(), text.begin(), text.end());
    }
    rebuilt.push_back(0x01);  // framing bit

    // Lace the comment and setup packets into new pages of up to 255 segments each.
    struct Segment {
        const uint8_t* data;
        uint8_t length;
        bool ends_packet;
    };
    std::vector<Segment> segments;
    for (const std::vector<uint8_t>* packet : {&rebuilt, &packets[2]}) {
        size_t left = packet->size();
        const uint8_t* data = packet->data();
        for (;;) {
            const uint8_t length = static_cast<uint8_t>(std::min<size_t>(left, 255));
            segments.push_back({data, length, length < 255});
            data += length;
            left -= length;
            if (length < 255) break;
        }
    }

    out.clear();
    out.reserve(in.size() + rebuilt.size());
    out.insert(out.end(), in.begin(), in.begin() + static_cast<std::ptrdiff_t>(pages[0].size));
    const uint32_t serial = pages[0].serial;
    uint32_t sequence = 1;
    bool continued = false;
    for (size_t first = 0; first < segments.size(); first += 255) {
        const size_t last = std::min(segments.size(), first + 255);
        bool any_end = false;
        for (size_t i = first; i < last; ++i) any_end = any_end || segments[i].ends_packet;
        std::vector<uint8_t> page = {'O', 'g', 'g', 'S', 0, static_cast<uint8_t>(continued ? kOggContinued : 0)};
        // Header packets have granule 0; a page on which no packet ends has none (all ones).
        const uint64_t granule = any_end ? 0 : ~0ull;
        for (int i = 0; i < 8; ++i) page.push_back(static_cast<uint8_t>(granule >> (8 * i)));
        PutU32Le(page, serial);
        PutU32Le(page, sequence++);
        PutU32Le(page, 0);
        page.push_back(static_cast<uint8_t>(last - first));
        for (size_t i = first; i < last; ++i) page.push_back(segments[i].length);
        for (size_t i = first; i < last; ++i) {
            page.insert(page.end(), segments[i].data, segments[i].data + segments[i].length);
        }
        FinishOggPage(page);
        out.insert(out.end(), page.begin(), page.end());
        continued = !segments[last - 1].ends_packet;
    }

    // The audio pages, unchanged but for their sequence numbers and checksums.
    for (size_t p = header_pages; p < pages.size(); ++p) {
        std::vector<uint8_t> page(in.begin() + static_cast<std::ptrdiff_t>(pages[p].offset),
                                  in.begin() + static_cast<std::ptrdiff_t>(pages[p].offset + pages[p].size));
        SetU32Le(page, 18, sequence++);
        FinishOggPage(page);
        out.insert(out.end(), page.begin(), page.end());
    }
    return true;
}

std::string CheckCourseSongName(const std::string& stem) {
    if (stem.empty()) return "Name the song.";
    if (stem.size() > kMaxCourseStem) return "Use at most 64 characters.";
    if (!IsValidUtf8(stem)) return "The name is not valid text.";
    for (const unsigned char c : stem) {
        if (c < 32 || c == 127) return "The name has a control character.";
        if (std::strchr("<>:\"/\\|?*", static_cast<char>(c)) != nullptr) {
            return "A file name cannot hold < > : \" / \\ | ? or *.";
        }
    }
    if (stem.front() == '.' || stem.back() == '.' || stem.front() == ' ' || stem.back() == ' ') {
        return "The name cannot start or end with a dot or a space.";
    }
    const std::string folded = Fold(stem);
    static const char* const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                            "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
                                            "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* reserved : kReserved) {
        if (folded == reserved) return "Windows reserves this name.";
    }
    if (gdx::music::IsReplacementStem(stem)) {
        return "This is a game music slot name, which replaces game music. Pick another name, or choose the slot "
               "as the target.";
    }
    return std::string();
}

InstallPlan PlanMusicInstall(const fs::path& game_dir, const std::string& stem, AudioFormat format) {
    InstallPlan plan;
    plan.music_dir = game_dir / "music";
    plan.destination = plan.music_dir / PathFromUtf8(stem + FormatExtension(format));
    plan.displaced = SameStem(plan.music_dir, stem);
    for (const fs::path& path : plan.displaced) {
        std::error_code ec;
        const fs::path backup = WithSuffix(path, kPreviousExt);
        if (fs::exists(backup, ec)) plan.replaced_backups.push_back(backup);
    }
    if (gdx::music::IsReplacementStem(stem)) {
        for (const fs::path& path : SameStem(game_dir / "mods" / "music", stem)) plan.legacy.push_back(path);
    } else {
        for (const char* folder : {"mods/music/custom", "music/custom"}) {
            for (const fs::path& path : SameStem(game_dir / PathFromUtf8(folder), stem)) plan.legacy.push_back(path);
        }
    }
    return plan;
}

fs::path MakeMusicStaging(const fs::path& music_dir, std::string& error) {
    std::error_code ec;
    fs::create_directories(music_dir, ec);
    if (ec) {
        error = "cannot create " + PathToUtf8(music_dir) + ": " + ec.message();
        return fs::path();
    }
    static std::atomic<uint32_t> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 16; ++attempt) {
        char token[48];
        std::snprintf(token, sizeof(token), "music-%llx-%x", static_cast<unsigned long long>(ticks),
                      static_cast<unsigned>(counter.fetch_add(1)));
        const fs::path staging = music_dir / (std::string(kStagingPrefix) + token);
        if (fs::create_directory(staging, ec) && !ec) return staging;
    }
    error = "cannot create a staging folder in " + PathToUtf8(music_dir);
    return fs::path();
}

bool RemoveMusicStaging(const fs::path& staging, const fs::path& music_dir) {
    if (staging.empty() || staging.parent_path() != music_dir) return false;
    const std::string name = PathToUtf8(staging.filename());
    if (name.rfind(kStagingPrefix, 0) != 0 || name.size() == sizeof(kStagingPrefix) - 1) return false;
    std::error_code ec;
    fs::remove_all(staging, ec);
    return !fs::exists(staging, ec);
}

bool StageMusic(const fs::path& source, const AudioInfo& info, const LoopChoice& choice, const fs::path& staged,
                const CancelFlag& cancel, ProgressValue& progress, std::string& error, const LevelRequest& request,
                MusicLevel* written) {
    progress.store(0.0f);
    const std::string loop_problem = CheckLoop(info, choice);
    if (!loop_problem.empty()) {
        error = loop_problem;
        return false;
    }
    MusicLevel level;
    float measured = 0.0f;
    if (request.match) {
        LoudnessResult loudness;
        if (!MeasureLoudness(source, cancel, progress, 0.0f, 0.4f, loudness, error)) return false;
        level.set = true;
        level.gain_db = std::min(60.0, std::max(-60.0, ReplayGainFor(loudness.lufs) + request.trim_db));
        level.peak = loudness.peak;
        measured = 0.4f;
    }
    if (written != nullptr) *written = level;
    std::error_code ec;
    if (choice.policy == LoopPolicy::Keep && !level.set) {
        fs::copy_file(source, staged, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy the recording: " + ec.message();
            return false;
        }
    } else {
        std::vector<uint8_t> in;
        std::vector<uint8_t> out;
        if (!ReadAll(source, in, error)) return false;
        bool ok = false;
        if (info.format == AudioFormat::Wav) {
            ok = RewriteWavLoop(in, choice, info.rate, out, error, level);
        } else if (info.format == AudioFormat::Flac) {
            ok = RewriteFlacLoop(in, choice, out, error, level);
        } else {
            ok = RewriteOggLoop(in, choice, out, error, level);
        }
        if (!ok || !WriteAll(staged, out, error)) return false;
    }
    const float checked = measured + 0.1f * (1.0f - measured);
    progress.store(checked);
    if (cancel.load()) {
        error = "cancelled";
        return false;
    }

    AudioInfo check;
    if (!InspectAudio(staged, check, error)) {
        error = "the staged copy does not open: " + error;
        return false;
    }
    if (check.rate != info.rate || check.channels != info.channels || check.frames != info.frames) {
        error = "the staged copy does not match the recording's rate, channels or length";
        return false;
    }
    uint64_t want_start = 0;
    uint64_t want_end = 0;
    EffectiveLoop(info, choice, want_start, want_end);
    const bool want_tags = choice.policy == LoopPolicy::Custom || (choice.policy == LoopPolicy::Keep && info.has_loop);
    if (check.has_loop != want_tags || check.loop_start != want_start || check.loop_end != want_end) {
        error = "the staged copy does not carry the chosen loop";
        return false;
    }
    if (level.set && (!check.has_gain || std::fabs(check.gain_db - level.gain_db) > 0.02 ||
                      check.has_peak != (level.peak > 0.0f))) {
        error = "the staged copy does not carry the measured level";
        return false;
    }
    return DecodeToEnd(staged, info.frames, info.channels, cancel, progress, checked, 1.0f, error);
}

bool CommitMusicInstall(const InstallPlan& plan, const fs::path& staged, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(staged, ec)) {
        error = "the staged copy is missing";
        return false;
    }
    Moves moves;
    std::vector<fs::path> old_backups;
    for (const fs::path& path : plan.displaced) {
        const fs::path backup = WithSuffix(path, kPreviousExt);
        if (fs::exists(backup, ec)) {
            const fs::path old = WithSuffix(backup, kOldBackupExt);
            fs::remove(old, ec);
            if (!moves.Move(backup, old, error)) {
                moves.Undo();
                return false;
            }
            old_backups.push_back(old);
        }
        if (!moves.Move(path, backup, error)) {
            moves.Undo();
            return false;
        }
    }
    if (fs::exists(plan.destination, ec)) {
        error = "another file appeared at " + PathToUtf8(plan.destination) + "; nothing was changed";
        moves.Undo();
        return false;
    }
    if (!moves.Move(staged, plan.destination, error)) {
        moves.Undo();
        return false;
    }
    for (const fs::path& old : old_backups) fs::remove(old, ec);
    return true;
}

RestorePlan PlanMusicRestore(const fs::path& game_dir, const std::string& stem) {
    RestorePlan plan;
    const fs::path music = game_dir / "music";
    plan.current = SameStem(music, stem);
    std::error_code ec;
    fs::directory_iterator it(music, ec);
    if (ec) return plan;
    const std::string folded = Fold(stem);
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& candidate = it->path();
        if (Fold(PathToUtf8(candidate.extension())) != kPreviousExt) continue;
        const fs::path inner = candidate.stem();  // <stem>.<ext>
        std::error_code type_ec;
        if (ExtensionRankUtf8(inner) >= 0 && Fold(Utf8Stem(inner)) == folded && it->is_regular_file(type_ec)) {
            plan.previous.push_back(candidate);
        }
    }
    std::sort(plan.previous.begin(), plan.previous.end());
    return plan;
}

bool RestoreMusic(const RestorePlan& plan, std::string& error) {
    if (plan.previous.empty()) {
        error = "there is no earlier version to restore";
        return false;
    }
    Moves moves;
    std::vector<std::pair<fs::path, fs::path>> parked;
    for (const fs::path& path : plan.current) {
        const fs::path swap = WithSuffix(path, kSwapExt);
        if (!moves.Move(path, swap, error)) {
            moves.Undo();
            return false;
        }
        parked.push_back({swap, WithSuffix(path, kPreviousExt)});
    }
    for (const fs::path& backup : plan.previous) {
        fs::path target = backup;
        target.replace_extension();
        std::error_code ec;
        if (fs::exists(target, ec)) {
            error = "a file already exists at " + PathToUtf8(target);
            moves.Undo();
            return false;
        }
        if (!moves.Move(backup, target, error)) {
            moves.Undo();
            return false;
        }
    }
    for (const auto& [swap, previous] : parked) {
        std::error_code ec;
        if (fs::exists(previous, ec)) {
            error = "a file already exists at " + PathToUtf8(previous);
            moves.Undo();
            return false;
        }
        if (!moves.Move(swap, previous, error)) {
            moves.Undo();
            return false;
        }
    }
    return true;
}

bool LoadNativeTargets(const fs::path& json, std::vector<NativeTarget>& out, std::string& error) {
    out.clear();
    std::vector<uint8_t> bytes;
    std::error_code ec;
    if (fs::file_size(json, ec) > 64 * 1024 || ec) {
        error = ec ? "cannot read " + PathToUtf8(json) : "the targets file is larger than 64 KiB";
        return false;
    }
    if (!ReadAll(json, bytes, error)) return false;
    JsonValue root;
    if (!JsonValue::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), root, error)) {
        error = "the targets file is not valid JSON: " + error;
        return false;
    }
    const JsonValue* schema = root.Find("schema_version");
    int64_t version = 0;
    if (schema == nullptr || !schema->AsInt64(version) || version != 1) {
        error = "the targets file is not schema 1";
        return false;
    }
    const JsonValue* targets = root.Find("targets");
    if (targets == nullptr || targets->kind() != JsonValue::Kind::Array) {
        error = "the targets file has no target list";
        return false;
    }
    auto text = [](const JsonValue& object, const char* key) -> std::string {
        const JsonValue* value = object.Find(key);
        return value != nullptr && value->kind() == JsonValue::Kind::String ? value->AsString() : std::string();
    };
    auto resources = [&](const JsonValue* list, std::vector<NativeResource>& into) -> bool {
        if (list == nullptr) return true;
        if (list->kind() != JsonValue::Kind::Array) return false;
        for (const JsonValue& item : list->AsArray()) {
            NativeResource resource;
            resource.key = text(item, "key");
            resource.channel = text(item, "channel");
            int64_t rate = 0;
            const JsonValue* rate_value = item.Find("rate_hz");
            if (resource.key.empty() || rate_value == nullptr || !rate_value->AsInt64(rate) || rate < 1000 ||
                rate > 192000) {
                return false;
            }
            resource.rate_hz = static_cast<uint32_t>(rate);
            into.push_back(resource);
        }
        return true;
    };
    for (const JsonValue& item : targets->AsArray()) {
        NativeTarget target;
        target.id = text(item, "id");
        target.label = text(item, "label");
        target.stream_key = text(item, "stream_key");
        int64_t bgm = 0;
        const JsonValue* bgm_value = item.Find("bgm_id");
        const JsonValue* native = item.Find("native");
        if (target.id.empty() || target.label.empty() || bgm_value == nullptr || !bgm_value->AsInt64(bgm) ||
            native == nullptr || native->kind() != JsonValue::Kind::Object ||
            !resources(native->Find("cartridge"), target.cartridge) ||
            !resources(native->Find("expansion_kit"), target.expansion_kit)) {
            error = "a target in the targets file is incomplete";
            out.clear();
            return false;
        }
        target.bgm_id = static_cast<int>(bgm);
        out.push_back(std::move(target));
    }
    return true;
}

uint64_t ResampledFrames(uint64_t frames, uint32_t from, uint32_t to) {
    if (from == 0) return 0;
    if (from == to) return frames;
    return static_cast<uint64_t>(std::llround(static_cast<double>(frames) * to / from));
}

uint64_t NativeBytes(uint64_t samples) { return (samples + 15) / 16 * 9; }

bool WriteNativeWav(const fs::path& source, const AudioInfo& info, uint32_t rate, const LoopChoice& choice,
                    const fs::path& wav, const fs::path& loop_json, const CancelFlag& cancel,
                    ProgressValue& progress, std::string& error) {
    progress.store(0.0f);
    if (rate == 0 || info.rate == 0 || info.frames == 0) {
        error = "the recording or the target has no rate";
        return false;
    }
    const std::string loop_problem = choice.policy == LoopPolicy::Custom ? CheckLoop(info, choice) : std::string();
    if (!loop_problem.empty()) {
        error = loop_problem;
        return false;
    }
    std::string narrow;
    if (!NarrowPathForC(source, narrow)) {
        error = "the decoder cannot open this path";
        return false;
    }
    GdxMusicInfo raw{};
    GdxMusicDecoder* decoder = gdx_music_decoder_open(narrow.c_str(), &raw);
    if (decoder == nullptr) {
        error = "the recording does not open";
        return false;
    }
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(info.frames));
    std::vector<float> buffer(static_cast<size_t>(kReadChunkFrames * raw.channels));
    for (;;) {
        if (cancel.load()) {
            gdx_music_decoder_close(decoder);
            error = "cancelled";
            return false;
        }
        const uint64_t got = gdx_music_decoder_read(decoder, buffer.data(), kReadChunkFrames);
        for (uint64_t i = 0; i < got; ++i) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < raw.channels; ++c) sum += buffer[static_cast<size_t>(i * raw.channels + c)];
            mono.push_back(sum / static_cast<float>(raw.channels));
        }
        progress.store(0.7f * static_cast<float>(std::min<uint64_t>(mono.size(), info.frames)) /
                       static_cast<float>(info.frames));
        if (got < kReadChunkFrames) break;
    }
    gdx_music_decoder_close(decoder);
    if (mono.empty()) {
        error = "the recording decoded to nothing";
        return false;
    }

    const uint64_t out_frames = ResampledFrames(mono.size(), info.rate, rate);
    if (out_frames == 0 || out_frames > 0x3FFFFFFFull) {
        error = "the converted recording has no usable length";
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(44 + out_frames * 2));
    PutTag(bytes, "RIFF");
    PutU32Le(bytes, static_cast<uint32_t>(36 + out_frames * 2));
    PutTag(bytes, "WAVE");
    PutTag(bytes, "fmt ");
    PutU32Le(bytes, 16);
    PutU16Le(bytes, 1);  // PCM
    PutU16Le(bytes, 1);  // mono
    PutU32Le(bytes, rate);
    PutU32Le(bytes, rate * 2);
    PutU16Le(bytes, 2);
    PutU16Le(bytes, 16);
    PutTag(bytes, "data");
    PutU32Le(bytes, static_cast<uint32_t>(out_frames * 2));
    const double step = static_cast<double>(info.rate) / static_cast<double>(rate);
    for (uint64_t i = 0; i < out_frames; ++i) {
        const double position = static_cast<double>(i) * step;
        const size_t left = std::min(static_cast<size_t>(position), mono.size() - 1);
        const size_t right = std::min(left + 1, mono.size() - 1);
        const float fraction = static_cast<float>(position - static_cast<double>(left));
        const float value = mono[left] + (mono[right] - mono[left]) * fraction;
        const long scaled = std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
        PutU16Le(bytes, static_cast<uint16_t>(static_cast<int16_t>(scaled)));
        if ((i & 0xFFFF) == 0) {
            if (cancel.load()) {
                error = "cancelled";
                return false;
            }
            progress.store(0.7f + 0.25f * static_cast<float>(i) / static_cast<float>(out_frames));
        }
    }
    if (!WriteAll(wav, bytes, error)) return false;

    AudioInfo decoded = info;
    decoded.frames = mono.size();
    uint64_t start = 0;
    uint64_t end = 0;
    EffectiveLoop(decoded, choice, start, end);
    uint64_t out_start = ResampledFrames(start, info.rate, rate) / 16 * 16;
    uint64_t out_end = std::min(ResampledFrames(end, info.rate, rate), out_frames);
    if (out_end <= out_start) {
        error = "the loop is too short to survive the conversion";
        return false;
    }
    const std::string loop = "{\"start\": " + std::to_string(out_start) + ", \"end\": " + std::to_string(out_end) + "}\n";
    if (!WriteAll(loop_json, std::vector<uint8_t>(loop.begin(), loop.end()), error)) return false;
    progress.store(1.0f);
    return true;
}

BackgroundTask::~BackgroundTask() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
}

bool BackgroundTask::Start(std::string label, Work work) {
    if (state_ == State::Running) return false;
    if (thread_.joinable()) thread_.join();
    label_ = std::move(label);
    cancel_.store(false);
    progress_.store(0.0f);
    done_.store(false);
    ok_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_.clear();
    }
    state_ = State::Running;
    thread_ = std::thread([this, work = std::move(work)]() {
        std::string message;
        const bool ok = work(cancel_, progress_, message);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = message;
        }
        ok_ = ok;
        done_.store(true);
    });
    return true;
}

void BackgroundTask::Cancel() {
    if (state_ == State::Running) cancel_.store(true);
}

BackgroundTask::State BackgroundTask::Poll() {
    if (state_ == State::Running && done_.load()) {
        thread_.join();
        state_ = ok_ ? State::Succeeded : (cancel_.load() ? State::Cancelled : State::Failed);
    }
    return state_;
}

std::string BackgroundTask::Message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

}  // namespace gdx_companion
