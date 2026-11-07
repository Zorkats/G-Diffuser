// Copyright (c) Zorkats

#include "gdx_companion_loop.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <utility>

#include "decoders.h"
#include "gdx_companion_loudness.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kReadFrames = 8192;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kBands = 16;
constexpr uint64_t kMaxBlocks = 8000;
constexpr double kWindowSeconds = 4.0;
constexpr double kMinLoopSeconds = 5.0;
constexpr double kMinRecordingSeconds = 12.0;
constexpr double kLengthSeparationSeconds = 2.0;
constexpr float kQuietBelowPeakDb = 30.0f;

// Sums for the normalized correlation of pairs (x[a + i], x[b + i]).
struct Correlation {
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    uint64_t pairs = 0;

    void Add(const std::vector<float>& x, uint64_t a, uint64_t b, uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) {
            const double va = x[static_cast<size_t>(a + i)];
            const double vb = x[static_cast<size_t>(b + i)];
            ab += va * vb;
            aa += va * va;
            bb += vb * vb;
        }
        pairs += n;
    }

    // Two silent stretches join seamlessly; silence against sound does not.
    double Value() const {
        constexpr double kSilent = 1e-9;
        const bool silent_a = aa <= kSilent * static_cast<double>(pairs);
        const bool silent_b = bb <= kSilent * static_cast<double>(pairs);
        if (silent_a || silent_b) return silent_a && silent_b ? 1.0 : 0.0;
        return ab / std::sqrt(aa * bb);
    }
};

// About 40 ms: long enough to hear a skip, short enough that the music around it does not dominate.
uint64_t SeamWindow(uint32_t rate) { return std::max<uint64_t>(256, rate / 24); }

// The correlation of the window before end with the window before start, and of the window after end
// (what the file holds next) with the window after start (what the loop plays next).
Correlation SeamCorrelation(const LoopAudio& audio, uint64_t start, uint64_t end, uint64_t window) {
    Correlation sums;
    const uint64_t before = std::min(window, start);
    if (before > 0) sums.Add(audio.mono, end - before, start - before, before);
    const uint64_t after = std::min(window, audio.frames - end);
    if (after > 0) sums.Add(audio.mono, end, start, after);
    return sums;
}

void Fft(std::vector<std::complex<float>>& data) {
    const size_t n = data.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const std::complex<float> step(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        for (size_t i = 0; i < n; i += length) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < length / 2; ++k) {
                const std::complex<float> even = data[i + k];
                const std::complex<float> odd = data[i + k + length / 2] * w;
                data[i + k] = even + odd;
                data[i + k + length / 2] = even - odd;
                w *= step;
            }
        }
    }
}

// The best loop length in frames within radius of guess for a loop ending at end, by the correlation of
// the stretch before end with the stretch one length earlier.
uint64_t RefineLength(const LoopAudio& audio, uint64_t end, uint64_t guess, uint64_t radius) {
    const uint64_t low = guess > radius ? guess - radius : 1;
    const uint64_t high = std::min(guess + radius, end);
    auto score = [&](uint64_t length) {
        const uint64_t window = std::min<uint64_t>(16384, end - length);
        if (window == 0) return -2.0;
        Correlation sums;
        sums.Add(audio.mono, end - window, end - length - window, window);
        return sums.Value();
    };
    uint64_t best = guess;
    double best_score = score(guess);
    for (uint64_t length = low; length <= high; length += 4) {
        const double value = score(length);
        if (value > best_score) {
            best_score = value;
            best = length;
        }
    }
    const uint64_t coarse = best;
    const uint64_t fine_low = coarse > low + 4 ? coarse - 4 : low;
    const uint64_t fine_high = std::min(coarse + 4, high);
    for (uint64_t length = fine_low; length <= fine_high; ++length) {
        const double value = score(length);
        if (value > best_score) {
            best_score = value;
            best = length;
        }
    }
    return best;
}

}  // namespace

void SetLoopAudio(LoopAudio& audio, uint32_t rate, std::vector<float> mono) {
    audio.rate = rate;
    audio.frames = mono.size();
    audio.mono = std::move(mono);
    const size_t blocks = static_cast<size_t>((audio.frames + kPeakBlockFrames - 1) / kPeakBlockFrames);
    audio.block_min.assign(blocks, 0.0f);
    audio.block_max.assign(blocks, 0.0f);
    for (size_t b = 0; b < blocks; ++b) {
        const size_t from = b * kPeakBlockFrames;
        const size_t to = std::min<size_t>(from + kPeakBlockFrames, audio.mono.size());
        const auto range = std::minmax_element(audio.mono.begin() + static_cast<std::ptrdiff_t>(from),
                                               audio.mono.begin() + static_cast<std::ptrdiff_t>(to));
        audio.block_min[b] = *range.first;
        audio.block_max[b] = *range.second;
    }
}

bool ReadLoopAudio(const fs::path& path, const CancelFlag& cancel, ProgressValue& progress, LoopAudio& audio,
                   std::string& error) {
    progress.store(0.0f);
    std::string narrow;
    if (!NarrowPathForC(path, narrow)) {
        error = "the decoder cannot open this path";
        return false;
    }
    GdxMusicInfo info{};
    GdxMusicDecoder* decoder = gdx_music_decoder_open(narrow.c_str(), &info);
    if (decoder == nullptr) {
        error = "the recording does not open";
        return false;
    }
    if (info.totalFrames > kMaxLoopAudioFrames) {
        gdx_music_decoder_close(decoder);
        error = "the recording is too long for the waveform";
        return false;
    }
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(info.totalFrames));
    std::vector<float> buffer(static_cast<size_t>(kReadFrames * info.channels));
    LoudnessMeter meter(info.sampleRate, info.channels);
    for (;;) {
        if (cancel.load()) {
            gdx_music_decoder_close(decoder);
            error = "cancelled";
            return false;
        }
        const uint64_t got = gdx_music_decoder_read(decoder, buffer.data(), kReadFrames);
        meter.Add(buffer.data(), static_cast<size_t>(got));
        for (uint64_t f = 0; f < got && mono.size() < kMaxLoopAudioFrames; ++f) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < info.channels; ++c) sum += buffer[static_cast<size_t>(f * info.channels + c)];
            mono.push_back(sum / static_cast<float>(info.channels));
        }
        if (info.totalFrames > 0) {
            progress.store(static_cast<float>(mono.size()) / static_cast<float>(info.totalFrames));
        }
        if (got < kReadFrames) break;
    }
    gdx_music_decoder_close(decoder);
    if (mono.empty()) {
        error = "the recording is empty";
        return false;
    }
    SetLoopAudio(audio, info.sampleRate, std::move(mono));
    audio.has_loudness = meter.Integrated(audio.lufs);
    audio.peak = meter.Peak();
    progress.store(1.0f);
    return true;
}

void PeakRange(const LoopAudio& audio, uint64_t from, uint64_t to, float& low, float& high) {
    to = std::min(to, audio.frames);
    low = 0.0f;
    high = 0.0f;
    if (from >= to) return;
    low = audio.mono[static_cast<size_t>(from)];
    high = low;
    auto raw = [&](uint64_t a, uint64_t b) {
        for (uint64_t i = a; i < b; ++i) {
            low = std::min(low, audio.mono[static_cast<size_t>(i)]);
            high = std::max(high, audio.mono[static_cast<size_t>(i)]);
        }
    };
    const uint64_t first_block = (from + kPeakBlockFrames - 1) / kPeakBlockFrames;
    const uint64_t last_block = to / kPeakBlockFrames;
    if (first_block >= last_block) {
        raw(from, to);
        return;
    }
    raw(from, first_block * kPeakBlockFrames);
    for (uint64_t b = first_block; b < last_block; ++b) {
        low = std::min(low, audio.block_min[static_cast<size_t>(b)]);
        high = std::max(high, audio.block_max[static_cast<size_t>(b)]);
    }
    raw(last_block * kPeakBlockFrames, to);
}

uint64_t NearestZeroCrossing(const LoopAudio& audio, uint64_t frame, uint64_t radius) {
    auto crosses = [&](uint64_t i) {
        if (i == 0 || i >= audio.frames) return false;
        return (audio.mono[static_cast<size_t>(i - 1)] < 0.0f) != (audio.mono[static_cast<size_t>(i)] < 0.0f);
    };
    for (uint64_t d = 0; d <= radius; ++d) {
        if (crosses(frame + d)) return frame + d;
        if (d <= frame && crosses(frame - d)) return frame - d;
    }
    return frame;
}

bool SeamMatch(const LoopAudio& audio, uint64_t start, uint64_t end, float& match) {
    match = 0.0f;
    if (audio.frames == 0 || start >= end || end > audio.frames) return false;
    const Correlation sums = SeamCorrelation(audio, start, end, SeamWindow(audio.rate));
    if (sums.pairs == 0) return false;
    match = static_cast<float>(sums.Value());
    return true;
}

uint64_t AlignLoopEnd(const LoopAudio& audio, uint64_t start, uint64_t end, uint64_t radius) {
    if (audio.frames == 0 || start >= end || end > audio.frames) return end;
    const uint64_t window = SeamWindow(audio.rate) * 2;
    const uint64_t low = std::max(start + 1, end > radius ? end - radius : 0);
    const uint64_t high = std::min(audio.frames, end + radius);
    uint64_t best = end;
    const Correlation current = SeamCorrelation(audio, start, end, window);
    double best_score = current.pairs > 0 ? current.Value() : -2.0;
    for (uint64_t candidate = low; candidate <= high; ++candidate) {
        const Correlation sums = SeamCorrelation(audio, start, candidate, window);
        if (sums.pairs == 0) continue;
        const double value = sums.Value();
        const uint64_t distance = candidate > end ? candidate - end : end - candidate;
        const uint64_t best_distance = best > end ? best - end : end - best;
        if (value > best_score + 1e-9 || (value >= best_score - 1e-9 && distance < best_distance)) {
            best_score = value;
            best = candidate;
        }
    }
    return best;
}

bool FindLoopPoints(const LoopAudio& audio, size_t count, const CancelFlag& cancel, ProgressValue& progress,
                    std::vector<LoopCandidate>& out, std::string& error) {
    out.clear();
    progress.store(0.0f);
    const uint32_t rate = audio.rate;
    const uint64_t frames = audio.frames;
    if (rate == 0 || static_cast<double>(frames) < kMinRecordingSeconds * rate) {
        error = "the recording is too short to search; it needs at least 12 seconds";
        return false;
    }

    // Spectral features: 16 log-spaced band energies in dB per hop.
    uint64_t hop = rate >= 32000 ? 2048 : 1024;
    while (frames / hop > kMaxBlocks) hop *= 2;
    const uint64_t size = std::min<uint64_t>(hop * 2, 8192);
    const size_t blocks = static_cast<size_t>((frames - size) / hop + 1);
    std::vector<size_t> edges(kBands + 1);
    const double low_hz = 80.0;
    const double high_hz = std::min(11000.0, rate * 0.45);
    for (size_t k = 0; k <= kBands; ++k) {
        const double hz = low_hz * std::pow(high_hz / low_hz, static_cast<double>(k) / kBands);
        size_t bin = static_cast<size_t>(std::lround(hz * static_cast<double>(size) / rate));
        if (k > 0) bin = std::max(bin, edges[k - 1] + 1);
        edges[k] = std::min<size_t>(bin, static_cast<size_t>(size / 2));
    }
    std::vector<float> hann(static_cast<size_t>(size));
    for (size_t i = 0; i < hann.size(); ++i) {
        hann[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (hann.size() - 1)));
    }
    std::vector<float> features(blocks * kBands);
    std::vector<float> loudness(blocks);
    std::vector<std::complex<float>> spectrum(static_cast<size_t>(size));
    for (size_t b = 0; b < blocks; ++b) {
        if ((b & 63) == 0) {
            if (cancel.load()) {
                error = "cancelled";
                return false;
            }
            progress.store(0.3f * static_cast<float>(b) / static_cast<float>(blocks));
        }
        const size_t from = static_cast<size_t>(b * hop);
        for (size_t i = 0; i < spectrum.size(); ++i) spectrum[i] = {audio.mono[from + i] * hann[i], 0.0f};
        Fft(spectrum);
        double total = 0.0;
        for (size_t k = 0; k < kBands; ++k) {
            double energy = 0.0;
            for (size_t bin = edges[k]; bin < edges[k + 1]; ++bin) energy += std::norm(spectrum[bin]);
            total += energy;
            features[b * kBands + k] = static_cast<float>(10.0 * std::log10(energy + 1e-10));
        }
        loudness[b] = static_cast<float>(10.0 * std::log10(total + 1e-10));
    }

    const double hop_seconds = static_cast<double>(hop) / rate;
    const size_t window = std::max<size_t>(8, static_cast<size_t>(std::lround(kWindowSeconds / hop_seconds)));
    const size_t min_length = std::max(window, static_cast<size_t>(std::lround(kMinLoopSeconds / hop_seconds)));
    if (min_length + window >= blocks) {
        error = "the recording is too short to search; it needs at least 12 seconds";
        return false;
    }
    std::vector<double> loud_sum(blocks + 1, 0.0);
    float peak = loudness[0];
    for (size_t b = 0; b < blocks; ++b) {
        loud_sum[b + 1] = loud_sum[b] + loudness[b];
        peak = std::max(peak, loudness[b]);
    }
    const double quiet = static_cast<double>(peak - kQuietBelowPeakDb);
    auto distance = [&](size_t a, size_t b) {
        float sum = 0.0f;
        const float* fa = &features[a * kBands];
        const float* fb = &features[b * kBands];
        for (size_t k = 0; k < kBands; ++k) sum += std::fabs(fa[k] - fb[k]);
        return sum / static_cast<float>(kBands);
    };

    // For each loop length in blocks, the end block whose preceding window matches best.
    struct Best {
        float score;
        size_t end_block;
    };
    std::vector<Best> best(blocks, Best{1e30f, 0});
    std::vector<float> pair(blocks);
    for (size_t length = min_length; length + window <= blocks; ++length) {
        if (((length - min_length) & 31) == 0) {
            if (cancel.load()) {
                error = "cancelled";
                return false;
            }
            progress.store(0.3f + 0.6f * static_cast<float>(length - min_length) /
                                      static_cast<float>(blocks - window - min_length + 1));
        }
        double sum = 0.0;
        for (size_t t = length; t < blocks; ++t) {
            pair[t] = distance(t, t - length);
            sum += pair[t];
            if (t >= length + window) sum -= pair[t - window];
            if (t + 1 < length + window) continue;
            const size_t first = t + 1 - window;
            const double here = (loud_sum[t + 1] - loud_sum[first]) / static_cast<double>(window);
            const double there = (loud_sum[t + 1 - length] - loud_sum[first - length]) / static_cast<double>(window);
            if (here < quiet || there < quiet) continue;
            const float score = static_cast<float>(sum / static_cast<double>(window));
            if (score < best[length].score) best[length] = Best{score, t};
        }
    }

    std::vector<size_t> lengths;
    for (size_t length = min_length; length < blocks; ++length) {
        if (best[length].score < 1e29f) lengths.push_back(length);
    }
    std::sort(lengths.begin(), lengths.end(), [&](size_t a, size_t b) { return best[a].score < best[b].score; });
    const size_t separation = static_cast<size_t>(std::lround(kLengthSeparationSeconds / hop_seconds));
    std::vector<size_t> picked;
    for (const size_t length : lengths) {
        if (picked.size() == count) break;
        bool near = false;
        for (const size_t other : picked) {
            near = near || (length > other ? length - other : other - length) < separation;
        }
        if (!near) picked.push_back(length);
    }
    if (picked.empty()) {
        error = "no loop was found; the recording may be too quiet or never repeat";
        return false;
    }

    // Line each loop up to the frame and put its end on a zero crossing, keeping its length.
    for (size_t i = 0; i < picked.size(); ++i) {
        if (cancel.load()) {
            error = "cancelled";
            return false;
        }
        progress.store(0.9f + 0.1f * static_cast<float>(i) / static_cast<float>(picked.size()));
        const size_t length_blocks = picked[i];
        const uint64_t end_guess = std::min<uint64_t>(frames, best[length_blocks].end_block * hop + size / 2);
        const uint64_t length = RefineLength(audio, end_guess, length_blocks * hop, hop);
        uint64_t end = NearestZeroCrossing(audio, end_guess, std::max<uint64_t>(1, rate / 1000));
        if (end < length || end > frames) end = end_guess;
        LoopCandidate candidate;
        candidate.start = end - length;
        candidate.end = end;
        candidate.distance = best[length_blocks].score;
        SeamMatch(audio, candidate.start, candidate.end, candidate.match);
        out.push_back(candidate);
    }
    progress.store(1.0f);
    return true;
}

std::string FormatLoopTime(uint64_t frame, uint32_t rate) {
    if (rate == 0) return "0:00.000";
    const uint64_t millis = frame * 1000 / rate;
    char text[48];
    std::snprintf(text, sizeof(text), "%llu:%02llu.%03llu", static_cast<unsigned long long>(millis / 60000),
                  static_cast<unsigned long long>(millis / 1000 % 60), static_cast<unsigned long long>(millis % 1000));
    return text;
}

bool ParseLoopTime(const std::string& text, uint32_t rate, uint64_t& frame) {
    if (rate == 0) return false;
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (begin == end) return false;
    std::vector<std::string> parts(1);
    for (size_t i = begin; i < end; ++i) {
        if (text[i] == ':') {
            parts.emplace_back();
        } else {
            parts.back().push_back(text[i]);
        }
    }
    if (parts.size() > 3) return false;
    double seconds = 0.0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& part = parts[i];
        const bool last = i + 1 == parts.size();
        if (part.empty()) return false;
        size_t dots = 0;
        for (const char c : part) {
            if (c == '.') {
                ++dots;
            } else if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        if (dots > (last ? 1u : 0u) || part == ".") return false;
        const double value = std::strtod(part.c_str(), nullptr);
        if (i > 0 && value >= 60.0) return false;
        seconds = seconds * 60.0 + value;
    }
    if (seconds > 1e7) return false;
    frame = static_cast<uint64_t>(std::llround(seconds * rate));
    return true;
}

}  // namespace gdx_companion
