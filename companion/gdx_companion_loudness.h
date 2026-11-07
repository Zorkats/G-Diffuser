// Copyright (c) Zorkats
//
// Loudness for the Music workspace: the integrated loudness of a recording, measured as ITU-R BS.1770-4
// and EBU R128 define it (K-weighting, 400 ms blocks every 100 ms, an absolute gate at -70 LUFS and a
// relative gate 10 LU below the ungated level), and its sample peak.
//
// The game plays a mono recording on both speakers, so a mono recording is measured as dual mono: the
// same channel counted twice. The sample peak is the largest decoded sample; the game resamples by
// linear interpolation, which never exceeds it, so it is the right bound for clipping.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gdx_companion {

class LoudnessMeter {
public:
    // channels is 1 or 2.
    LoudnessMeter(uint32_t rate, uint32_t channels);

    // Adds frames of interleaved samples, 1.0 at full scale.
    void Add(const float* interleaved, size_t frames);

    // The integrated loudness in LUFS of everything added. False when no 400 ms block passes the gates:
    // the recording is silent or shorter than 400 ms.
    bool Integrated(double& lufs) const;

    // The largest absolute sample added, 1.0 at full scale.
    float Peak() const { return peak_; }

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
    };
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    static double Run(const Biquad& f, State& s, double x);

    uint32_t channels_;
    double weight_;     // 2 for dual mono, 1 per stereo channel
    size_t quarter_;    // frames in 100 ms
    Biquad shelf_{};    // stage 1: the head's high shelf
    Biquad highpass_{}; // stage 2: the revised low-frequency B-curve
    State state_[2][2]; // [channel][stage]
    double energy_ = 0; // the current 100 ms, weighted squares summed over channels
    size_t filled_ = 0; // frames in the current 100 ms
    std::vector<double> quarters_; // the energy of each finished 100 ms
    float peak_ = 0.0f;
};

struct LoudnessResult {
    double lufs = 0;   // integrated loudness
    float peak = 0;    // sample peak, 1.0 at full scale
};

// Decodes path to the end with the game's decoders and measures it. progress moves from `from` to `to`.
// Fails with error set when the file does not open, the work is cancelled, or the recording is silent.
bool MeasureLoudness(const std::filesystem::path& path, const std::atomic<bool>& cancel, std::atomic<float>& progress,
                     float from, float to, LoudnessResult& result, std::string& error);

// The ReplayGain 2 track gain that brings a recording of lufs to the -18 LUFS reference, in dB.
double ReplayGainFor(double lufs);

// "REPLAYGAIN_TRACK_GAIN=-12.34 dB" and "REPLAYGAIN_TRACK_PEAK=0.987654", formatted without the locale.
std::string ReplayGainComment(double gain_db);
std::string ReplayPeakComment(float peak);

}  // namespace gdx_companion
