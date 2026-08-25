// port/gdx_audio_seq_packs.h — Audio modding stage 2: sequence replacement overlays.
//
// A mounted mods/*.o2r pack can carry raw native sequence (aseq) bytes at "audio/seq/<name>"
// (one entry per SeqId, names below in the .cpp). When the master switch is on, the sequence
// load hooks in decomp/src/audio/{disk,rom}/lib/load.c serve those bytes instead of DMA-ing the
// stock sequence. Overrides apply on the next song/scene load; sequences already resident on the
// permanent heap are not retro-swapped.

#ifndef GDX_AUDIO_SEQ_PACKS_H
#define GDX_AUDIO_SEQ_PACKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when gEnhancements.Workshop.SequencePacks is on, 0 otherwise.
int gdx_seq_packs_enabled(void);

// Copies the mounted pack override for seqId into dst. Returns the override byte count, or 0
// when no pack provides the sequence, the master switch context is unavailable, or the override
// does not fit the stock slot (dstCapacity) — 0 always means "fall back to the stock load".
int GdxSeqPackResolve(int32_t seqId, void* dst, size_t dstCapacity);

// Returns the FontId (0..FONT_MAX-1) declared by an optional companion file
// "audio/seq/<name>.font" in the mounted pack, or -1 if none is present. The file content is a
// plain-text font reference: either the short SeqId/FontId name (e.g. "ddbgm_mute_city") or the
// enum identifier ("FONT_DDBGM_MUTE_CITY"). When a valid companion is present, the loader uses
// that font for the sequence instead of the stock slot's default, so cross-bank sequence swaps
// can bring their original bank with them without requiring a full soundfont pack.
int GdxSeqPackGetFont(int32_t seqId);

#ifdef __cplusplus
}
#endif

#endif // GDX_AUDIO_SEQ_PACKS_H
