// Copyright (c) Zorkats
//
// Plays a recording through SDL audio with the game's own decoders, repeating the loop the page chose,
// so the user hears the seam before installing. Fed from the UI thread: Pump keeps about a quarter of a
// second queued, so no audio callback thread touches the decoder. Part of the executable only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gdx_companion_state.h"

struct GdxMusicDecoder;

namespace gdx_companion {

class AudioPreview {
public:
    AudioPreview() = default;
    AudioPreview(const AudioPreview&) = delete;
    AudioPreview& operator=(const AudioPreview&) = delete;
    ~AudioPreview();

    // Applies a page request, then tops up the queue. Writes status every call.
    void Handle(const PreviewRequest& request, PreviewStatus& status);
    void Pump(PreviewStatus& status);
    void Stop();

private:
    bool Open(const PreviewRequest& request, std::string& error);

    GdxMusicDecoder* decoder_ = nullptr;
    uint32_t device_ = 0;
    uint32_t rate_ = 0;
    uint32_t channels_ = 0;
    uint64_t frames_ = 0;
    uint64_t loop_start_ = 0;
    uint64_t loop_end_ = 0;
    uint64_t read_position_ = 0;  // the next frame the decoder returns
    uint64_t queued_frames_ = 0;  // frames handed to SDL so far, for the position readout
    uint64_t start_frame_ = 0;
    std::string path_;
    std::vector<float> buffer_;
};

}  // namespace gdx_companion
