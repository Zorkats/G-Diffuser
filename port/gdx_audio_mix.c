// gdx_audio_mix.c -- per-sequence-player volume scale for the Music and Sound effects sliders.
//
// The decomp mixes every sequence player's channel volumes through one choke point,
// Audio_SequencePlayerProcessSound (decomp/src/audio/{disk,rom}/lib/effects.c). Under PORT that
// function multiplies the player's appliedFadeVolume by gdx_audio_seq_player_scale(player). Seq
// player 1 is the background music. Seq players 0 and 2 are sound effects, and the Expansion Kit
// title guitar runs on player 0, so it follows the Sound effects slider.
//
// THREADING. Every function here runs on the dedicated audio thread, because
// Audio_SequencePlayerProcessSound runs inside AudioSynth_Update on that thread. The menu only
// writes the two CVars and never touches a sequence player. The change check reads both CVars
// (the same one-int-per-read idiom as gdx_audio_lle.c), caches them, and reports whether one
// player's scale moved since it was last applied. Only then does effects.c set recalculateVolume,
// which is the flag the decomp already uses to recompute channel volumes on demand.
//
// At 100/100 every scale is exactly 1.0f, the change check never fires, and effects.c skips the
// multiply, so the default output is bit-identical to stock.

#include "gdx_audio_mix.h"

// Defined in libultraship (C ABI). Declared locally so this C TU does not pull the C++ header.
extern int CVarGetInteger(const char* name, int defaultValue);

#define GDX_AUDIO_MIX_PLAYERS 3
#define GDX_AUDIO_MIX_MUSIC_PLAYER 1

// Cached from the CVars by the change check. Written only on the audio thread.
static float sMusicScale = 1.0f;
static float sSfxScale = 1.0f;

// The scale each player was last recalculated with. Starts at stock so an untouched 100/100
// config never reports a change.
static float sAppliedScale[GDX_AUDIO_MIX_PLAYERS] = { 1.0f, 1.0f, 1.0f };

// Set while a streamed music replacement is live (port/gdx_music_stream.cpp, written on its worker
// thread). It forces seq player 1 to scale 0, so the stock track is silent under the stream, and the
// stream applies the Music slider itself through gdx_audio_music_slider_scale(). volatile because the
// audio thread reads it without a lock; an aligned int cannot tear.
static volatile int sStreamMutesMusic = 0;

static float gdx_audio_mix_percent_to_scale(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return (float) percent / 100.0f;
}

static float gdx_audio_mix_scale_for_player(int playerIndex) {
    if (playerIndex == GDX_AUDIO_MIX_MUSIC_PLAYER) {
        return sStreamMutesMusic ? 0.0f : sMusicScale;
    }
    return sSfxScale;
}

static int gdx_audio_mix_player_in_range(int playerIndex) {
    return playerIndex >= 0 && playerIndex < GDX_AUDIO_MIX_PLAYERS;
}

float gdx_audio_seq_player_scale(int playerIndex) {
    if (!gdx_audio_mix_player_in_range(playerIndex)) {
        return 1.0f;
    }
    return gdx_audio_mix_scale_for_player(playerIndex);
}

int gdx_audio_seq_player_scale_changed(int playerIndex) {
    float scale;

    if (!gdx_audio_mix_player_in_range(playerIndex)) {
        return 0;
    }

    sMusicScale = gdx_audio_mix_percent_to_scale(CVarGetInteger("gEnhancements.Audio.MusicVolume", 100));
    sSfxScale = gdx_audio_mix_percent_to_scale(CVarGetInteger("gEnhancements.Audio.SfxVolume", 100));

    scale = gdx_audio_mix_scale_for_player(playerIndex);
    if (scale == sAppliedScale[playerIndex]) {
        return 0;
    }
    sAppliedScale[playerIndex] = scale;
    return 1;
}

// Called from the worker thread when a streamed track starts (1) or ends (0). The next tick's change
// check on the audio thread sees the new scale for seq player 1 and recalculates its channels.
void gdx_audio_seq_player_stream_mute(int active) {
    sStreamMutesMusic = active ? 1 : 0;
}

// The Music slider as a 0..1 scale, before any stream mute. The streamed track multiplies by this.
float gdx_audio_music_slider_scale(void) {
    return sMusicScale;
}
