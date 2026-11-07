// Copyright (c) Zorkats

#include "gdx_companion_preview.h"

#include <algorithm>

#include <SDL.h>

#include "decoders.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace {

constexpr uint64_t kChunkFrames = 2048;

}  // namespace

AudioPreview::~AudioPreview() { Stop(); }

void AudioPreview::Stop() {
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
    if (decoder_ != nullptr) {
        gdx_music_decoder_close(decoder_);
        decoder_ = nullptr;
    }
    path_.clear();
    queued_frames_ = 0;
}

bool AudioPreview::Open(const PreviewRequest& request, std::string& error) {
    Stop();
    std::string narrow;
    if (!NarrowPathForC(PathFromUtf8(request.path), narrow)) {
        error = "the decoder cannot open this path";
        return false;
    }
    GdxMusicInfo info{};
    decoder_ = gdx_music_decoder_open(narrow.c_str(), &info);
    if (decoder_ == nullptr) {
        error = "the recording does not open";
        return false;
    }
    rate_ = info.sampleRate;
    channels_ = info.channels;
    frames_ = info.totalFrames;
    loop_end_ = request.loop_end > 0 && request.loop_end <= frames_ ? request.loop_end : frames_;
    loop_start_ = request.loop_start < loop_end_ ? request.loop_start : 0;
    start_frame_ = std::min(request.start_frame, loop_end_ > 0 ? loop_end_ - 1 : 0);
    if (gdx_music_decoder_seek(decoder_, start_frame_) == 0) {
        error = "the recording cannot seek";
        Stop();
        return false;
    }
    read_position_ = start_frame_;

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(rate_);
    want.format = AUDIO_F32SYS;
    want.channels = static_cast<Uint8>(channels_);
    want.samples = 1024;
    want.callback = nullptr;
    SDL_AudioSpec have{};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device_ == 0) {
        error = std::string("no audio device: ") + SDL_GetError();
        Stop();
        return false;
    }
    path_ = request.path;
    buffer_.assign(static_cast<size_t>(kChunkFrames * channels_), 0.0f);
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void AudioPreview::Handle(const PreviewRequest& request, PreviewStatus& status) {
    if (request.stop || request.path.empty()) {
        Stop();
        status.error.clear();
    } else {
        std::string error;
        if (Open(request, error)) {
            status.error.clear();
        } else {
            status.error = error;
        }
    }
    Pump(status);
}

void AudioPreview::Pump(PreviewStatus& status) {
    status.playing = device_ != 0;
    status.path = path_;
    status.rate = rate_;
    if (device_ == 0 || decoder_ == nullptr) {
        status.position = 0;
        return;
    }
    const uint64_t frame_bytes = static_cast<uint64_t>(channels_) * sizeof(float);
    const uint64_t target = rate_ / 4;  // a quarter of a second
    for (int guard = 0; guard < 16; ++guard) {
        const uint64_t queued = SDL_GetQueuedAudioSize(device_) / frame_bytes;
        if (queued >= target) break;
        const uint64_t want = std::min<uint64_t>(kChunkFrames, loop_end_ - read_position_);
        uint64_t got = want == 0 ? 0 : gdx_music_decoder_read(decoder_, buffer_.data(), want);
        if (got > 0) {
            SDL_QueueAudio(device_, buffer_.data(), static_cast<Uint32>(got * frame_bytes));
            read_position_ += got;
            queued_frames_ += got;
        }
        if (read_position_ >= loop_end_ || got < want) {
            if (gdx_music_decoder_seek(decoder_, loop_start_) == 0) {
                status.error = "the recording cannot seek back to the loop start";
                Stop();
                status.playing = false;
                return;
            }
            read_position_ = loop_start_;
        }
    }
    // The position heard is the frames sent minus the frames still queued, folded into the loop.
    const uint64_t queued = SDL_GetQueuedAudioSize(device_) / frame_bytes;
    const uint64_t played = queued_frames_ > queued ? queued_frames_ - queued : 0;
    uint64_t position = start_frame_ + played;
    if (position >= loop_end_ && loop_end_ > loop_start_) {
        position = loop_start_ + (position - loop_end_) % (loop_end_ - loop_start_);
    }
    status.position = position;
}

}  // namespace gdx_companion
