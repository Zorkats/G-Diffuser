// port/gdx_audio_soundfont_packs.h — Audio modding stage 4: whole-soundfont overlays.
//
// A mounted mods/*.o2r pack can carry a full replacement instrument graph for one soundfont at
// "audio/font/<FONTNAME>" (FONTNAME = FontId enum identifier verbatim from decomp/include/sfx.h,
// EXPANSION_KIT branch). Each entry is a "GFT1" v1 container (frozen layout at the top of the
// .cpp). When the master switch is on, the hook at the end of gdx_audio_convert_font in
// decomp/src/audio/disk/lib/load.c swaps the freshly converted font's instruments/drums/sfx
// pointers for arena-built overlay graphs. Counts always equal the stock font, so every
// bounds-checked reader (Audio_GetInstrument/Drum/SoundEffect) keeps working unchanged.
//
// All 23 EK fonts are CACHEPOLICY_0 (converted once, never re-converted), so "Reload packs" is
// serviced by GdxSoundfontPackTick: on a workshop epoch change it re-applies present overlays or
// restores the stashed stock pointers. Old overlay graphs are never freed, so notes already in
// flight keep their (old) tunedSample pointers until they end.

#ifndef GDX_AUDIO_SOUNDFONT_PACKS_H
#define GDX_AUDIO_SOUNDFONT_PACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when gEnhancements.Workshop.SoundfontPacks is on, 0 otherwise.
int gdx_soundfont_packs_enabled(void);

// Applies the mounted-pack overlay for fontId to the just-converted decomp SoundFont at
// `soundFont` (opaque; the .cpp mirrors the layout). Any validation failure leaves the font
// untouched (all-stock or all-overlay, never partial).
void GdxSoundfontPackApply(int32_t fontId, void* soundFont);

// Workshop reload servicing. Compares the pack epoch against the last-seen one; on change, every
// font seen so far is re-applied (overlay present and switch on) or restored to its stock
// pointers. Audio thread only (hooked at AudioLoad_ProcessLoads).
void GdxSoundfontPackTick(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_AUDIO_SOUNDFONT_PACKS_H
