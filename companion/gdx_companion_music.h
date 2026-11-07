// Copyright (c) Zorkats
//
// The Music workspace without its page: the slots the game streams, reading a recording with the game's
// own decoders (port/third_party/music_decoders), choosing its loop, and installing it into <game>/music/.
//
// An install never edits the chosen recording. It writes a copy into a staging folder inside music/
// (the game does not scan subfolders), reopens that copy with the decoders and reads it to the end, and
// only then moves it into place. Every file in music/ with the same stem, which the game might pick
// instead, is first renamed to <name>.previous, which the game ignores; Restore swaps those back. When a
// step fails, the files already moved go back where they were.
//
// Level: with the level matched, the install measures the recording's loudness (EBU R128) and writes
// it as the tags the game reads (REPLAYGAIN_TRACK_GAIN and REPLAYGAIN_TRACK_PEAK in an Ogg or FLAC, a
// version 2 bext chunk in a WAV), so the game plays it at the stock music level. The audio itself is
// never re-encoded or scaled.
//
// The native path writes mono 16-bit WAVs at a native resource's own rate, with a .loop.json, for the
// sample packer (tools/gen_sample_pack.py), which the page then runs as a worker job.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gdx_companion {

struct MusicSlot {
    int id;
    const char* stem;
    const char* where;
    bool streams;  // false: the game plays this through the sound-effect player and ignores music/
};

// Every slot, in game order, from docs/modding/CUSTOM_MUSIC.md and port/gdx_music_files.h.
const std::vector<MusicSlot>& MusicSlots();

enum class AudioFormat { Unknown, Ogg, Flac, Wav };

AudioFormat FormatOf(const std::filesystem::path& path);
const char* FormatExtension(AudioFormat format);  // ".ogg", ".flac", ".wav", or ""
const char* FormatName(AudioFormat format);

struct AudioInfo {
    AudioFormat format = AudioFormat::Unknown;
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0;
    bool has_loop = false;  // the file carries loop tags the game reads
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;  // exclusive; frames when the file has no loop tags
    bool has_gain = false;  // the file carries a level tag the game reads
    double gain_db = 0;     // its ReplayGain 2 track gain
    bool has_peak = false;
    float peak = 0;         // its peak, 1.0 at full scale
};

// Opens path with the game's decoders. Returns false with error set when the game could not play it.
bool InspectAudio(const std::filesystem::path& path, AudioInfo& info, std::string& error);

enum class LoopPolicy {
    Keep,    // the recording's own loop tags; without them the whole recording repeats
    Whole,   // remove any loop tags, so the whole recording repeats
    Custom,  // repeat [start, end)
};

struct LoopChoice {
    LoopPolicy policy = LoopPolicy::Keep;
    uint64_t start = 0;  // frames
    uint64_t end = 0;    // frames, exclusive
};

// The level tags to write. When set, every REPLAYGAIN_TRACK_GAIN and REPLAYGAIN_TRACK_PEAK comment (or
// the WAV bext loudness) is replaced; otherwise the file's own are kept.
struct MusicLevel {
    bool set = false;
    double gain_db = 0;  // ReplayGain 2 track gain, to the -18 LUFS reference
    float peak = 0;      // sample peak, 1.0 at full scale
};

// What the page asks of an install.
struct LevelRequest {
    bool match = false;  // measure the recording and write its level
    double trim_db = 0;  // plays this much louder (or, negative, quieter) than the stock music level
};

// Every policy is offered for every format the game plays. Whole and Custom rewrite the tags: the smpl
// chunk of a WAV, the VORBIS_COMMENT block of a FLAC, the comment header of an Ogg Vorbis.
bool LoopPolicyOffered(AudioFormat format, LoopPolicy policy);

// The region the game will repeat for info under choice.
void EffectiveLoop(const AudioInfo& info, const LoopChoice& choice, uint64_t& start, uint64_t& end);

// Empty when choice can be used for info, or one sentence saying why not.
std::string CheckLoop(const AudioInfo& info, const LoopChoice& choice);

// Rewrites a WAV's loop: drops every smpl chunk and, for Custom, appends one with a single forward loop.
// Keep leaves the smpl chunks as they are. With level set, the loudness fields of the first bext chunk
// are written (the chunk is raised to version 2), or a new bext chunk goes before the data. Every other
// chunk is copied unchanged.
bool RewriteWavLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, uint32_t rate, std::vector<uint8_t>& out,
                    std::string& error, const MusicLevel& level = MusicLevel{});

// Rewrites a FLAC's loop: removes LOOPSTART, LOOPLENGTH and LOOPEND comments and, for Custom, adds
// LOOPSTART and LOOPLENGTH; Keep leaves them. With level set, the ReplayGain track comments are replaced.
// Other comments, metadata blocks and the audio frames are copied unchanged; padding blocks are dropped.
bool RewriteFlacLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, std::vector<uint8_t>& out,
                     std::string& error, const MusicLevel& level = MusicLevel{});

// Rewrites an Ogg Vorbis file's loop the same way, in its comment header. The comment and setup headers
// are laced into new pages; the identification page is copied as it is, and every audio page is copied
// with only its sequence number and checksum renewed. Refuses a damaged page, a file with more than one
// stream, and a stream whose headers are not laid out as the Vorbis specification requires.
bool RewriteOggLoop(const std::vector<uint8_t>& in, const LoopChoice& choice, std::vector<uint8_t>& out,
                    std::string& error, const MusicLevel& level = MusicLevel{});

// A course song stem: printable, no characters a file name cannot hold, at most 64 bytes, and not a
// game slot name (those replace game music instead). Empty when allowed, or one sentence.
std::string CheckCourseSongName(const std::string& stem);

struct InstallPlan {
    std::filesystem::path music_dir;
    std::filesystem::path destination;
    std::vector<std::filesystem::path> displaced;         // same stem in music/, each kept as <name>.previous
    std::vector<std::filesystem::path> replaced_backups;  // older .previous files those backups replace
    std::vector<std::filesystem::path> legacy;            // same stem in mods/music/, which music/ outranks
};

// What installing stem in format would move. Reads the folders; changes nothing.
InstallPlan PlanMusicInstall(const std::filesystem::path& game_dir, const std::string& stem, AudioFormat format);

// A new staging folder inside music/, created empty. Returns an empty path with error set on failure.
std::filesystem::path MakeMusicStaging(const std::filesystem::path& music_dir, std::string& error);

// Deletes staging when it is a staging folder directly inside music_dir. Returns false otherwise.
bool RemoveMusicStaging(const std::filesystem::path& staging, const std::filesystem::path& music_dir);

using CancelFlag = std::atomic<bool>;
using ProgressValue = std::atomic<float>;

// Writes source, with its loop rewritten under choice, to staged, then reopens staged and decodes it to the
// end. With request.match, first measures the recording and writes its level, trimmed by request.trim_db,
// into the copy; written (when not null) receives it. Fails when the result does not open, has a
// different rate, channel count or length, or does not carry the loop or level asked for, and when a
// recording to match is silent.
bool StageMusic(const std::filesystem::path& source, const AudioInfo& info, const LoopChoice& choice,
                const std::filesystem::path& staged, const CancelFlag& cancel, ProgressValue& progress,
                std::string& error, const LevelRequest& request = LevelRequest{}, MusicLevel* written = nullptr);

// Moves plan.displaced aside and staged to plan.destination. On failure every move is undone.
bool CommitMusicInstall(const InstallPlan& plan, const std::filesystem::path& staged, std::string& error);

struct RestorePlan {
    std::vector<std::filesystem::path> current;   // same stem in music/, which become .previous
    std::vector<std::filesystem::path> previous;  // <stem>.<ext>.previous, which come back
};

RestorePlan PlanMusicRestore(const std::filesystem::path& game_dir, const std::string& stem);

// Swaps current and previous. Fails, changing nothing, when there is no backup.
bool RestoreMusic(const RestorePlan& plan, std::string& error);

// Native sample targets, tools/music_targets.json schema 1.
struct NativeResource {
    std::string key;
    uint32_t rate_hz = 0;
    std::string channel;  // mono, part_0, part_1
};

struct NativeTarget {
    std::string id;
    std::string label;
    std::string stream_key;
    int bgm_id = 0;
    std::vector<NativeResource> cartridge;
    std::vector<NativeResource> expansion_kit;
};

inline constexpr uint64_t kNativeBudgetBytes = 1u << 20;

bool LoadNativeTargets(const std::filesystem::path& json, std::vector<NativeTarget>& out, std::string& error);

// round(frames * to / from), as the sample packer's resample_linear.
uint64_t ResampledFrames(uint64_t frames, uint32_t from, uint32_t to);

// Bytes of native sample memory for samples decoded samples: 9 per 16, rounded up.
uint64_t NativeBytes(uint64_t samples);

// Mixes source to mono, resamples it to rate, and writes wav (16-bit PCM). Writes loop_json with the loop
// under choice, in output samples, its start rounded down to a multiple of 16.
bool WriteNativeWav(const std::filesystem::path& source, const AudioInfo& info, uint32_t rate,
                    const LoopChoice& choice, const std::filesystem::path& wav, const std::filesystem::path& loop_json,
                    const CancelFlag& cancel, ProgressValue& progress, std::string& error);

// One piece of slow work on a background thread: staging, verifying, writing native WAVs. The page polls
// it each frame; Cancel asks the work to stop at its next check.
class BackgroundTask {
public:
    enum class State { Idle, Running, Succeeded, Failed, Cancelled };
    using Work = std::function<bool(const CancelFlag& cancel, ProgressValue& progress, std::string& message)>;

    BackgroundTask() = default;
    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;
    ~BackgroundTask();

    // Refused while a task runs.
    bool Start(std::string label, Work work);
    void Cancel();
    // Joins a finished thread and returns the state. Succeeded, Failed and Cancelled stay until Start.
    State Poll();
    bool Running() const { return state_ == State::Running; }
    float Progress() const { return progress_.load(); }
    std::string Message() const;
    const std::string& Label() const { return label_; }

private:
    std::thread thread_;
    std::atomic<bool> done_{false};
    CancelFlag cancel_{false};
    ProgressValue progress_{0.0f};
    State state_ = State::Idle;
    bool ok_ = false;
    mutable std::mutex mutex_;
    std::string message_;
    std::string label_;
};

}  // namespace gdx_companion
