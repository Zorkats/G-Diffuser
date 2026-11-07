// Copyright (c) Zorkats
//
// Loop editing for the Music workspace without its page: a mono copy of a recording for the waveform,
// snapping a marker to a zero crossing, measuring how smooth a loop's seam is, moving a loop's end so it
// lines up with its start, and finding loop points in a recording that has none.
//
// A loop [start, end) is smooth when the frames around end sound like the frames around start: the game
// then jumps from end back to start without a click or a skip in the music. Everything here works on the
// channel average of the game's own decoding, so the page shows what the game plays.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gdx_companion_music.h"

namespace gdx_companion {

inline constexpr uint64_t kPeakBlockFrames = 256;
// About 11 minutes at 48 kHz, 128 MiB of samples. Longer recordings get no waveform.
inline constexpr uint64_t kMaxLoopAudioFrames = 32ull << 20;

struct LoopAudio {
    uint32_t rate = 0;
    uint64_t frames = 0;
    std::vector<float> mono;       // frames samples, the average of the channels
    std::vector<float> block_min;  // the lowest sample of each kPeakBlockFrames block
    std::vector<float> block_max;
    bool has_loudness = false;  // false for a silent recording
    double lufs = 0;            // integrated loudness (gdx_companion_loudness.h)
    float peak = 0;             // sample peak, 1.0 at full scale
};

// Takes mono as the recording and builds its block peaks.
void SetLoopAudio(LoopAudio& audio, uint32_t rate, std::vector<float> mono);

// Decodes path with the game's decoders into audio, measuring its loudness on the way. Fails for
// recordings over kMaxLoopAudioFrames.
bool ReadLoopAudio(const std::filesystem::path& path, const CancelFlag& cancel, ProgressValue& progress,
                   LoopAudio& audio, std::string& error);

// The lowest and highest sample in [from, to), read from the block peaks where the range covers them.
void PeakRange(const LoopAudio& audio, uint64_t from, uint64_t to, float& low, float& high);

// The frame closest to frame, within radius, where the waveform changes sign (the frame after the
// change). frame itself when there is none.
uint64_t NearestZeroCrossing(const LoopAudio& audio, uint64_t frame, uint64_t radius);

// How alike the frames around end are to the frames around start, from -1 to 1, where 1 is a seamless
// jump. False when there is nothing to compare: a loop over the whole recording.
bool SeamMatch(const LoopAudio& audio, uint64_t start, uint64_t end, float& match);

// The end within radius of end whose seam with start matches best; end itself when no other is better.
uint64_t AlignLoopEnd(const LoopAudio& audio, uint64_t start, uint64_t end, uint64_t radius);

struct LoopCandidate {
    uint64_t start = 0;
    uint64_t end = 0;     // exclusive
    float match = 0.0f;   // SeamMatch of the result
    float distance = 0.0f;  // the mean spectral difference of the four seconds before each point, in dB
};

// Up to count loops at least five seconds long, best first. The search compares the spectrum of every
// four-second stretch of the recording with every earlier one, keeps the best end for each loop length,
// takes the lengths that differ by at least two seconds, then lines each loop up to the frame and puts
// its end on a zero crossing. Quiet stretches are not used as loop points. Fails, with error set, for
// recordings shorter than 12 seconds.
bool FindLoopPoints(const LoopAudio& audio, size_t count, const CancelFlag& cancel, ProgressValue& progress,
                    std::vector<LoopCandidate>& out, std::string& error);

// "m:ss.mmm".
std::string FormatLoopTime(uint64_t frame, uint32_t rate);

// Reads "m:ss.mmm", "h:mm:ss.mmm" or plain seconds such as "12.5" into a frame. False for anything else.
bool ParseLoopTime(const std::string& text, uint32_t rate, uint64_t& frame);

}  // namespace gdx_companion
