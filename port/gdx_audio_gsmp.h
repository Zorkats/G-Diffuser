// port/gdx_audio_gsmp.h — Shared GSMP v1 container parser for the audio modding stages.
//
// Frozen container format ("GSMP", version 1, ALL little-endian, hand-packed — never a struct
// overlay; the sibling writer is tools/gen_sample_pack.py):
//
//   0   magic "GSMP" (4 bytes)
//   4   version u16 = 1
//   6   codec u16 = 0 (CODEC_ADPCM; anything else rejected)
//   8   reserved u32 = 0
//   12  encodedSize u32 (payload bytes; must be % 9 == 0)
//   16  decodedLength u32 (must == encodedSize/9*16)
//   20  loop.start u32
//   24  loop.end u32
//   28  loop.count u32
//   32  loop.predictorState s16[16] (32 bytes, ALWAYS present; meaningful iff count != 0)
//   64  book.order u32 (1..2)
//   68  book.npred u32 (1..8)
//   72  book.coefs s16[8*order*npred]
//   ..  crc32 u32 (IEEE 802.3 CRC-32 of the payload bytes)
//   ..  payload (encodedSize bytes)
//
// Header-only so stage 3 (gdx_audio_sample_packs.cpp, standalone audio/sample/<key> entries) and
// stage 4 (gdx_audio_soundfont_packs.cpp, GSMP blobs embedded in a GFT1 font container) run the
// exact same validation. gdxGsmpValidate is pure: no allocation, no mutation.

#ifndef GDX_AUDIO_GSMP_H
#define GDX_AUDIO_GSMP_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gdx_content_io.h" // gdx_content_crc32

// ── Mirror of the decomp Sample/AdpcmLoop/AdpcmBook layout ──────────────────────────────────────
// decomp/src/audio/disk/lib/audio.h:83-93. Port TUs stay out of decomp headers (CRT typedef
// collisions, same reason n64_sched.c carries a local AudioTask view); identical member types and
// order reproduce the layout, and these asserts pin it to the host ABI (x64 pointers).
struct GdxSampleLoopMirror;
struct GdxSampleBookMirror;

struct GdxSampleMirror {
    uint32_t unk0 : 1;
    uint32_t codec : 3;
    uint32_t medium : 2;
    uint32_t unk_bit26 : 1;
    uint32_t isRelocated : 1;
    uint32_t size : 24;
    uint8_t* sampleAddr;
    GdxSampleLoopMirror* loop;
    GdxSampleBookMirror* book;
};
static_assert(offsetof(GdxSampleMirror, sampleAddr) == 0x08, "Sample layout drift");
static_assert(offsetof(GdxSampleMirror, loop) == 0x10, "Sample layout drift");
static_assert(offsetof(GdxSampleMirror, book) == 0x18, "Sample layout drift");
static_assert(sizeof(GdxSampleMirror) == 0x20, "Sample layout drift");

// AdpcmLoop: header is 0x10; predictorState exists in memory iff count != 0.
struct GdxSampleLoopMirror {
    uint32_t start;
    uint32_t end;
    uint32_t count;
    uint32_t unk_0C;
    int16_t predictorState[16];
};
static_assert(sizeof(GdxSampleLoopMirror) == 0x30, "AdpcmLoop layout drift");

// AdpcmBook: header is 0x8, then 8*order*numPredictors s16 coefs.
struct GdxSampleBookMirror {
    int32_t order;
    int32_t numPredictors;
    int16_t book[];
};
static_assert(sizeof(GdxSampleBookMirror) == 0x8, "AdpcmBook layout drift");

constexpr uint32_t kCodecAdpcm = 0; // decomp audio.h CODEC_ADPCM
constexpr uint32_t kMediumRam = 0;  // decomp audio.h MEDIUM_RAM

inline uint16_t rdLE16(const uint8_t* p) {
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

inline uint32_t rdLE32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

enum RejectReason {
    kRejectArgs = 1,
    kRejectStockCodec = 2,
    kRejectTooSmall = 3,
    kRejectMagic = 4,
    kRejectVersion = 5,
    kRejectCodec = 6,
    kRejectReserved = 7,
    kRejectSizeProof = 8,
    kRejectFrameAlign = 9,
    kRejectDecodedLength = 10,
    kRejectBookOrder = 11,
    kRejectBookNpred = 12,
    kRejectContainerSize = 13,
    kRejectLoopRange = 14,
    kRejectLoopAlign = 15,
    kRejectCrc = 16,
    kRejectBudget = 17,
    kRejectArena = 18,
};

// Validated view into a GSMP v1 container; the pointers reference the container bytes.
struct GdxGsmpView {
    uint32_t encodedSize;
    uint32_t decodedLength;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t loopCount;
    uint32_t bookOrder;
    uint32_t bookNpred;
    size_t coefBytes;
    const uint8_t* predictorState; // 32 bytes at offset 32; meaningful iff loopCount != 0
    const uint8_t* coefs;          // coefBytes of s16 coefficients
    const uint8_t* payload;        // encodedSize ADPCM bytes
};

// Full GSMP v1 validation of the container at c[0,size). Returns 0 and fills *out on success,
// else one of RejectReason.
inline int gdxGsmpValidate(const uint8_t* c, size_t size, GdxGsmpView* out) {
    if (size < 72 + 4) {
        return kRejectTooSmall;
    }
    if (std::memcmp(c, "GSMP", 4) != 0) {
        return kRejectMagic;
    }
    if (rdLE16(c + 4) != 1) {
        return kRejectVersion;
    }
    if (rdLE16(c + 6) != kCodecAdpcm) {
        return kRejectCodec;
    }
    if (rdLE32(c + 8) != 0) {
        return kRejectReserved;
    }
    out->encodedSize = rdLE32(c + 12);
    out->decodedLength = rdLE32(c + 16);
    out->loopStart = rdLE32(c + 20);
    out->loopEnd = rdLE32(c + 24);
    out->loopCount = rdLE32(c + 28);
    out->bookOrder = rdLE32(c + 64);
    out->bookNpred = rdLE32(c + 68);

    if (out->encodedSize == 0 || (out->encodedSize % 9) != 0) {
        return kRejectFrameAlign;
    }
    if (out->decodedLength != (out->encodedSize / 9) * 16) {
        return kRejectDecodedLength;
    }
    if (out->bookOrder < 1 || out->bookOrder > 2) {
        return kRejectBookOrder;
    }
    if (out->bookNpred < 1 || out->bookNpred > 8) {
        return kRejectBookNpred;
    }
    out->coefBytes = (size_t) 16 * out->bookOrder * out->bookNpred; // s16[8*order*npred]
    // Exact-layout check, done before the coefs are read: header + coefs + crc + payload.
    if (size != 72 + out->coefBytes + 4 + out->encodedSize) {
        return kRejectContainerSize;
    }
    // Loop rules (mirrors gen_sample_pack.py's parse_gsmp): a looped sample (count != 0) needs
    // frame-aligned start < end <= decodedLength; a one-shot (count == 0) carries start/end 0.
    if (out->loopCount != 0) {
        if ((out->loopStart % 16) != 0) {
            return kRejectLoopAlign;
        }
        if (!(out->loopStart < out->loopEnd && out->loopEnd <= out->decodedLength)) {
            return kRejectLoopRange;
        }
    } else if (out->loopStart != 0 || out->loopEnd != 0) {
        return kRejectLoopRange;
    }
    out->predictorState = c + 32;
    out->coefs = c + 72;
    const uint8_t* crcAt = out->coefs + out->coefBytes;
    out->payload = crcAt + 4;
    if (rdLE32(crcAt) != gdx_content_crc32(out->payload, out->encodedSize)) {
        return kRejectCrc;
    }
    return 0;
}

#endif // GDX_AUDIO_GSMP_H
