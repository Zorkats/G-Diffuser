#ifndef GDX_MUSIC_DECODERS_H
#define GDX_MUSIC_DECODERS_H

// Ogg Vorbis, FLAC and WAV decoding for the streamed music replacement (port/gdx_music_stream.cpp).
// The implementations are vendored in port/third_party/stb_vorbis and port/third_party/dr_libs and
// compiled once, in decoders.c, with warnings suppressed. Callers see only this C ABI.
//
// Every decoder yields interleaved float frames with 1 or 2 channels. Loop points are in source
// frames. loopEnd is exclusive. When a file carries no usable loop tags the loop is the whole file.
//
// Level: Ogg Vorbis and FLAC carry REPLAYGAIN_TRACK_GAIN ("-12.34 dB") and REPLAYGAIN_TRACK_PEAK
// ("0.987654") comments; a WAV carries a version 2 bext chunk with its integrated loudness and peak.
// gdx_music_level_gain turns them into the gain that plays the recording at the stock music level.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxMusicDecoder GdxMusicDecoder;

typedef struct GdxMusicInfo {
    uint32_t sampleRate;
    uint32_t channels;   // 1 or 2
    uint64_t totalFrames;
    uint64_t loopStart;  // first frame of the loop, inclusive
    uint64_t loopEnd;    // one past the last frame of the loop
    int hasLoopTags;     // 1 when LOOPSTART/LOOPLENGTH or RIFF smpl supplied the loop
    int hasGain;         // 1 when gainDb came from the file
    float gainDb;        // ReplayGain 2 track gain: the change that brings the recording to -18 LUFS
    int hasPeak;         // 1 when peak came from the file
    float peak;          // the largest sample, 1.0 at full scale
} GdxMusicInfo;

// The ReplayGain 2 reference level, and the integrated loudness of the stock music (the title screen
// BGM captured at the audio interface tap measures -17.1 LUFS).
#define GDX_MUSIC_REPLAYGAIN_REFERENCE_LUFS (-18.0f)
#define GDX_MUSIC_STOCK_LUFS (-17.0f)

// The linear gain that plays info's recording at the stock music level: 1.0 without a gain. Never
// raises the peak past full scale; without a known peak it only lowers.
float gdx_music_level_gain(const GdxMusicInfo* info);

// Opens path by extension (.ogg, .flac, .wav, case-insensitive) and fills info. Returns NULL when
// the extension is unknown, the file cannot be opened, or the channel count is not 1 or 2.
GdxMusicDecoder* gdx_music_decoder_open(const char* path, GdxMusicInfo* info);

// Reads up to frames frames into out (frames * channels floats). Returns the number of frames read,
// which is less than requested only at end of file or on a decode error.
uint64_t gdx_music_decoder_read(GdxMusicDecoder* dec, float* out, uint64_t frames);

// Repositions the decoder to an absolute source frame. Returns 1 on success, 0 on failure.
int gdx_music_decoder_seek(GdxMusicDecoder* dec, uint64_t frame);

void gdx_music_decoder_close(GdxMusicDecoder* dec);

#ifdef __cplusplus
}
#endif

#endif // GDX_MUSIC_DECODERS_H
