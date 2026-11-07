// Copyright (c) Zorkats
//
// Headless tests for loop editing: block peaks, zero crossings, the seam match, lining a loop's end up
// with its start, the loop finder on a synthesized song whose body repeats, reading a recording through
// the game's decoders, and the time field format. Every recording is synthesized here; no game audio is
// used. Links gdx_companion_host only.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gdx_companion_loop.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

constexpr double kPi = 3.14159265358979323846;

// A small deterministic generator, so every run tests the same signal.
struct Lcg {
    uint32_t state;
    uint32_t Next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    float Unit() { return static_cast<float>(Next() & 0xFFFF) / 32768.0f - 1.0f; }
};

// seconds of quarter-second notes from a pentatonic table, with a little noise, all from seed.
std::vector<float> Phrase(uint32_t rate, double seconds, uint32_t seed) {
    static const double kNotes[] = {220.0, 247.5, 277.2, 330.0, 370.0, 440.0, 495.0, 554.4, 660.0, 740.0};
    Lcg lcg{seed};
    const size_t frames = static_cast<size_t>(seconds * rate);
    const size_t note_frames = rate / 4;
    std::vector<float> out(frames);
    double frequency = kNotes[0];
    double phase = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        if (i % note_frames == 0) frequency = kNotes[lcg.Next() % 10];
        phase += 2.0 * kPi * frequency / rate;
        const double envelope = 1.0 - 0.6 * static_cast<double>(i % note_frames) / note_frames;
        out[i] = static_cast<float>(0.4 * envelope * std::sin(phase)) + 0.02f * lcg.Unit();
    }
    return out;
}

// An intro, then a body that repeats twice and starts a third time.
struct Song {
    uint32_t rate = 22050;
    uint64_t intro = 0;
    uint64_t body = 0;
    std::vector<float> samples;
};

Song MakeSong() {
    Song song;
    const std::vector<float> intro = Phrase(song.rate, 3.0, 7);
    const std::vector<float> body = Phrase(song.rate, 8.0, 99);
    song.intro = intro.size();
    song.body = body.size();
    song.samples = intro;
    song.samples.insert(song.samples.end(), body.begin(), body.end());
    song.samples.insert(song.samples.end(), body.begin(), body.end());
    song.samples.insert(song.samples.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(song.rate * 4));
    return song;
}

void Peaks() {
    Lcg lcg{1};
    std::vector<float> noise(5000);
    for (float& value : noise) value = lcg.Unit();
    LoopAudio audio;
    SetLoopAudio(audio, 8000, noise);
    bool ok = audio.frames == 5000 && audio.block_min.size() == (5000 + kPeakBlockFrames - 1) / kPeakBlockFrames;
    const uint64_t ranges[][2] = {{0, 5000}, {3, 7}, {100, 900}, {255, 257}, {256, 512}, {4999, 5000}, {1000, 4321}};
    for (const auto& range : ranges) {
        float low = 0.0f;
        float high = 0.0f;
        PeakRange(audio, range[0], range[1], low, high);
        const auto expect = std::minmax_element(noise.begin() + static_cast<std::ptrdiff_t>(range[0]),
                                                noise.begin() + static_cast<std::ptrdiff_t>(range[1]));
        ok = ok && low == *expect.first && high == *expect.second;
    }
    Check(ok, "block peaks match a sample by sample scan for every range");
    float low = 1.0f;
    float high = 1.0f;
    PeakRange(audio, 4000, 4000, low, high);
    Check(low == 0.0f && high == 0.0f, "an empty range has no peak");
}

void ZeroCrossings() {
    std::vector<float> sine(1000);
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(2.0 * kPi * (i + 0.5) / 100.0));
    LoopAudio audio;
    SetLoopAudio(audio, 8000, sine);
    // The sine changes sign between frames 49 and 50, 99 and 100, and so on.
    Check(NearestZeroCrossing(audio, 47, 10) == 50, "the nearest crossing after the frame is found");
    Check(NearestZeroCrossing(audio, 103, 10) == 100, "the nearest crossing before the frame is found");
    Check(NearestZeroCrossing(audio, 75, 5) == 75, "the frame stays when no crossing is within reach");
}

void Seams() {
    const Song song = MakeSong();
    LoopAudio audio;
    SetLoopAudio(audio, song.rate, song.samples);
    const uint64_t start = song.intro + song.rate;  // one second into the first body
    const uint64_t end = start + song.body;
    float match = 0.0f;
    Check(SeamMatch(audio, start, end, match) && match > 0.999f, "a loop of exactly one body is seamless");
    float off = 0.0f;
    Check(SeamMatch(audio, start, end + 37, off) && off < 0.98f, "an end 37 frames late matches less");
    Check(!SeamMatch(audio, 0, audio.frames, match), "a loop over the whole recording has nothing to compare");
    Check(!SeamMatch(audio, 10, 10, match), "an empty loop is refused");

    Check(AlignLoopEnd(audio, start, end + 37, song.rate / 20) == end, "a late end is moved back onto the body");
    Check(AlignLoopEnd(audio, start, end - 211, song.rate / 20) == end, "an early end is moved forward onto the body");
    Check(AlignLoopEnd(audio, start, end, song.rate / 20) == end, "an aligned end stays");

    std::vector<float> silence(20000, 0.0f);
    LoopAudio quiet;
    SetLoopAudio(quiet, 8000, silence);
    Check(SeamMatch(quiet, 5000, 15000, match) && match == 1.0f, "silence joins silence seamlessly");
}

void Finder() {
    const Song song = MakeSong();
    LoopAudio audio;
    SetLoopAudio(audio, song.rate, song.samples);
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    std::vector<LoopCandidate> found;
    std::string error;
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = FindLoopPoints(audio, 5, cancel, progress, found, error);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("finder: %zu candidates in %.2f s%s%s\n", found.size(), seconds, error.empty() ? "" : ", ",
                error.c_str());
    for (const LoopCandidate& candidate : found) {
        std::printf("  start %llu end %llu length %llu match %.4f distance %.3f\n",
                    static_cast<unsigned long long>(candidate.start), static_cast<unsigned long long>(candidate.end),
                    static_cast<unsigned long long>(candidate.end - candidate.start), candidate.match,
                    candidate.distance);
    }
    Check(ok && !found.empty() && found.size() <= 5, "the finder suggests up to five loops");
    if (!ok || found.empty()) return;
    const LoopCandidate& top = found.front();
    const uint64_t length = top.end - top.start;
    const uint64_t repeats = (length + song.body / 2) / song.body;
    const uint64_t expected = repeats * song.body;
    const uint64_t miss = length > expected ? length - expected : expected - length;
    Check(repeats >= 1 && miss <= 2, "the best loop is a whole number of bodies long, to two frames");
    Check(top.match > 0.95f, "the best loop's seam matches");
    Check(top.start >= song.intro - song.rate / 2 && top.end <= audio.frames, "the best loop sits inside the song");
    Check(progress.load() == 1.0f, "the finder reports its progress to the end");
    bool long_enough = true;
    for (const LoopCandidate& candidate : found) {
        long_enough = long_enough && candidate.end - candidate.start >= static_cast<uint64_t>(4.9 * song.rate);
    }
    Check(long_enough, "every suggestion is at least five seconds long");

    LoopAudio short_audio;
    SetLoopAudio(short_audio, song.rate, Phrase(song.rate, 5.0, 3));
    Check(!FindLoopPoints(short_audio, 5, cancel, progress, found, error) && !error.empty(),
          "a recording under 12 seconds is refused with a reason");
    CancelFlag stop{true};
    Check(!FindLoopPoints(audio, 5, stop, progress, found, error) && error == "cancelled", "the finder can be cancelled");
}

void Le16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Le32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void Reading(const fs::path& dir) {
    // A stereo 16-bit WAV whose left channel is +0.5 and right channel is -0.25 of full scale.
    const uint32_t frames = 3000;
    std::vector<uint8_t> wav = {'R', 'I', 'F', 'F'};
    Le32(wav, 36 + frames * 4);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Le32(wav, 16);
    Le16(wav, 1);
    Le16(wav, 2);
    Le32(wav, 8000);
    Le32(wav, 8000 * 4);
    Le16(wav, 4);
    Le16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    Le32(wav, frames * 4);
    for (uint32_t f = 0; f < frames; ++f) {
        Le16(wav, 16384);
        Le16(wav, static_cast<uint16_t>(-8192));
    }
    const fs::path path = dir / "stereo.wav";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    }
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    LoopAudio audio;
    std::string error;
    Check(ReadLoopAudio(path, cancel, progress, audio, error) && audio.frames == frames && audio.rate == 8000,
          "a recording is read through the game's decoder");
    Check(std::fabs(audio.mono[100] - 0.125f) < 1e-3f && progress.load() == 1.0f, "the channels are averaged");
    Check(!ReadLoopAudio(dir / "missing.wav", cancel, progress, audio, error) && !error.empty(),
          "a missing recording fails with a reason");
}

void Times() {
    Check(FormatLoopTime(3000000, 48000) == "1:02.500", "a frame formats as m:ss.mmm");
    Check(FormatLoopTime(0, 0) == "0:00.000", "no rate formats as zero");
    uint64_t frame = 0;
    Check(ParseLoopTime("1:02.500", 48000, frame) && frame == 3000000, "m:ss.mmm reads back");
    Check(ParseLoopTime(" 12.5 ", 48000, frame) && frame == 600000, "plain seconds read");
    Check(ParseLoopTime("1:00:00", 1000, frame) && frame == 3600000, "h:mm:ss reads");
    Check(ParseLoopTime("0:00.0001", 48000, frame) && frame == 5, "a fraction rounds to the nearest frame");
    Check(!ParseLoopTime("1:75", 48000, frame), "seconds past 59 are refused");
    Check(!ParseLoopTime("", 48000, frame) && !ParseLoopTime("abc", 48000, frame) &&
              !ParseLoopTime("1.2:3", 48000, frame) && !ParseLoopTime("1::2", 48000, frame) &&
              !ParseLoopTime("-1", 48000, frame) && !ParseLoopTime("1:2:3:4", 48000, frame),
          "malformed times are refused");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_loop_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    Peaks();
    ZeroCrossings();
    Seams();
    Finder();
    Reading(base);
    Times();

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
