/* LLE audio dispatch: the seam between the game's audio scheduler and the audio RSP. Each
 * M_AUDTASK goes either to the REAL aspMain microcode running on the vendored cxd4 RSP
 * interpreter, through the per-tick memory-model bridge below, or falls back to the software HLE
 * interpreter (gdx_audio_hle_run). LLE is the default -- it is the grain-free path.
 *
 * The microcode itself never ships: it is Nintendo-copyrighted, so decomp/assets/yaml/us/rev0/
 * rsp_blob.yaml extracts it from the user's own ROM into the locally-generated archive and
 * gdx_lle_load_ucode_blob below reads it back from there. No archive entry, no LLE.
 *
 * THE MEMORY-MODEL BRIDGE (gdx_audio_lle_bridge_run)
 * aspMain DMAs its command list and every buffer that list references (compressed sample data,
 * ADPCM codebooks, per-note decode/resample state, reverb rings, the AI output PCM) straight out
 * of RDRAM by PHYSICAL OFFSET. On this host those buffers are scattered across the 16MB gdx_rdram
 * arena and the exe's static image, and an Acmd w1 field carries only the low 32 bits of a host
 * pointer (see n64_audio_hle.c's truncation note). So the bridge reconstructs each full host
 * pointer with the same resolvers the HLE uses, copies the referenced buffer into a contiguous
 * physical scratch window the RSP can reach, rewrites the command's w1 to that scratch offset,
 * runs one task, then copies RSP-written buffers back out to their host homes. The scratch is a
 * bounded per-tick arena rather than a full heap relocation: a relocated heap cannot fit under
 * cxd4's 24-bit DMA addressing alongside the 12-13MB GFX working set.
 *
 * THE ENDIANNESS RULE. cxd4 stores ALL of RDRAM/DMEM/IMEM as HOST-NATIVE LITTLE-ENDIAN 32-BIT
 * WORDS, modelling N64 big-endian memory as "byte-swap each aligned 32-bit word". Our scratch IS
 * a region of gdx_rdram (cxd4's RDRAM base is pointed straight at it by gdx_rsp_lle_init), so a
 * host-native u32 stored at gdx_rdram+P is exactly the word cxd4 reads as RDRAM word P. Placing a
 * buffer for the RSP therefore means transforming each aligned 32-bit HOST word according to how
 * the host stores that buffer's element type:
 *
 *   GDX_XFORM_CMD (identity) -- Acmd words. The game already built w0/w1 as host-native u32, and
 *       a host-native u32 IS an N64-BE u32 word-swapped, so copying the raw command bytes verbatim
 *       is already correct; only address-bearing w1 fields need rewriting.
 *
 *   GDX_XFORM_S16 (rotate-16) -- arrays the host stores as NATIVE s16 (decomp reads them with
 *       plain `short`/gdx_rd_s16: predictorState, adpcmdecState, finalResampleState, filter state
 *       and coefficients, reverb-ring PCM, AI output PCM). Word-swapping the N64-BE form of a
 *       native s16 pair reduces, on the host word W, to exactly (W>>16)|(W<<16).
 *
 *   GDX_XFORM_BYTESTREAM (bswap32) -- buffers the host stores as a RAW big-endian byte stream in
 *       N64 linear order (compressed ADPCM sample data loaded straight from cart/disk; the HLE
 *       copies those bytes 1:1 into DMEM and decodes them correctly, which proves host order ==
 *       N64 linear byte order). Word-swapping a linear byte run is a plain 32-bit byte reversal.
 *
 * All three transforms are their own inverse, so COPY-BACK uses the SAME transform as copy-in.
 *
 * Two choices rest on inference rather than a hard ABI field, and are flagged TUNING KNOB at
 * their sites: (1) the A_LOADBUFF compressed-vs-PCM classifier (BYTESTREAM only for the single
 * compressed-ADPCM load, identified by its DMEM window ending at DMEM_COMPRESSED_ADPCM_DATA) --
 * revisit if uncompressed CODEC_S16 samples come out wrong; (2) whether the host really stores
 * compressed sample data big-endian -- if LLE ADPCM is noise while HLE is clean, flip that branch.
 *
 * SAFETY NET. If any opcode or address cannot be confidently marshalled (unresolved w1 token,
 * truly unknown opcode, scratch overflow) the bridge ABORTS the tick and defers to
 * gdx_audio_hle_run. No host buffer is mutated before a successful RSP run, so the fall-through is
 * clean: LLE never crashes, worst case it silently degrades to HLE for one tick. Cross-tick state
 * stays consistent across mixed LLE/HLE ticks because the HOST buffers are the source of truth --
 * every input is copied host->scratch before the run and every output scratch->host after it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gdx_audio_lle_stage.h"
#include "rsp/cxd4/gdx_rsp_driver.h"
#include "n64_rdram.h"

extern void gdx_audio_hle_run(const void* dataPtr, unsigned int dataSizeBytes);
extern int gdx_unlock_diag_enabled(void);
extern void gdx_unlock_diagf(const char* fmt, ...);

/* Raw archive-entry loader (port/AssetLoader.cpp). */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

/* US rev0 aspMain slices, extracted into the archive by rsp_blob.yaml. */
#define GDX_UCODE_TEXT_BYTES 0xD30u /* ROM 0x66270..0x66FA0 */
#define GDX_UCODE_DATA_BYTES 0x2E0u /* ROM 0x71CA0..0x71F80 */

/* Archive entry layout: 64-byte Torch header, u32 LE payload size, then the raw ROM bytes. */
#define GDX_O2R_HEADER_BYTES 0x40u
#define GDX_O2R_PAYLOAD_OFF (GDX_O2R_HEADER_BYTES + 4u)

static uint8_t* sUcodeText; /* aspMain text, payload of rsp_blob/aspmain_text */
static uint8_t* sUcodeData; /* aspMain data, payload of rsp_blob/aspmain_data */

/* Low-32 -> full host pointer resolvers (port/n64_gfx_bridge.cpp; the same ones the HLE uses).
 * Deliberately `unsigned int`: unambiguously 32 bits in every TU, unlike uintptr_t here. */
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);

/* Verbose-diagnostics gate (defined in port/n64_sched.c). */
extern int gdx_diag_verbose(void);

/* Defined in libultraship (C ABI). Declared here so this C TU need not pull the C++ bridge
 * header. */
extern int CVarGetInteger(const char* name, int defaultValue);

static int gdx_audio_lle_enabled(void) {
    /* Read each tick on the audio thread. The CVar is pre-registered at boot, so a concurrent
     * menu write is a benign int-value race -- worst case one tick sees the old value. */
    return CVarGetInteger("gEnhancements.Audio.LLE", 1) != 0;
}

static int sGdxUnlockDspTraceTasks = 0;
static unsigned int sGdxUnlockDspTraceGeneration = 0;

void gdx_unlock_audio_trace_dsp_begin(void) {
    if (gdx_unlock_diag_enabled()) {
        sGdxUnlockDspTraceTasks = 120;
        sGdxUnlockDspTraceGeneration++;
        if (sGdxUnlockDspTraceGeneration == 0) {
            sGdxUnlockDspTraceGeneration = 1;
        }
    }
}

int gdx_unlock_audio_trace_dsp_active(void) {
    return sGdxUnlockDspTraceTasks > 0;
}

unsigned int gdx_unlock_audio_trace_generation(void) {
    return sGdxUnlockDspTraceGeneration;
}

static void gdx_unlock_audio_trace_dsp_consume(void) {
    if (sGdxUnlockDspTraceTasks > 0) {
        sGdxUnlockDspTraceTasks--;
    }
}

#define GDX_UNLOCK_AI_CAPTURE_FRAMES 64000u
#define GDX_UNLOCK_AI_CAPTURE_CHANNELS 2u
#define GDX_UNLOCK_AI_CAPTURE_PATH "gdiffuser-unlock-ai.wav"

typedef struct GdxUnlockAiCapture {
    unsigned int generation;
    uint32_t frames;
    uint32_t buffers;
    uint32_t maxDelta[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    uint32_t zeroSamples[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    uint32_t clippedSamples[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    uint64_t squareSum[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int64_t sampleSum[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int16_t minSample[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int16_t maxSample[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int16_t previousSample[GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int16_t pcm[GDX_UNLOCK_AI_CAPTURE_FRAMES * GDX_UNLOCK_AI_CAPTURE_CHANNELS];
    int hasPreviousSample;
    int complete;
} GdxUnlockAiCapture;

static GdxUnlockAiCapture sGdxUnlockAiCapture;

static void gdx_unlock_ai_capture_put_u16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void gdx_unlock_ai_capture_put_u32(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void gdx_unlock_ai_capture_reset(unsigned int generation, unsigned int sampleRate) {
    unsigned int channel;

    sGdxUnlockAiCapture.generation = generation;
    sGdxUnlockAiCapture.frames = 0;
    sGdxUnlockAiCapture.buffers = 0;
    sGdxUnlockAiCapture.hasPreviousSample = 0;
    sGdxUnlockAiCapture.complete = 0;
    for (channel = 0; channel < GDX_UNLOCK_AI_CAPTURE_CHANNELS; channel++) {
        sGdxUnlockAiCapture.maxDelta[channel] = 0;
        sGdxUnlockAiCapture.zeroSamples[channel] = 0;
        sGdxUnlockAiCapture.clippedSamples[channel] = 0;
        sGdxUnlockAiCapture.squareSum[channel] = 0;
        sGdxUnlockAiCapture.sampleSum[channel] = 0;
        sGdxUnlockAiCapture.minSample[channel] = INT16_MAX;
        sGdxUnlockAiCapture.maxSample[channel] = INT16_MIN;
        sGdxUnlockAiCapture.previousSample[channel] = 0;
    }

    gdx_unlock_diagf("[unlock-audio] ai tap begin generation=%u frames=%u sampleRate=%u channels=2 "
                     "format=s16le boundary=pre-osAiSetNextBuffer\n",
                     generation, GDX_UNLOCK_AI_CAPTURE_FRAMES, sampleRate);
}

static int gdx_unlock_ai_capture_write_wav(unsigned int sampleRate) {
    uint8_t header[44];
    uint32_t dataBytes = sGdxUnlockAiCapture.frames * GDX_UNLOCK_AI_CAPTURE_CHANNELS * sizeof(int16_t);
    FILE* file;
    int wroteAll;

    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    gdx_unlock_ai_capture_put_u32(&header[4], 36u + dataBytes);
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    gdx_unlock_ai_capture_put_u32(&header[16], 16u);
    gdx_unlock_ai_capture_put_u16(&header[20], 1u);
    gdx_unlock_ai_capture_put_u16(&header[22], GDX_UNLOCK_AI_CAPTURE_CHANNELS);
    gdx_unlock_ai_capture_put_u32(&header[24], sampleRate);
    gdx_unlock_ai_capture_put_u32(&header[28],
                                  sampleRate * GDX_UNLOCK_AI_CAPTURE_CHANNELS * sizeof(int16_t));
    gdx_unlock_ai_capture_put_u16(&header[32], GDX_UNLOCK_AI_CAPTURE_CHANNELS * sizeof(int16_t));
    gdx_unlock_ai_capture_put_u16(&header[34], sizeof(int16_t) * 8u);
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    gdx_unlock_ai_capture_put_u32(&header[40], dataBytes);

    file = fopen(GDX_UNLOCK_AI_CAPTURE_PATH, "wb");
    if (file == NULL) {
        return 0;
    }
    wroteAll = (fwrite(header, 1, sizeof(header), file) == sizeof(header)) &&
               (fwrite(sGdxUnlockAiCapture.pcm, 1, dataBytes, file) == dataBytes);
    if (fclose(file) != 0) {
        wroteAll = 0;
    }
    return wroteAll;
}

static void gdx_unlock_ai_capture_finish(unsigned int sampleRate) {
    unsigned int channel;
    int wroteWav;

    sGdxUnlockAiCapture.complete = 1;
    for (channel = 0; channel < GDX_UNLOCK_AI_CAPTURE_CHANNELS; channel++) {
        double mean = (double)sGdxUnlockAiCapture.sampleSum[channel] / sGdxUnlockAiCapture.frames;
        double meanSquare = (double)sGdxUnlockAiCapture.squareSum[channel] / sGdxUnlockAiCapture.frames;

        gdx_unlock_diagf("[unlock-audio] ai tap stats generation=%u channel=%c min=%d max=%d "
                         "mean=%.3f rms=%.3f zeros=%u clipped=%u maxDelta=%u\n",
                         sGdxUnlockAiCapture.generation, channel == 0 ? 'L' : 'R',
                         sGdxUnlockAiCapture.minSample[channel], sGdxUnlockAiCapture.maxSample[channel],
                         mean, sqrt(meanSquare), sGdxUnlockAiCapture.zeroSamples[channel],
                         sGdxUnlockAiCapture.clippedSamples[channel], sGdxUnlockAiCapture.maxDelta[channel]);
    }

    wroteWav = gdx_unlock_ai_capture_write_wav(sampleRate);
    gdx_unlock_diagf("[unlock-audio] ai tap complete generation=%u buffers=%u frames=%u artifact=%s "
                     "bytes=%u status=%s\n",
                     sGdxUnlockAiCapture.generation, sGdxUnlockAiCapture.buffers,
                     sGdxUnlockAiCapture.frames, GDX_UNLOCK_AI_CAPTURE_PATH,
                     44u + sGdxUnlockAiCapture.frames * GDX_UNLOCK_AI_CAPTURE_CHANNELS * sizeof(int16_t),
                     wroteWav ? "ok" : "write-failed");
}

void gdx_unlock_audio_capture_ai_buffer(const int16_t* buffer, unsigned int frameCount,
                                        unsigned int sampleRate) {
    unsigned int generation = sGdxUnlockDspTraceGeneration;
    uint32_t framesToCopy;
    uint32_t frame;
    unsigned int channel;

    if ((generation == 0) || (buffer == NULL) || (frameCount == 0)) {
        return;
    }
    if (generation != sGdxUnlockAiCapture.generation) {
        gdx_unlock_ai_capture_reset(generation, sampleRate);
    }
    if (sGdxUnlockAiCapture.complete) {
        return;
    }

    framesToCopy = frameCount;
    if (framesToCopy > GDX_UNLOCK_AI_CAPTURE_FRAMES - sGdxUnlockAiCapture.frames) {
        framesToCopy = GDX_UNLOCK_AI_CAPTURE_FRAMES - sGdxUnlockAiCapture.frames;
    }
    sGdxUnlockAiCapture.buffers++;

    for (frame = 0; frame < framesToCopy; frame++) {
        for (channel = 0; channel < GDX_UNLOCK_AI_CAPTURE_CHANNELS; channel++) {
            int32_t value = buffer[frame * GDX_UNLOCK_AI_CAPTURE_CHANNELS + channel];
            uint32_t captureIndex =
                (sGdxUnlockAiCapture.frames + frame) * GDX_UNLOCK_AI_CAPTURE_CHANNELS + channel;

            sGdxUnlockAiCapture.pcm[captureIndex] = (int16_t)value;
            sGdxUnlockAiCapture.sampleSum[channel] += value;
            sGdxUnlockAiCapture.squareSum[channel] += (uint64_t)(value * value);
            if (value < sGdxUnlockAiCapture.minSample[channel]) {
                sGdxUnlockAiCapture.minSample[channel] = (int16_t)value;
            }
            if (value > sGdxUnlockAiCapture.maxSample[channel]) {
                sGdxUnlockAiCapture.maxSample[channel] = (int16_t)value;
            }
            if (value == 0) {
                sGdxUnlockAiCapture.zeroSamples[channel]++;
            }
            if ((value == INT16_MIN) || (value == INT16_MAX)) {
                sGdxUnlockAiCapture.clippedSamples[channel]++;
            }
            if (sGdxUnlockAiCapture.hasPreviousSample) {
                int32_t signedDelta = value - sGdxUnlockAiCapture.previousSample[channel];
                uint32_t delta = signedDelta < 0 ? (uint32_t)-signedDelta : (uint32_t)signedDelta;

                if (delta > sGdxUnlockAiCapture.maxDelta[channel]) {
                    sGdxUnlockAiCapture.maxDelta[channel] = delta;
                }
            }
            sGdxUnlockAiCapture.previousSample[channel] = (int16_t)value;
        }
        sGdxUnlockAiCapture.hasPreviousSample = 1;
    }

    sGdxUnlockAiCapture.frames += framesToCopy;
    if (sGdxUnlockAiCapture.frames == GDX_UNLOCK_AI_CAPTURE_FRAMES) {
        gdx_unlock_ai_capture_finish(sampleRate);
    }
}

static void gdx_lle_logf(const char* fmt, ...) {
    va_list ap;
    FILE* f;
    if (!gdx_diag_verbose()) {
        return;
    }
    f = fopen("gdiffuser-run.log", "a");
    if (f == NULL) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* Mirrors n64_audio_hle.c / PR/abi.h. EXPANSION_KIT build, hence A_HILOGAIN == 14. */
enum {
    GDX_A_SPNOOP       = 0,
    GDX_A_ADPCM        = 1,
    GDX_A_CLEARBUFF    = 2,
    GDX_A_UNK3         = 3,
    GDX_A_ADDMIXER     = 4,
    GDX_A_RESAMPLE     = 5,
    GDX_A_RESAMPLE_ZOH = 6,
    GDX_A_FILTER       = 7,
    GDX_A_SETBUFF      = 8,
    GDX_A_DMEMMOVE     = 10,
    GDX_A_LOADADPCM    = 11,
    GDX_A_MIXER        = 12,
    GDX_A_INTERLEAVE   = 13,
    GDX_A_HILOGAIN     = 14,
    GDX_A_SETLOOP      = 15,
    GDX_A_INTERL       = 17,
    GDX_A_ENVSETUP1    = 18,
    GDX_A_ENVMIXER     = 19,
    GDX_A_LOADBUFF     = 20,
    GDX_A_SAVEBUFF     = 21,
    GDX_A_ENVSETUP2    = 22,
    GDX_A_S8DEC        = 23,
    GDX_A_UNK19        = 25,
    GDX_A_DUPLICATE    = 26
};

/* Classifies A_LOADBUFF as a compressed sample chunk vs PCM. The one compressed-ADPCM load in
 * disk/lib/synthesis.c passes addr = DMEM_COMPRESSED_ADPCM_DATA - aligned, so it is the unique
 * load whose DMEM window ENDS exactly here (mod 0x10000, which absorbs the u16 wrap when
 * aligned > addr). Reverb-ring and PCM loads target other DMEM addresses. */
#define GDX_DMEM_COMPRESSED_ADPCM_DATA 0x940u

/* State-buffer DMA sizes in bytes, matching what the real aspMain DMAs and cross-checked against
 * the decomp struct fields:
 *   adpcmdecState[16]       -> 32  (A_ADPCM / A_S8DEC persistent decode history)
 *   finalResampleState[16]  -> 32  (A_RESAMPLE persistent 4-tap window + frac + pitch)
 *   predictorState[16]      -> 32  (A_SETLOOP loop-restart history; ROM-constant, read-only)
 *   filter coef  = FILTER_SIZE                   = 8*2  = 16 (A_FILTER prime, read-only)
 *   filter state = 2*(FILTER_BUF_PART1+PART2)    = 2*32 = 64 (A_FILTER apply, in/out) */
#define GDX_STATE_ADPCM_BYTES     32u
#define GDX_STATE_RESAMPLE_BYTES  32u
#define GDX_STATE_LOOP_BYTES      32u
#define GDX_FILTER_COEF_BYTES     16u
#define GDX_FILTER_STATE_BYTES    64u
/* A_LOADADPCM codebook DMA cap (matches the HLE's private book scratch bound). */
#define GDX_ADPCM_BOOK_MAX_BYTES  4096u

/* Endianness transforms; all involutions, see the file header. */
typedef enum {
    GDX_XFORM_CMD = 0,      /* identity -- Acmd words */
    GDX_XFORM_S16,          /* rotate-16 -- native s16 arrays */
    GDX_XFORM_BYTESTREAM    /* bswap32 -- raw big-endian byte streams */
} GdxXform;

static uint32_t gdx_bswap32(uint32_t w) {
    return ((w & 0x000000FFu) << 24) | ((w & 0x0000FF00u) << 8) |
           ((w & 0x00FF0000u) >> 8)  | ((w & 0xFF000000u) >> 24);
}
static uint32_t gdx_rot16(uint32_t w) {
    return (w >> 16) | (w << 16);
}
static uint32_t gdx_xform_word(uint32_t w, GdxXform xf) {
    switch (xf) {
        case GDX_XFORM_S16:        return gdx_rot16(w);
        case GDX_XFORM_BYTESTREAM: return gdx_bswap32(w);
        case GDX_XFORM_CMD:
        default:                   return w;
    }
}

/* Scratch arena layout; all offsets are RELATIVE to sScratchPhys.
 *   [0 .. DATA_END)              aspMain data section, written once at init.
 *   [PERSIST_OFF .. PERSIST_END) persistent state slots, never reset: in/out decode/resample/
 *                                filter state keyed by host pointer, so a given note's state
 *                                always maps to the same scratch slot.
 *   [TICK_OFF .. SCRATCH_SIZE)   per-tick bump arena, reset every tick: command list, compressed
 *                                sample data, codebooks, filter coefs, reverb-ring and AI-output
 *                                DMA windows, and any input-only buffer.
 * Carved from the TOP of RDRAM, above the ~12-13MB GFX working set, and never reclaimed. */
#define GDX_LLE_SCRATCH_SIZE   0x200000u   /* 2 MB (well over any observed audio tick) */
#define GDX_LLE_DATA_OFF       0x000000u
#define GDX_LLE_PERSIST_OFF    0x000300u   /* after the 0x2E0-byte data section, 16-aligned */
#define GDX_LLE_PERSIST_END    0x040000u   /* 256 KB for persistent state slots */
#define GDX_LLE_TICK_OFF       0x040000u   /* per-tick arena starts here */

static int      sInitState  = 0;           /* 0 = not attempted, 1 = ready, -1 = failed */
static uint32_t sScratchPhys = 0;          /* physical RDRAM offset of the scratch base */
static uint32_t sDataSecPhys = 0;          /* physical offset of the aspMain data section */

/* Persistent state slot table (stable host-ptr -> scratch offset across ticks). */
#define GDX_PERSIST_MAX 512
typedef struct {
    const void* host;
    uint32_t    phys;
    uint32_t    size;   /* rounded slot size */
} GdxPersistSlot;
static GdxPersistSlot sPersist[GDX_PERSIST_MAX];
static int            sPersistCount = 0;
static uint32_t       sPersistBump  = GDX_LLE_PERSIST_OFF;

/* Per-tick mapping list: dedups repeated references within a tick and records which
 * buffers to copy back after the run. Rebuilt each tick. */
#define GDX_MAP_MAX 1024
typedef struct {
    const void* host;
    uint32_t    phys;
    uint32_t    size;      /* exact byte count to copy back */
    GdxXform    xf;
    int         isOutput;  /* copy scratch->host after the RSP run */
} GdxMap;
static GdxMap sMap[GDX_MAP_MAX];
static int    sMapCount = 0;
static uint32_t sTickBump = GDX_LLE_TICK_OFF;

/* Move `bytes` between a host buffer and the scratch (itself host-addressable memory inside
 * gdx_rdram), transforming each aligned 32-bit word. Both handle a ragged 1-3 byte tail without
 * reading or writing past either endpoint. */
static void gdx_copy_in(uint32_t dstPhys, const void* src, uint32_t bytes, GdxXform xf) {
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = gdx_rdram + dstPhys;
    uint32_t full = bytes & ~3u;
    uint32_t i;
    for (i = 0; i < full; i += 4u) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        w = gdx_xform_word(w, xf);
        memcpy(d + i, &w, 4);
    }
    /* Dead for every real buffer -- every DMA size here is a multiple of 4 (states 32/64/16B,
     * LOAD/SAVE sizes are (n<<4), codebooks 32B multiples, the data section 736B). A word
     * transform cannot straddle a sub-word boundary, so a pathological unaligned size falls back
     * to a raw byte copy: never out of bounds, and never zeroes a neighbouring byte the way a
     * transformed partial word would. */
    if (bytes & 3u) {
        memcpy(d + full, s + full, bytes & 3u);
    }
}
static void gdx_copy_out(void* dst, uint32_t srcPhys, uint32_t bytes, GdxXform xf) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = gdx_rdram + srcPhys;
    uint32_t full = bytes & ~3u;
    uint32_t i;
    for (i = 0; i < full; i += 4u) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        w = gdx_xform_word(w, xf);   /* same transform: each is its own inverse */
        memcpy(d + i, &w, 4);
    }
    if (bytes & 3u) {                /* see gdx_copy_in: safe raw tail, never reached in practice */
        memcpy(d + full, s + full, bytes & 3u);
    }
}

/* Load one microcode blob from the archive. Returns a pointer to the payload (process-lifetime
 * allocation, never freed -- the RSP driver keeps using it), or NULL if the entry is missing or
 * its size is wrong. */
static uint8_t* gdx_lle_load_ucode_blob(const char* key, uint32_t expectBytes) {
    const size_t cap = (size_t)GDX_O2R_PAYLOAD_OFF + expectBytes;
    uint8_t* buf = (uint8_t*)malloc(cap);
    size_t copied = 0;
    uint32_t payloadSize;

    if (buf == NULL) {
        return NULL;
    }
    if (!GDiffuser_LoadArchiveFileBytes(key, buf, cap, &copied) || copied < cap) {
        free(buf);
        return NULL;
    }
    payloadSize = (uint32_t)buf[GDX_O2R_HEADER_BYTES] | ((uint32_t)buf[GDX_O2R_HEADER_BYTES + 1] << 8) |
                  ((uint32_t)buf[GDX_O2R_HEADER_BYTES + 2] << 16) | ((uint32_t)buf[GDX_O2R_HEADER_BYTES + 3] << 24);
    if (payloadSize != expectBytes) {
        free(buf);
        return NULL;
    }
    return buf + GDX_O2R_PAYLOAD_OFF;
}

/* One-time init: wire cxd4, reserve the scratch, seed the data section. Returns 1 if the bridge
 * is usable, 0 if it must permanently defer to HLE. */
static int gdx_lle_init_once(void) {
    void* scratch;
    uintptr_t rel;

    if (sInitState != 0) {
        return sInitState > 0;
    }

    if (gdx_rdram == NULL) {
        /* Should never happen (RDRAM is up long before audio), but never crash. */
        sInitState = -1;
        return 0;
    }

    if (sUcodeText == NULL) {
        sUcodeText = gdx_lle_load_ucode_blob("rsp_blob/aspmain_text", GDX_UCODE_TEXT_BYTES);
    }
    if (sUcodeData == NULL) {
        sUcodeData = gdx_lle_load_ucode_blob("rsp_blob/aspmain_data", GDX_UCODE_DATA_BYTES);
    }
    if (sUcodeText == NULL || sUcodeData == NULL) {
        /* Stale dev archive or a profile without rsp_blob entries: HLE keeps audio working. */
        fprintf(stderr, "[audio-lle] bridge DISABLED: aspMain microcode missing from the archive\n");
        sInitState = -1;
        return 0;
    }

    gdx_rsp_lle_init(gdx_rdram);

    /* Session-lifetime scratch from the TOP of RDRAM, downward. The allocator is fatal on
     * exhaustion, so a non-NULL return is guaranteed. */
    scratch = gdx_rdram_persist_alloc_raw((size_t)GDX_LLE_SCRATCH_SIZE, (size_t)16);
    rel = (uintptr_t)((unsigned char*)scratch - gdx_rdram);

    /* cxd4 masks DMA addresses to 24 bits: the ENTIRE scratch must live below 16MB. */
    if (rel + (uintptr_t)GDX_LLE_SCRATCH_SIZE > (uintptr_t)GDX_RDRAM_SIZE) {
        fprintf(stderr, "[audio-lle] bridge DISABLED: scratchPhys=%06lX + size exceeds 24-bit DMA window\n",
                (unsigned long)rel);
        sInitState = -1;
        return 0;
    }
    sScratchPhys = (uint32_t)rel;

    /* Seed the aspMain data section (raw big-endian ROM blob). The RSP DMAs ucode_data from here
     * into DMEM at boot, and the driver byte-swaps the same blob into DMEM[0] as a fallback, so
     * the two must agree: bswap32 per word. */
    sDataSecPhys = sScratchPhys + GDX_LLE_DATA_OFF;
    gdx_copy_in(sDataSecPhys, sUcodeData, GDX_UCODE_DATA_BYTES, GDX_XFORM_BYTESTREAM);

    sPersistBump  = GDX_LLE_PERSIST_OFF;
    sPersistCount = 0;

    fprintf(stderr, "[audio-lle] bridge init OK: scratchPhys=%06X size=%X dataSec=%06X (len=%X)\n",
            (unsigned)sScratchPhys, (unsigned)GDX_LLE_SCRATCH_SIZE,
            (unsigned)sDataSecPhys, (unsigned)GDX_UCODE_DATA_BYTES);

    sInitState = 1;
    return 1;
}

/* Persistent slot: stable scratch offset for a given host state pointer across ticks.
 * Returns 1 and sets *outPhys on success; 0 if the persistent region is exhausted. */
static int gdx_persist_slot(const void* host, uint32_t size, uint32_t* outPhys) {
    uint32_t rounded = (size + 15u) & ~15u;
    int i;
    for (i = 0; i < sPersistCount; i++) {
        if (sPersist[i].host == host) {
            if (sPersist[i].size < rounded) {
                return 0;   /* a grown request must never overflow a fixed slot */
            }
            *outPhys = sPersist[i].phys;
            return 1;
        }
    }
    if (sPersistCount >= GDX_PERSIST_MAX) {
        return 0;
    }
    if (sPersistBump + rounded > GDX_LLE_PERSIST_END) {
        return 0;
    }
    sPersist[sPersistCount].host = host;
    sPersist[sPersistCount].phys = sScratchPhys + sPersistBump;
    sPersist[sPersistCount].size = rounded;
    *outPhys = sPersist[sPersistCount].phys;
    sPersistBump += rounded;
    sPersistCount++;
    return 1;
}

/* Stage one address-bearing buffer into the scratch and hand back its physical offset.
 *   host       resolved full host pointer (never NULL here)
 *   size       exact bytes the RSP will DMA
 *   xf         endianness transform for this buffer's element type
 *   isOutput   RSP writes it -> copy scratch->host after the run
 *   persistent true for cross-tick in/out state (stable slot); false for per-tick
 *   doCopyIn   copy host->scratch now (all inputs and in/out state; false for pure
 *              outputs like the AI buffer that the RSP fills from scratch)
 * Returns 1 on success (*outPhys set), 0 on any overflow (caller must bail to HLE). */
static int gdx_stage(const void* host, uint32_t size, GdxXform xf,
                     int isOutput, int persistent, int doCopyIn, uint32_t* outPhys) {
    uint32_t phys;
    uint32_t rounded;
    int i;

    /* Within-tick dedup: a host buffer the RSP writes maps to one scratch slot, so e.g. a
     * reverb ring loaded then saved in one tick shares its DMA window (the save sees the
     * load's data), and repeated state references copy in only once. A plain input only
     * shares a window that is at least as large, with the same transform: see
     * gdx_audio_lle_stage.h for the loop-wrap case that broke (issue #33). */
    for (i = 0; i < sMapCount; i++) {
        if (sMap[i].host == host &&
            gdx_lle_stage_can_reuse(sMap[i].size, (int) sMap[i].xf, sMap[i].isOutput, size, (int) xf,
                                    isOutput || persistent)) {
            if (isOutput) {
                sMap[i].isOutput = 1;
            }
            *outPhys = sMap[i].phys;
            return 1;
        }
    }

    rounded = (size + 15u) & ~15u;
    if (persistent) {
        if (!gdx_persist_slot(host, size, &phys)) {
            return 0;
        }
    } else {
        if (sTickBump + rounded > GDX_LLE_SCRATCH_SIZE) {
            return 0;
        }
        phys = sScratchPhys + sTickBump;
        sTickBump += rounded;
    }

    if (sMapCount >= GDX_MAP_MAX) {
        return 0;
    }

    if (doCopyIn) {
        gdx_copy_in(phys, host, size, xf);
    }

    sMap[sMapCount].host     = host;
    sMap[sMapCount].phys     = phys;
    sMap[sMapCount].size     = size;
    sMap[sMapCount].xf       = xf;
    sMap[sMapCount].isOutput = isOutput;
    sMapCount++;

    *outPhys = phys;
    return 1;
}

/* Resolve an Acmd w1 low-32 token to a full host pointer (registered range first, then
 * the exe module image). Returns NULL if neither resolver knows it (caller bails). */
static void* gdx_lle_resolve(uint32_t raw) {
    void* p;
    if (raw == 0) {
        return NULL;
    }
    p = gdx_resolve_registered_host_address((unsigned int)raw);
    if (p != NULL) {
        return p;
    }
    return gdx_resolve_module_host_address((unsigned int)raw);
}

static void gdx_audio_lle_bridge_run(const void* dataPtr, unsigned int dataSizeBytes) {
    const uint32_t* cmds;       /* host command list, {w0,w1} pairs of host-native u32 */
    uint32_t count;
    uint32_t cmdBytes;
    uint32_t cmdListPhys;
    uint32_t* scratchCmds;      /* the scratch copy we rewrite w1 fields in */
    uint32_t ostask[16];        /* 64-byte OSTask, host-native little-endian */
    uint32_t i;
    uint32_t compressedLoads = 0;
    uint32_t pcmLoads = 0;
    static int sBailLogs = 0;

    /* Never mutate host state before a successful run, so a mid-marshal abort hands the
     * untouched tick to the HLE. */
    #define GDX_LLE_BAIL(reasonfmt, ...)                                              \
        do {                                                                          \
            if (gdx_unlock_audio_trace_dsp_active()) {                                \
                gdx_unlock_diagf("[unlock-audio] dsp task selected=LLE outcome=fallback reason=" reasonfmt "\n", \
                                 __VA_ARGS__);                                         \
                gdx_unlock_audio_trace_dsp_consume();                                 \
            }                                                                         \
            if (sBailLogs < 32) {                                                     \
                sBailLogs++;                                                          \
                gdx_lle_logf("[audio-lle] bail -> HLE: " reasonfmt "\n", __VA_ARGS__);\
            }                                                                         \
            gdx_audio_hle_run(dataPtr, dataSizeBytes);                                \
            return;                                                                   \
        } while (0)

    if (!gdx_lle_init_once()) {
        if (gdx_unlock_audio_trace_dsp_active()) {
            gdx_unlock_diagf("[unlock-audio] dsp task selected=LLE outcome=fallback reason=bridge-init-failed\n");
            gdx_unlock_audio_trace_dsp_consume();
        }
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    cmds  = (const uint32_t*)dataPtr;
    count = dataSizeBytes / 8u;                 /* Acmd is 8 bytes: {u32 w0; u32 w1;} */
    if (cmds == NULL || count == 0u) {
        if (gdx_unlock_audio_trace_dsp_active()) {
            gdx_unlock_diagf("[unlock-audio] dsp task selected=LLE outcome=fallback reason=empty-command-list\n");
            gdx_unlock_audio_trace_dsp_consume();
        }
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    /* Reset the per-tick arena and mapping list; persistent slots survive. */
    sTickBump = GDX_LLE_TICK_OFF;
    sMapCount = 0;

    /* Slots are keyed by host pointer and never individually evicted, so audio-heap churn
     * (bank/scene changes reallocate note state to fresh pointers) would eventually fill the
     * table and bail every state-bearing tick to HLE for the rest of the run. Recycle the whole
     * region BETWEEN ticks when nearly full -- never mid-marshal, which would invalidate slots
     * already placed this tick. Safe because host buffers are authoritative: each tick copies
     * state host->scratch before the run, so live slots are re-established next tick with correct
     * data, and at worst a dead reverb tail glitches for one tick. One tick uses under ~32. */
    if (sPersistCount > GDX_PERSIST_MAX - 32) {
        sPersistCount = 0;
        sPersistBump  = GDX_LLE_PERSIST_OFF;
    }

    /* Copy the command list into scratch verbatim (host-native u32 == cxd4 word). The
     * address-bearing w1 fields are rewritten IN THIS COPY, leaving the game's original list
     * pristine so the HLE fallback and the game itself are unaffected. */
    cmdBytes = count * 8u;
    {
        uint32_t rounded = (cmdBytes + 15u) & ~15u;
        if (sTickBump + rounded > GDX_LLE_SCRATCH_SIZE) {
            GDX_LLE_BAIL("cmd list %u bytes overflows scratch", (unsigned)cmdBytes);
        }
        cmdListPhys = sScratchPhys + sTickBump;
        sTickBump += rounded;
        memcpy(gdx_rdram + cmdListPhys, cmds, cmdBytes);
        scratchCmds = (uint32_t*)(void*)(gdx_rdram + cmdListPhys);
    }

    /* Marshal pass: resolve w1, stage the buffer, rewrite the scratch copy's w1. */
    for (i = 0; i < count; i++) {
        uint32_t w0 = cmds[i * 2u + 0u];
        uint32_t w1 = cmds[i * 2u + 1u];
        uint32_t op = (w0 >> 24) & 0xFFu;

        switch (op) {

            /* Address-bearing opcodes: w1 is an RDRAM address. */

            case GDX_A_LOADBUFF: {
                /* INPUT only. The compressed-ADPCM chunk load is the unique one whose DMEM
                 * window ends at DMEM_COMPRESSED_ADPCM_DATA, so it is a raw BE byte stream;
                 * every other load (reverb ring, uncompressed PCM) is native s16.
                 * TUNING KNOB: this classifier and the BYTESTREAM choice. */
                uint32_t size     = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemDest = w0 & 0xFFFFu;
                GdxXform xf = (((dmemDest + size) & 0xFFFFu) == GDX_DMEM_COMPRESSED_ADPCM_DATA)
                              ? GDX_XFORM_BYTESTREAM : GDX_XFORM_S16;
                if (xf == GDX_XFORM_BYTESTREAM) {
                    compressedLoads++;
                } else {
                    pcmLoads++;
                }
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("LOADBUFF unresolved w1=%08X", (unsigned)w1);
                }
                if (size == 0u) {
                    break; /* nothing to DMA; leave w1 as-is (harmless, size 0) */
                }
                if (!gdx_stage(host, size, xf, /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("LOADBUFF scratch overflow (size=%X)", (unsigned)size);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_SAVEBUFF: {
                /* OUTPUT. Destinations are reverb rings and the AI output buffer, all native
                 * s16. No copy-in -- the RSP fills the DMA window. If the same host pointer was
                 * already loaded this tick, gdx_stage's dedup reuses that window, which is what
                 * makes a load->process->save round-trip work. */
                uint32_t size    = ((w0 >> 16) & 0xFFu) << 4;
                uint32_t dmemSrc = w0 & 0xFFFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                (void)dmemSrc;
                if (host == NULL) {
                    GDX_LLE_BAIL("SAVEBUFF unresolved w1=%08X", (unsigned)w1);
                }
                if (size == 0u) {
                    break;
                }
                if (!gdx_stage(host, size, GDX_XFORM_S16, /*out*/1, /*persist*/0, /*copyIn*/0, &phys)) {
                    GDX_LLE_BAIL("SAVEBUFF scratch overflow (size=%X)", (unsigned)size);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_LOADADPCM: {
                /* Load an ADPCM codebook (host-native s16 array, written by gdx_rd_s16).
                 * byteCount = w0 & 0xFFFFFF (capped like the HLE's private book). INPUT. */
                uint32_t byteCount = w0 & 0xFFFFFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("LOADADPCM unresolved w1=%08X", (unsigned)w1);
                }
                if (byteCount > GDX_ADPCM_BOOK_MAX_BYTES) {
                    byteCount = GDX_ADPCM_BOOK_MAX_BYTES;
                }
                if (byteCount == 0u) {
                    break;
                }
                if (!gdx_stage(host, byteCount, GDX_XFORM_S16, /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("LOADADPCM scratch overflow (bytes=%X)", (unsigned)byteCount);
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_SETLOOP: {
                /* Loop-restart predictor history (predictorState[16], host s16). INPUT only and
                 * ROM-constant, so a per-tick slot is fine: the RSP never writes it back. */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("SETLOOP unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_LOOP_BYTES, GDX_XFORM_S16,
                               /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("SETLOOP scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_ADPCM:
            case GDX_A_S8DEC: {
                /* Persistent per-note decode state (adpcmdecState[16], host s16). IN/OUT: the
                 * RSP reads last tick's tail and writes this tick's back. A stable slot keyed by
                 * the state pointer, plus copy in/out each tick, keeps decode continuity correct
                 * even across mixed LLE/HLE ticks. */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("ADPCM/S8DEC unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_ADPCM_BYTES, GDX_XFORM_S16,
                               /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("ADPCM/S8DEC scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_RESAMPLE: {
                /* Persistent per-note resample state (finalResampleState[16], host s16). IN/OUT:
                 * 4-sample history plus fractional accumulator, carried tick to tick. */
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("RESAMPLE unresolved w1=%08X", (unsigned)w1);
                }
                if (!gdx_stage(host, GDX_STATE_RESAMPLE_BYTES, GDX_XFORM_S16,
                               /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                    GDX_LLE_BAIL("RESAMPLE scratch overflow%s", "");
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            case GDX_A_FILTER: {
                /* Two-step op, see the HLE's RunFilter header:
                 *   prime (f==2): w1 = coefficient table (FILTER_SIZE=16 host s16), INPUT.
                 *   apply (else): w1 = filter delay-line state (64 bytes host s16), IN/OUT and
                 *                 persistent -- history carries across command lists. */
                uint32_t f = (w0 >> 16) & 0xFFu;
                void* host = gdx_lle_resolve(w1);
                uint32_t phys;
                if (host == NULL) {
                    GDX_LLE_BAIL("FILTER(f=%u) unresolved w1=%08X", (unsigned)f, (unsigned)w1);
                }
                if (f == 2u) {
                    if (!gdx_stage(host, GDX_FILTER_COEF_BYTES, GDX_XFORM_S16,
                                   /*out*/0, /*persist*/0, /*copyIn*/1, &phys)) {
                        GDX_LLE_BAIL("FILTER coef scratch overflow%s", "");
                    }
                } else {
                    if (!gdx_stage(host, GDX_FILTER_STATE_BYTES, GDX_XFORM_S16,
                                   /*out*/1, /*persist*/1, /*copyIn*/1, &phys)) {
                        GDX_LLE_BAIL("FILTER state scratch overflow%s", "");
                    }
                }
                scratchCmds[i * 2u + 1u] = phys;
                break;
            }

            /* Non-address opcodes: w1 is packed DMEM offsets or immediates, NEVER a pointer, so
             * there is nothing to marshal. Two that read as address-bearing but are not:
             * A_ENVSETUP2's w1 is TWO immediate volume words under the EXPANSION_KIT ABI, not a
             * state address (contrary to the general N64 lore), and A_ENVMIXER's w1 is a packed
             * DMEM-dest word despite the union field being named `addr`. A_HILOGAIN's w1 high
             * half is a DMEM offset. */
            case GDX_A_SPNOOP:
            case GDX_A_CLEARBUFF:
            case GDX_A_SETBUFF:
            case GDX_A_DMEMMOVE:
            case GDX_A_MIXER:
            case GDX_A_ADDMIXER:
            case GDX_A_INTERLEAVE:
            case GDX_A_INTERL:
            case GDX_A_ENVSETUP1:
            case GDX_A_ENVSETUP2:
            case GDX_A_ENVMIXER:
            case GDX_A_HILOGAIN:
            /* SDK-unknown or unused by this decomp: DMEM-internal or no-ops in synthesis.c, so
             * they carry no RDRAM address and pass through untouched. */
            case GDX_A_UNK3:
            case GDX_A_RESAMPLE_ZOH:
            case GDX_A_UNK19:
            case GDX_A_DUPLICATE:
                break;

            /* Genuinely unknown opcode: w1 cannot be proven not to be an address, so degrade
             * this tick to the HLE rather than feed the RSP a bad DMA source. */
            default:
                GDX_LLE_BAIL("unknown opcode=%u w0=%08X w1=%08X", (unsigned)op,
                             (unsigned)w0, (unsigned)w1);
        }
    }

    /* 64-byte OSTask, host-native little-endian. aspMain's boot reads exactly these four fields
     * (ucode text disasm: lw at +0x18/+0x1C and +0x30/+0x34); everything else is zero.
     *   +0x00 type            = 2 (M_AUDTASK)
     *   +0x18 ucode_data      = scratch offset of the data section
     *   +0x1C ucode_data_size = GDX_UCODE_DATA_BYTES (0x2E0)
     *   +0x30 data_ptr        = scratch offset of the rewritten command list
     *   +0x34 data_size       = dataSizeBytes */
    memset(ostask, 0, sizeof(ostask));
    ostask[0x00u / 4u] = 2u;
    ostask[0x18u / 4u] = sDataSecPhys;
    ostask[0x1Cu / 4u] = GDX_UCODE_DATA_BYTES;
    ostask[0x30u / 4u] = cmdListPhys;
    ostask[0x34u / 4u] = dataSizeBytes;

    /* Run one aspMain task to the task-complete BREAK. */
    gdx_rsp_lle_run_task(sUcodeText, GDX_UCODE_TEXT_BYTES,
                         sUcodeData, GDX_UCODE_DATA_BYTES,
                         ostask);

    /* If the ucode never reached BREAK (mis-marshalled or runaway task) the driver longjmp'd out
     * after its instruction cap. Copy-out is below, so no host buffer has been mutated yet:
     * discard the partial scratch output and fall back to the HLE for this tick rather than spin
     * the audio thread. */
    if (!gdx_rsp_lle_completed()) {
        if (gdx_unlock_audio_trace_dsp_active()) {
            gdx_unlock_diagf("[unlock-audio] dsp task selected=LLE outcome=fallback reason=watchdog cmds=%u bytes=%u\n",
                             (unsigned)count, (unsigned)dataSizeBytes);
            gdx_unlock_audio_trace_dsp_consume();
        }
        if (gdx_diag_verbose()) {
            gdx_lle_logf("[audio-lle] tick WATCHDOG-TIMEOUT: cmds=%u bytes=%u -> HLE fallback\n",
                         (unsigned)count, (unsigned)dataSizeBytes);
        }
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }

    /* Copy every RSP-written buffer back to its host home under the inverse (== same) transform.
     * The AI PCM especially must land back in host memory for the existing AI pipeline to play. */
    for (i = 0; i < (uint32_t)sMapCount; i++) {
        if (sMap[i].isOutput) {
            gdx_copy_out((void*)sMap[i].host, sMap[i].phys, sMap[i].size, sMap[i].xf);
        }
    }

    if (gdx_diag_verbose()) {
        gdx_lle_logf("[audio-lle] tick OK: cmds=%u bytes=%u maps=%d tickUsed=%X persistSlots=%d status=%08X\n",
                     (unsigned)count, (unsigned)dataSizeBytes, sMapCount,
                     (unsigned)(sTickBump - GDX_LLE_TICK_OFF), sPersistCount,
                     (unsigned)gdx_rsp_lle_status());
    }

    if (gdx_unlock_audio_trace_dsp_active()) {
        gdx_unlock_diagf("[unlock-audio] dsp task selected=LLE outcome=completed cmds=%u bytes=%u maps=%d compressedLoads=%u pcmLoads=%u\n",
                         (unsigned)count, (unsigned)dataSizeBytes, sMapCount,
                         (unsigned)compressedLoads, (unsigned)pcmLoads);
        gdx_unlock_audio_trace_dsp_consume();
    }

    #undef GDX_LLE_BAIL
}

/* Entry point. When LLE is enabled the bridge takes over; the bridge itself defers to
 * gdx_audio_hle_run on any trouble. */
void gdx_audio_lle_run(const void* dataPtr, unsigned int dataSizeBytes) {
    if (!gdx_audio_lle_enabled()) {
        if (gdx_unlock_audio_trace_dsp_active()) {
            gdx_unlock_diagf("[unlock-audio] dsp task selected=HLE outcome=completed bytes=%u\n",
                             dataSizeBytes);
            gdx_unlock_audio_trace_dsp_consume();
        }
        gdx_audio_hle_run(dataPtr, dataSizeBytes);
        return;
    }
    gdx_audio_lle_bridge_run(dataPtr, dataSizeBytes);
}
