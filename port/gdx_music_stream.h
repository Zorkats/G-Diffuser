#ifndef GDX_MUSIC_STREAM_H
#define GDX_MUSIC_STREAM_H

// Streamed BGM replacement (plan section 6A). When a BGM id starts and a file exists for its key
// (music/<bgm key>.ogg|.flac|.wav beside the exe, or mods/music/...), the file is decoded on a worker
// thread into a ring of stereo float frames. libultraship's osAiSetNextBuffer mixes that ring into the
// output buffer before the low-pass and master volume. Stock seq player 1 is muted while a stream is
// live (gdx_audio_seq_player_stream_mute), so the stream is the only music.
//
// The stream gain is the Music slider times the raw seq player 1 fade (fadeVolume x fadeVolumeScale),
// so the stock fade-out on Audio_DDBgmStop and the pause ducking carry over. Implemented in
// port/gdx_music_stream.cpp; the decoders are in port/third_party/music_decoders.
//
// Threading: gdx_music_stream_bgm_start, gdx_music_stream_bgm_stop and gdx_music_stream_play_file run on
// the game thread. gdx_music_stream_mix_s16 and gdx_music_stream_is_mixing run on the audio thread. The
// decoder runs on a worker thread that owns the file handle.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called from decomp Audio_DDBgmStart (and its deferred start) with the BGM id. Looks up the key and a
// file, logs one line, and starts a stream when a file exists and gEnhancements.Audio.StreamedMusic is on.
void gdx_music_stream_bgm_start(int bgmId);

// Called from decomp Audio_DDBgmStop. A live stream follows the stock fade-out to silence and then closes.
void gdx_music_stream_bgm_stop(void);

// Plays an arbitrary file with no BGM key lookup. This is the entry point for the 7A custom soundtrack
// feature. Returns 1 when the request was accepted. label is used only for logging and may be NULL.
int gdx_music_stream_play_file(const char* path, const char* label);

// Course override (7A): while set, gdx_music_stream_bgm_start with this bgmId plays path instead of the
// key lookup, if gEnhancements.Audio.StreamedMusic is on. The stock start still runs; only the music is
// replaced. The override is cleared by gdx_music_stream_bgm_stop (Audio_DDBgmStop and the preview path).
// A NULL or empty path clears it. label may be NULL. Game thread only.
void gdx_music_stream_set_course_override(int bgmId, const char* path, const char* label);

// Audio thread: nonzero while a stream is in the Playing or Draining state.
int gdx_music_stream_is_mixing(void);

// Audio thread: mixes the live stream into frames interleaved stereo s16 samples at deviceRate, in place.
// Returns 1 when samples were mixed, 0 when no stream is live.
int gdx_music_stream_mix_s16(int16_t* stereo, size_t frames, uint32_t deviceRate);

// Writes the BGM key for an id (for example "bgm_title"). Returns 0 for BGM_NONE, which never streams.
int gdx_music_stream_key_for_bgm(int bgmId, char* out, size_t outCap);

// Finds the file for a BGM id, searching mods/music then music beside the exe. Returns 1 and writes the
// path when one exists.
int gdx_music_stream_find_file(int bgmId, char* outPath, size_t outCap);

// Loop wrap for a source frame position. Positions at or past loopEnd map to loopStart + offset.
uint64_t gdx_music_loop_wrap(uint64_t pos, uint64_t loopStart, uint64_t loopEnd);

#ifdef __cplusplus
}
#endif

#endif // GDX_MUSIC_STREAM_H
