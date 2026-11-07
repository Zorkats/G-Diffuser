/* port/gdx_audio_lle_stage.h -- when an LLE staging request may share an earlier window.
 *
 * gdx_stage in gdx_audio_lle.c copies host buffers into RDRAM scratch for the real aspMain run and maps a host
 * pointer staged earlier in the same tick to the same window. Sharing is required for buffers the RSP writes: a
 * reverb ring loaded then saved in one tick, and the persistent per-note state slots.
 *
 * A plain input load is different. Several voices of one sample can load from the same host pointer in one tick
 * with different sizes: every voice that wraps its loop reads from the loop's first frame, and each pitch asks
 * for a different number of frames. The RSP DMAs the size it asks for, so reusing a smaller window made it read
 * the next window's bytes as ADPCM, which decoded to full-scale noise at the end of the tick (issue #33: the KO
 * star's five voices, and engines sharing a sample in a packed field).
 */
#ifndef GDX_AUDIO_LLE_STAGE_H
#define GDX_AUDIO_LLE_STAGE_H

#include <stdint.h>

/* Returns 1 when a request may use the window already staged this tick for the same host pointer.
 *
 * haveSize, haveXf, haveIsOutput: the staged window (its byte count, endianness transform, and whether the RSP
 * writes it). wantSize, wantXf: the new request. wantWrites: the request is an output or a persistent slot. */
static inline int gdx_lle_stage_can_reuse(uint32_t haveSize, int haveXf, int haveIsOutput, uint32_t wantSize,
                                          int wantXf, int wantWrites) {
    if (wantWrites || haveIsOutput) {
        return 1;
    }
    return (haveSize >= wantSize) && (haveXf == wantXf);
}

#endif
