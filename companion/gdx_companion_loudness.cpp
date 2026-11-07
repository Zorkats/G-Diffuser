// Copyright (c) Zorkats

#include "gdx_companion_loudness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "decoders.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAbsoluteGate = -70.0;  // LUFS
constexpr double kRelativeGate = -10.0;  // LU below the absolute-gated level
constexpr uint64_t kChunkFrames = 4096;

double LoudnessOf(double energy) { return -0.691 + 10.0 * std::log10(energy); }

// round(value * scale) as text with `places` decimals, without the C locale.
std::string FixedDecimal(double value, int places) {
    long long scale = 1;
    for (int i = 0; i < places; ++i) scale *= 10;
    const long long scaled = std::llround(value * static_cast<double>(scale));
    const unsigned long long magnitude =
        scaled < 0 ? static_cast<unsigned long long>(-scaled) : static_cast<unsigned long long>(scaled);
    char text[48];
    std::snprintf(text, sizeof(text), "%s%llu.%0*llu", scaled < 0 ? "-" : "", magnitude / scale, places,
                  magnitude % static_cast<unsigned long long>(scale));
    return text;
}

}  // namespace

LoudnessMeter::LoudnessMeter(uint32_t rate, uint32_t channels)
    : channels_(channels == 1 ? 1u : 2u), weight_(channels == 1 ? 2.0 : 1.0) {
    const double fs = rate > 0 ? static_cast<double>(rate) : 48000.0;
    quarter_ = std::max<size_t>(1, static_cast<size_t>(std::lround(fs / 10.0)));

    // The BS.1770 filters at any sample rate, from their analog prototypes as libebur128 derives them.
    {
        const double f0 = 1681.974450955533;
        const double gain = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / fs);
        const double vh = std::pow(10.0, gain / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf_.b0 = (vh + vb * k / q + k * k) / a0;
        shelf_.b1 = 2.0 * (k * k - vh) / a0;
        shelf_.b2 = (vh - vb * k / q + k * k) / a0;
        shelf_.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf_.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        highpass_.b0 = 1.0;
        highpass_.b1 = -2.0;
        highpass_.b2 = 1.0;
        highpass_.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass_.a2 = (1.0 - k / q + k * k) / a0;
    }
}

double LoudnessMeter::Run(const Biquad& f, State& s, double x) {
    const double y = f.b0 * x + f.b1 * s.x1 + f.b2 * s.x2 - f.a1 * s.y1 - f.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    s.y1 = y;
    return y;
}

void LoudnessMeter::Add(const float* interleaved, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < channels_; ++c) {
            const float sample = interleaved[i * channels_ + c];
            peak_ = std::max(peak_, std::fabs(sample));
            const double y = Run(highpass_, state_[c][1], Run(shelf_, state_[c][0], sample));
            energy_ += weight_ * y * y;
        }
        if (++filled_ == quarter_) {
            quarters_.push_back(energy_);
            energy_ = 0;
            filled_ = 0;
        }
    }
}

bool LoudnessMeter::Integrated(double& lufs) const {
    // Each block is four consecutive 100 ms quarters: 400 ms with 75% overlap.
    std::vector<double> blocks;
    if (quarters_.size() >= 4) {
        blocks.reserve(quarters_.size() - 3);
        const double frames = 4.0 * static_cast<double>(quarter_);
        for (size_t i = 0; i + 4 <= quarters_.size(); ++i) {
            const double energy = (quarters_[i] + quarters_[i + 1] + quarters_[i + 2] + quarters_[i + 3]) / frames;
            if (energy > 0.0 && LoudnessOf(energy) > kAbsoluteGate) blocks.push_back(energy);
        }
    }
    if (blocks.empty()) return false;
    double sum = 0;
    for (const double energy : blocks) sum += energy;
    const double threshold = LoudnessOf(sum / static_cast<double>(blocks.size())) + kRelativeGate;
    double gated = 0;
    size_t count = 0;
    for (const double energy : blocks) {
        if (LoudnessOf(energy) > threshold) {
            gated += energy;
            ++count;
        }
    }
    if (count == 0) return false;
    lufs = LoudnessOf(gated / static_cast<double>(count));
    return true;
}

bool MeasureLoudness(const std::filesystem::path& path, const std::atomic<bool>& cancel, std::atomic<float>& progress,
                     float from, float to, LoudnessResult& result, std::string& error) {
    std::string narrow;
    if (!NarrowPathForC(path, narrow)) {
        error = "the decoder cannot open this path";
        return false;
    }
    GdxMusicInfo info{};
    GdxMusicDecoder* decoder = gdx_music_decoder_open(narrow.c_str(), &info);
    if (decoder == nullptr) {
        error = "the game's decoder cannot open this file";
        return false;
    }
    LoudnessMeter meter(info.sampleRate, info.channels);
    const uint32_t channels = info.channels == 1 ? 1u : 2u;
    std::vector<float> buffer(static_cast<size_t>(kChunkFrames * channels));
    uint64_t total = 0;
    for (;;) {
        if (cancel.load()) {
            gdx_music_decoder_close(decoder);
            error = "cancelled";
            return false;
        }
        const uint64_t got = gdx_music_decoder_read(decoder, buffer.data(), kChunkFrames);
        meter.Add(buffer.data(), static_cast<size_t>(got));
        total += got;
        if (info.totalFrames > 0) {
            progress.store(from + (to - from) * static_cast<float>(std::min<uint64_t>(total, info.totalFrames)) /
                                      static_cast<float>(info.totalFrames));
        }
        if (got < kChunkFrames) break;
    }
    gdx_music_decoder_close(decoder);
    if (!meter.Integrated(result.lufs)) {
        error = "the recording is silent, or shorter than 400 ms, so its loudness cannot be measured";
        return false;
    }
    result.peak = meter.Peak();
    return true;
}

double ReplayGainFor(double lufs) { return static_cast<double>(GDX_MUSIC_REPLAYGAIN_REFERENCE_LUFS) - lufs; }

std::string ReplayGainComment(double gain_db) {
    return "REPLAYGAIN_TRACK_GAIN=" + FixedDecimal(gain_db, 2) + " dB";
}

std::string ReplayPeakComment(float peak) { return "REPLAYGAIN_TRACK_PEAK=" + FixedDecimal(peak, 6); }

}  // namespace gdx_companion
