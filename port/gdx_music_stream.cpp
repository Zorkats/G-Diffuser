// port/gdx_music_stream.cpp -- streamed BGM replacement (plan section 6A). The design is in
// port/gdx_music_stream.h.
//
// STATE. One atomic state word drives the stream: Idle, Loading, Playing, Draining. The game thread
// and the worker change it under Shared::mutex. The audio thread reads it lock-free and changes it
// only with a compare-exchange from Draining to Idle, so it never blocks.
//
// RING. A single-producer, single-consumer ring of stereo float frames at the source rate. The
// worker owns `written`, the mixer owns `consumed`, and both are monotonic 64-bit counts, so there
// is no wrap ambiguity. `sessionBase` and `epoch` start a new stream: the worker records the base,
// then bumps the epoch, and the mixer rewinds `consumed` to the base on the first call that sees the
// new epoch. Frames from the old stream sit below the base and are never played.
//
// GAIN. Native replacements follow the Music slider and raw seq player 1 fade, read as
// fadeVolume x fadeVolumeScale through Audio_PortBgmStreamFade. The stock fade-out on
// Audio_DDBgmStop and the pause ducking therefore reach the stream. Draining ends when that fade
// reaches zero or after a 4 s timeout. Explicit editor previews instead follow pause gain, because
// the editor may have disabled the native player before requesting a preview.

#include "gdx_music_stream.h"

#include "gdx_audio_mix.h"
#include "gdx_music_files.h"
#include "port_log.h"
#include "third_party/music_decoders/decoders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// Defined in libultraship (C ABI).
extern "C" int CVarGetInteger(const char* name, int defaultValue);

// Defined in decomp/src/audio/disk/external.c under PORT. The raw seq player 1 fade, or 0 when the
// player is disabled. Called on the audio thread.
extern "C" float Audio_PortBgmStreamFade(void);
extern "C" float Audio_PortBgmPreviewGain(void);

namespace {

constexpr uint64_t kRingFrames = 1u << 17; // about 3 s at 44.1 kHz; a power of two
constexpr uint64_t kRingMask = kRingFrames - 1;
constexpr uint64_t kChunkFrames = 2048;
constexpr double kPrefillSeconds = 0.25;
constexpr double kDrainTimeoutSeconds = 4.0;
using gdx::music::kBgmNone;
using gdx::music::kBgmNamedCount;
constexpr const char* kStreamedMusicCVar = "gEnhancements.Audio.StreamedMusic";
constexpr const char* kMatchLoudnessCVar = "gEnhancements.Audio.MatchMusicLoudness";

using gdx::music::kBgmNames;

enum StreamState : int {
    kIdle = 0,
    kLoading = 1,
    kPlaying = 2,
    kDraining = 3,
};

// Shared between the game thread, the worker and the audio thread. Allocated once and never freed,
// because the detached worker can outlive static destruction at exit.
struct Shared {
    Shared() : ring(kRingFrames * 2, 0.0f) {}

    std::atomic<int> state{ kIdle };
    std::atomic<uint32_t> sourceRate{ 0 };
    std::atomic<float> levelGain{ 1.0f };  // worker, stored before the epoch bump
    std::vector<float> ring;              // interleaved stereo frames, kRingFrames long
    std::atomic<uint64_t> written{ 0 };    // worker only
    std::atomic<uint64_t> consumed{ 0 };   // audio thread only
    std::atomic<uint64_t> sessionBase{ 0 }; // worker, stored before the epoch bump
    std::atomic<uint32_t> epoch{ 0 };
    std::atomic<uint64_t> request{ 0 };
    std::atomic<bool> preview{ false };
    std::atomic<bool> mixing{ false };

    std::mutex mutex;
    std::condition_variable cv;
    bool hasPending = false;        // guarded by mutex
    bool pendingPreview = false;
    std::string pendingPath;        // guarded by mutex
    std::string pendingLabel;       // guarded by mutex
    std::atomic<bool> pendingFlag{ false }; // mirror of hasPending for the worker's poll
    std::once_flag workerOnce;
};

Shared& shared() {
    static Shared& s = *new Shared();
    return s;
}

// Worker-only session: the open decoder and the next source frame to read.
struct Session {
    GdxMusicDecoder* dec = nullptr;
    GdxMusicInfo info{};
    uint64_t pos = 0;
    uint64_t request = 0;
    std::string label;
    std::vector<float> raw;
};

Session& session() {
    static Session& s = *new Session();
    return s;
}

// Audio-thread-only mixer state.
struct MixState {
    uint32_t epoch = 0;
    float level = 1.0f; // the session's level gain, latched with its epoch
    double phase = 0.0;
    uint64_t drainFrames = 0;
    int lastState = kIdle;
};

MixState& mixState() {
    static MixState& m = *new MixState();
    return m;
}

int16_t clampSample(int32_t v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(v);
}

int32_t roundToInt(float v) {
    return static_cast<int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

void closeSession() {
    Session& ss = session();
    if (ss.dec != nullptr) {
        gdx_music_decoder_close(ss.dec);
        ss.dec = nullptr;
    }
    ss.info = GdxMusicInfo{};
    ss.pos = 0;
    ss.label.clear();
}

// Ends the stream after a decode failure. A request that arrived meanwhile owns the state word, so it
// is left alone.
void failStream(const char* name, const char* why, uint64_t request) {
    Shared& s = shared();
    gdx_port_logf("[music] stream %s stopped: %s\n", name, why);
    closeSession();
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.request.load(std::memory_order_acquire) == request) {
            s.state.store(kIdle, std::memory_order_release);
            gdx_audio_seq_player_stream_mute(0);
        }
    }
}

void openSession(const std::string& path, const std::string& label, uint64_t request, bool preview) {
    Shared& s = shared();
    Session& ss = session();
    GdxMusicInfo info;
    GdxMusicDecoder* dec = gdx_music_decoder_open(path.c_str(), &info);

    if (dec == nullptr) {
        failStream(label.c_str(), "file could not be decoded", request);
        return;
    }
    if (info.sampleRate == 0 || info.totalFrames == 0 || info.loopEnd <= info.loopStart ||
        info.loopEnd > info.totalFrames) {
        gdx_music_decoder_close(dec);
        failStream(label.c_str(), "file has no usable length", request);
        return;
    }
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.request.load(std::memory_order_acquire) != request) {
        gdx_music_decoder_close(dec);
        return;
    }
    ss.dec = dec;
    ss.info = info;
    ss.pos = 0;
    ss.request = request;
    ss.label = label;
    ss.raw.assign(kChunkFrames * 2, 0.0f);

    // The file's ReplayGain or bext loudness brings it to the stock music level. Files without one play
    // as decoded.
    const bool match = CVarGetInteger(kMatchLoudnessCVar, 1) != 0;
    const float level = match ? gdx_music_level_gain(&info) : 1.0f;
    if (info.hasGain != 0) {
        gdx_port_logf("[music] %s: level %+.2f dB (track gain %+.2f dB, peak %.3f)%s\n", label.c_str(),
                      20.0f * std::log10(gdx_music_level_gain(&info)), info.gainDb,
                      info.hasPeak != 0 ? info.peak : 0.0f, match ? "" : ", matching off");
    }

    s.sourceRate.store(info.sampleRate, std::memory_order_release);
    s.levelGain.store(level, std::memory_order_release);
    s.sessionBase.store(s.written.load(std::memory_order_relaxed), std::memory_order_release);
    s.preview.store(preview, std::memory_order_release);
    s.epoch.fetch_add(1, std::memory_order_release);
}

// Fills the ring while there is room and no newer request is waiting. Each read stops at loopEnd so
// the decoder is repositioned to loopStart exactly when the loop wraps.
void produceFrames() {
    Shared& s = shared();
    Session& ss = session();
    const uint32_t ch = ss.info.channels;

    while (ss.dec != nullptr && s.request.load(std::memory_order_acquire) == ss.request &&
           !s.pendingFlag.load(std::memory_order_acquire)) {
        const uint64_t w = s.written.load(std::memory_order_relaxed);
        const uint64_t c = s.consumed.load(std::memory_order_acquire);
        const uint64_t base = std::max(c, s.sessionBase.load(std::memory_order_relaxed));
        const uint64_t used = w - base;
        if (used >= kRingFrames) {
            break;
        }
        uint64_t n = std::min<uint64_t>(kChunkFrames, kRingFrames - used);
        n = std::min<uint64_t>(n, ss.info.loopEnd - ss.pos);

        const uint64_t got = gdx_music_decoder_read(ss.dec, ss.raw.data(), n);
        if (s.request.load(std::memory_order_acquire) != ss.request) {
            return;
        }
        if (got == 0) {
            // Either the file ended early or the decoder stalled. Restart at the loop start once; if
            // the read is still empty there, the file is unusable.
            if (ss.pos == ss.info.loopStart || !gdx_music_decoder_seek(ss.dec, ss.info.loopStart)) {
                failStream(ss.label.c_str(), "decoder stalled", ss.request);
                return;
            }
            ss.pos = ss.info.loopStart;
            continue;
        }

        for (uint64_t i = 0; i < got; i++) {
            float* dst = s.ring.data() + ((w + i) & kRingMask) * 2;
            const float* src = ss.raw.data() + i * ch;
            dst[0] = src[0];
            dst[1] = ch == 2 ? src[1] : src[0];
        }
        s.written.store(w + got, std::memory_order_release);

        ss.pos += got;
        if (ss.pos >= ss.info.loopEnd) {
            if (!gdx_music_decoder_seek(ss.dec, ss.info.loopStart)) {
                failStream(ss.label.c_str(), "loop seek failed", ss.request);
                return;
            }
            ss.pos = ss.info.loopStart;
        }
    }
}

void workerTick(bool wait) {
    Shared& s = shared();
    std::string path;
    std::string label;
    bool haveRequest = false;
    bool preview = false;
    uint64_t request = 0;
    {
        std::unique_lock<std::mutex> lk(s.mutex);
        if (wait) {
            s.cv.wait_for(lk, std::chrono::milliseconds(5), [&s] {
                return s.hasPending && !s.mixing.load(std::memory_order_seq_cst);
            });
        }
        if (s.hasPending) {
            if (s.mixing.load(std::memory_order_seq_cst)) {
                return;
            }
            path.swap(s.pendingPath);
            label.swap(s.pendingLabel);
            preview = s.pendingPreview;
            request = s.request.load(std::memory_order_acquire);
            s.hasPending = false;
            s.pendingFlag.store(false, std::memory_order_release);
            haveRequest = true;
        }
    }
    if (haveRequest) {
        closeSession();
        openSession(path, label, request, preview);
        return;
    }

    Session& ss = session();
    if (ss.dec == nullptr) {
        return;
    }
    if (s.state.load(std::memory_order_acquire) == kIdle ||
        s.request.load(std::memory_order_acquire) != ss.request) {
        closeSession();
        std::lock_guard<std::mutex> lk(s.mutex);
        if (s.state.load(std::memory_order_acquire) == kIdle) {
            gdx_audio_seq_player_stream_mute(0);
        }
        return;
    }

    produceFrames();

    if (ss.dec != nullptr && s.state.load(std::memory_order_acquire) == kLoading) {
        const uint64_t prefill = static_cast<uint64_t>(kPrefillSeconds * s.sourceRate.load(std::memory_order_acquire));
        const uint64_t ready = s.written.load(std::memory_order_acquire) - s.sessionBase.load(std::memory_order_acquire);
        if (ready >= prefill) {
            std::lock_guard<std::mutex> lk(s.mutex);
            if (s.state.load(std::memory_order_acquire) == kLoading && !s.hasPending &&
                s.request.load(std::memory_order_acquire) == ss.request) {
                s.state.store(kPlaying, std::memory_order_release);
            }
        }
    }
}

void workerMain() {
    for (;;) {
        workerTick(true);
    }
}

int queueStream(const char* path, const char* label, bool preview) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    Shared& s = shared();
#ifndef GDX_MUSIC_STREAM_TEST
    std::call_once(s.workerOnce, [] { std::thread(workerMain).detach(); });
#endif
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        s.request.fetch_add(1, std::memory_order_acq_rel);
        s.pendingPath = path;
        s.pendingLabel = label != nullptr ? label : path;
        s.pendingPreview = preview;
        s.hasPending = true;
        s.pendingFlag.store(true, std::memory_order_release);
        s.state.store(kLoading, std::memory_order_seq_cst);
        gdx_audio_seq_player_stream_mute(1);
    }
    s.cv.notify_one();
    return 1;
}

}  // namespace

extern "C" uint64_t gdx_music_loop_wrap(uint64_t pos, uint64_t loopStart, uint64_t loopEnd) {
    if (loopEnd <= loopStart || pos < loopEnd) {
        return pos;
    }
    return loopStart + (pos - loopStart) % (loopEnd - loopStart);
}

extern "C" int gdx_music_stream_key_for_bgm(int bgmId, char* out, size_t outCap) {
    if (out == nullptr || outCap == 0 || bgmId < 0 || bgmId == kBgmNone) {
        return 0;
    }
    if (bgmId < kBgmNamedCount) {
        std::snprintf(out, outCap, "%s", kBgmNames[bgmId]);
    } else {
        std::snprintf(out, outCap, "bgm_%02x", bgmId);
    }
    return 1;
}

extern "C" int gdx_music_stream_find_file(int bgmId, char* outPath, size_t outCap) {
    char key[32];
    char candidate[1024];

    if (outPath == nullptr || outCap == 0 || gdx_music_stream_key_for_bgm(bgmId, key, sizeof key) == 0) {
        return 0;
    }
    for (const char* dir : gdx::music::kReplacementFolders) {
        if (gdx_exe_relative_path(candidate, sizeof candidate, dir) == nullptr) {
            continue;
        }
        const std::string found = gdx::music::FindFile(candidate, key).string();
        if (!found.empty()) {
            if (found.size() >= outCap) {
                return 0;
            }
            std::snprintf(outPath, outCap, "%s", found.c_str());
            return 1;
        }
    }
    return 0;
}

namespace {

// Course override (7A): one file that replaces the stream for one BGM id until the BGM stops.
// Game thread only, like bgm_start and bgm_stop.
struct CourseOverride {
    bool active = false;
    int bgmId = 0;
    char path[1024] = {};
    char label[64] = {};
};

CourseOverride& courseOverride() {
    static CourseOverride override;
    return override;
}

}  // namespace

extern "C" void gdx_music_stream_set_course_override(int bgmId, const char* path, const char* label) {
    CourseOverride& o = courseOverride();
    if (path == nullptr || path[0] == '\0') {
        o.active = false;
        return;
    }
    std::snprintf(o.path, sizeof o.path, "%s", path);
    std::snprintf(o.label, sizeof o.label, "%s", label != nullptr ? label : path);
    o.bgmId = bgmId;
    o.active = true;
}

extern "C" void gdx_music_stream_bgm_start(int bgmId) {
    char key[32];
    char path[1024];
    const int haveKey = gdx_music_stream_key_for_bgm(bgmId, key, sizeof key);
    const bool enabled = CVarGetInteger(kStreamedMusicCVar, 1) != 0;
    const CourseOverride& o = courseOverride();

    if (o.active && o.bgmId == bgmId && enabled) {
        gdx_port_logf("[music] BGM %s (id %d): course override %s\n", o.label, bgmId, o.path);
        queueStream(o.path, o.label, false);
        return;
    }

    const bool found = haveKey != 0 && enabled && gdx_music_stream_find_file(bgmId, path, sizeof path) != 0;

    gdx_port_logf("[music] BGM %s (id %d): %s\n", haveKey != 0 ? key : "none", bgmId,
                  found ? path : (enabled ? "no file, stock track" : "streaming off, stock track"));

    if (found) {
        queueStream(path, key, false);
        return;
    }

    // The stock track replaces whatever was streaming, so that stream is cut rather than drained.
    Shared& s = shared();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.state.load(std::memory_order_acquire) != kIdle || s.hasPending) {
        s.request.fetch_add(1, std::memory_order_acq_rel);
        s.hasPending = false;
        s.pendingFlag.store(false, std::memory_order_release);
        s.state.store(kIdle, std::memory_order_release);
        gdx_audio_seq_player_stream_mute(0);
    }
}

extern "C" void gdx_music_stream_bgm_stop(void) {
    courseOverride().active = false;
    Shared& s = shared();
    std::lock_guard<std::mutex> lk(s.mutex);
    const int st = s.state.load(std::memory_order_acquire);
    if (st == kPlaying && !s.preview.load(std::memory_order_acquire)) {
        // The audio thread keeps mixing, following the stock fade-out, until it reaches zero.
        s.state.store(kDraining, std::memory_order_release);
    } else if (st != kIdle) {
        s.request.fetch_add(1, std::memory_order_acq_rel);
        s.hasPending = false;
        s.pendingFlag.store(false, std::memory_order_release);
        s.state.store(kIdle, std::memory_order_release);
        gdx_audio_seq_player_stream_mute(0);
    }
}

extern "C" int gdx_music_stream_play_file(const char* path, const char* label) {
    return queueStream(path, label, true);
}

extern "C" int gdx_music_stream_is_mixing(void) {
    const int st = shared().state.load(std::memory_order_acquire);
    return st == kPlaying || st == kDraining;
}

extern "C" int gdx_music_stream_mix_s16(int16_t* stereo, size_t frames, uint32_t deviceRate) {
    Shared& s = shared();
    // A replacement cannot reuse the ring until an already-running callback has left it.
    struct MixingGuard {
        explicit MixingGuard(Shared& value) : shared(value) {
            shared.mixing.store(true, std::memory_order_seq_cst);
        }
        ~MixingGuard() {
            shared.mixing.store(false, std::memory_order_seq_cst);
        }
        Shared& shared;
    } guard(s);
    MixState& m = mixState();
    const int state = s.state.load(std::memory_order_seq_cst);
    const int prev = m.lastState;
    m.lastState = state;

    if (state != kPlaying && state != kDraining) {
        return 0;
    }
    if (state == kDraining && prev != kDraining) {
        m.drainFrames = 0;
    }
    if (stereo == nullptr || frames == 0 || deviceRate == 0) {
        return 0;
    }
    const uint32_t rate = s.sourceRate.load(std::memory_order_acquire);
    if (rate == 0) {
        return 0;
    }

    const uint32_t epoch = s.epoch.load(std::memory_order_acquire);
    if (epoch != m.epoch) {
        m.epoch = epoch;
        m.level = s.levelGain.load(std::memory_order_acquire);
        m.phase = 0.0;
        s.consumed.store(s.sessionBase.load(std::memory_order_acquire), std::memory_order_release);
    }

    float fade = s.preview.load(std::memory_order_acquire) ? Audio_PortBgmPreviewGain() : Audio_PortBgmStreamFade();
    fade = std::min(1.0f, std::max(0.0f, fade));

    if (state == kDraining) {
        m.drainFrames += frames;
        const bool timedOut = m.drainFrames >= static_cast<uint64_t>(kDrainTimeoutSeconds * deviceRate);
        if (fade <= 0.0f || timedOut) {
            int expected = kDraining;
            s.state.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel);
            return 0;
        }
    }

    const float gain = fade * m.level * gdx_audio_music_slider_scale() * 32767.0f;
    const double step = static_cast<double>(rate) / static_cast<double>(deviceRate);
    const float* ring = s.ring.data();
    uint64_t c = s.consumed.load(std::memory_order_relaxed);
    const uint64_t w = s.written.load(std::memory_order_acquire);

    for (size_t i = 0; i < frames; i++) {
        // Underrun: the frame stays silent and the phase holds, so playback resumes in step.
        if (c + 1 >= w) {
            continue;
        }
        const float t = static_cast<float>(m.phase);
        const float* a = ring + (c & kRingMask) * 2;
        const float* b = ring + ((c + 1) & kRingMask) * 2;
        const float left = (a[0] + (b[0] - a[0]) * t) * gain;
        const float right = (a[1] + (b[1] - a[1]) * t) * gain;
        int16_t* out = stereo + i * 2;
        out[0] = clampSample(out[0] + roundToInt(left));
        out[1] = clampSample(out[1] + roundToInt(right));

        m.phase += step;
        while (m.phase >= 1.0) {
            m.phase -= 1.0;
            c++;
        }
    }
    s.consumed.store(c, std::memory_order_release);
    return 1;
}
