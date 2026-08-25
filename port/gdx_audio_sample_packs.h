// port/gdx_audio_sample_packs.h — Audio modding stage 3: per-sample replacement overlays.
//
// A mounted mods/*.o2r pack can carry ADPCM sample replacements at
// "audio/sample/<bankName>__0x<bankOffset>__<stockSize>" (bank names below in the .cpp; the key
// shape matches torch's dumper, src/gdx/dump_audio.cpp sampleKey). Each entry is a "GSMP" v1
// container (frozen layout at the top of the .cpp). When the master switch is on, the hook in
// decomp/src/audio/disk/lib/load.c (end of gdx_fontconv_sample) rewrites the freshly converted
// Sample to point at arena copies of the pack bytes. Overrides apply the next time the owning
// font is converted (scene/song load); already-converted fonts are not retro-swapped.

#ifndef GDX_AUDIO_SAMPLE_PACKS_H
#define GDX_AUDIO_SAMPLE_PACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Applies the mounted-pack override for the stock sample identified by (bankId, bankOffset,
// stockSize) to the just-converted decomp Sample at `sample` (opaque; the .cpp mirrors the
// layout). bankId < 0 means the sample is device-addressed (no bank) and cannot be overridden.
// Any validation failure leaves the stock sample untouched.
void GdxSamplePackApply(int32_t fontId, int32_t bankId, uint32_t bankOffset, uint32_t stockSize, void* sample);

#ifdef __cplusplus
}
#endif

#endif // GDX_AUDIO_SAMPLE_PACKS_H
