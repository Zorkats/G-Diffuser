#ifndef GDX_AUDIO_MIX_H
#define GDX_AUDIO_MIX_H

// Per-sequence-player volume scale behind the Music and Sound effects sliders
// (gEnhancements.Audio.MusicVolume and gEnhancements.Audio.SfxVolume, 0-100 percent, default 100).
// Implemented in port/gdx_audio_mix.c. Called only from decomp/src/audio/{disk,rom}/lib/effects.c,
// on the dedicated audio thread (port/gdx_audio_thread.cpp).
//
// Seq player 1 carries the background music. Seq players 0 and 2 carry sound effects.
//
// gdx_audio_seq_player_scale() returns the cached scale for a player. At 100 it is exactly 1.0f,
// so the caller skips its multiply and the default output is bit-identical to stock.
//
// gdx_audio_seq_player_scale_changed() re-reads both CVars and returns 1 when that player's scale
// differs from the value it was last applied with. The caller then sets the player's
// recalculateVolume flag so the new scale takes effect on this tick.
//
// gdx_audio_seq_player_stream_mute() forces seq player 1 to scale 0 while a streamed music
// replacement is live (port/gdx_music_stream.cpp). gdx_audio_music_slider_scale() returns the Music
// slider as 0..1 without that mute, which is the gain the stream applies.

#ifdef __cplusplus
extern "C" {
#endif

float gdx_audio_seq_player_scale(int playerIndex);
int gdx_audio_seq_player_scale_changed(int playerIndex);
void gdx_audio_seq_player_stream_mute(int active);
float gdx_audio_music_slider_scale(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_AUDIO_MIX_H
