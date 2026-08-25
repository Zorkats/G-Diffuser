// port/gdx_audio_soundfont_packs.cpp — Audio modding stage 4: whole-soundfont overlays.
//
// Pack key convention: "audio/font/<FONTNAME>" — FONTNAME is the FontId enum identifier verbatim
// (decomp/include/sfx.h, EXPANSION_KIT branch: FONT_GUITAR=0 .. FONT_DDBGM_EAD_DEMO=22). The
// merged ArchiveManager is consulted directly (packs already mounted; last-wins follows the
// Workshop PackOrder), with a per-fontId per-epoch probe cache so a "Reload packs" re-probes each
// font exactly once (GdxSoundfontPackTick services the epoch change: re-apply present overlays,
// restore stashed stock pointers otherwise).
//
// Frozen container format ("GFT1", version 1, ALL little-endian, hand-packed — never a struct
// overlay; the sibling writer is tools/gen_soundfont_pack.py):
//
//   0    magic "GFT1" (4 bytes)
//   4    version u16 = 1
//   6    reserved u16 = 0
//   8    stockFontId u16            // must equal the fontId being converted
//   10   numInstruments u8          // must equal stock numInstruments (engine caps 126)
//   11   numDrums u8                // must equal stock (0 for all EK fonts)
//   12   numSfx u8                  // must equal stock (0 for all EK fonts)
//   13   numEnvelopes u8            // 1..64
//   14   numSamples u16             // 1..512
//   16   instruments[numInstruments] x 24 bytes:
//          0 sampleRefLow s16 (-1 absent; required valid iff rangeLo != 0)
//          2 sampleRefNormal s16 (-1 = empty slot)
//          4 sampleRefHigh s16 (-1 absent; required valid iff rangeHi != 0x7F)
//          6 envRef s16 (-1 = NULL envelope)
//          8 rangeLo u8
//          9 rangeHi u8
//          10 adsrDecayIndex u8
//          11 reserved u8 = 0
//          12 tuningLow f32
//          16 tuningNormal f32
//          20 tuningHigh f32
//   ..   drums[numDrums] x 12 bytes: { sampleRef s16 (required >=0), envRef s16 (or -1),
//          adsrDecayIndex u8, pan u8, reserved u16=0, tuning f32 }
//   ..   sfx[numSfx] x 8 bytes: { sampleRef s16 (or -1), reserved u16=0, tuning f32 }
//   ..   envelopes[numEnvelopes] x variable: { pointCount u8 (1..64), reserved u8=0, reserved
//          u16=0, points[pointCount] x { delay s16, arg s16 } }
//   ..   sampleTable[numSamples] x 8 bytes: { gsmpOffset u32 (absolute within container,
//          4-aligned), gsmpSize u32 }  // blob is a stage-3 GSMP v1 container
//   ..   gsmp blob area
//   ..   crc32 u32                  // gdx_content_crc32 of every preceding byte
//
// The container is validated in full BEFORE any arena allocation or mutation; a reject leaves the
// font's current pointers untouched (all-stock or all-overlay, never partial). Overlay graphs are
// built into the persistent RDRAM arena (gdx_rdram_persist_alloc_raw) — the same lifetime class as
// the stock font conversions; old graphs are never freed, so in-flight notes keep their snapshot
// tunedSample pointers until they end. CRT-heap memory would break the LLE; never malloc here.

#include "gdx_audio_soundfont_packs.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/File.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include "n64_rdram.h"      // gdx_rdram_persist_alloc_raw (after <cstddef>, per the header's contract)
#include "gdx_audio_gsmp.h" // GSMP blob validation + Sample/Loop/Book mirrors (shared with stage 3)

// port/gdx_workshop.cpp; bumps on every GdxWorkshopReload.
extern "C" uint32_t GdxWorkshopPackEpoch(void);
// port/n64_sched.c; the gdx log shim the decomp TUs use.
extern "C" void gdx_ck(const char* s);
extern "C" void gdx_cki(const char* s, int v);

namespace {

// ── Mirror of the decomp SoundFont/Instrument/Drum/SoundEffect/TunedSample/EnvelopePoint layout ─
// decomp/src/audio/disk/lib/audio.h:52-135 (Sample/AdpcmLoop/AdpcmBook come from gdx_audio_gsmp.h).
// Same reasoning as stage 3: identical member types and order reproduce the layout on the host
// ABI (x64 pointers), pinned by these asserts.
struct GdxEnvelopePointMirror {
    int16_t delay;
    int16_t arg;
};
static_assert(sizeof(GdxEnvelopePointMirror) == 0x4, "EnvelopePoint layout drift");

struct GdxTunedSampleMirror {
    GdxSampleMirror* sample;
    float tuning;
};
static_assert(offsetof(GdxTunedSampleMirror, tuning) == 0x8, "TunedSample layout drift");
static_assert(sizeof(GdxTunedSampleMirror) == 0x10, "TunedSample layout drift");

struct GdxInstrumentMirror {
    uint8_t isRelocated;
    uint8_t normalRangeLo;
    uint8_t normalRangeHi;
    uint8_t adsrDecayIndex;
    GdxEnvelopePointMirror* envelope;
    GdxTunedSampleMirror lowPitchTunedSample;
    GdxTunedSampleMirror normalPitchTunedSample;
    GdxTunedSampleMirror highPitchTunedSample;
};
static_assert(offsetof(GdxInstrumentMirror, envelope) == 0x08, "Instrument layout drift");
static_assert(offsetof(GdxInstrumentMirror, lowPitchTunedSample) == 0x10, "Instrument layout drift");
static_assert(offsetof(GdxInstrumentMirror, normalPitchTunedSample) == 0x20, "Instrument layout drift");
static_assert(offsetof(GdxInstrumentMirror, highPitchTunedSample) == 0x30, "Instrument layout drift");
static_assert(sizeof(GdxInstrumentMirror) == 0x40, "Instrument layout drift");

struct GdxDrumMirror {
    uint8_t adsrDecayIndex;
    uint8_t pan;
    uint8_t isRelocated;
    GdxTunedSampleMirror tunedSample;
    GdxEnvelopePointMirror* envelope;
};
static_assert(offsetof(GdxDrumMirror, tunedSample) == 0x08, "Drum layout drift");
static_assert(offsetof(GdxDrumMirror, envelope) == 0x18, "Drum layout drift");
static_assert(sizeof(GdxDrumMirror) == 0x20, "Drum layout drift");

struct GdxSoundEffectMirror {
    GdxTunedSampleMirror tunedSample;
};
static_assert(sizeof(GdxSoundEffectMirror) == 0x10, "SoundEffect layout drift");

struct GdxSoundFontMirror {
    uint8_t numInstruments;
    uint8_t numDrums;
    uint8_t sampleBankId1;
    uint8_t sampleBankId2;
    uint16_t numSfx;
    GdxInstrumentMirror** instruments;
    GdxDrumMirror** drums;
    GdxSoundEffectMirror* soundEffects;
};
static_assert(offsetof(GdxSoundFontMirror, instruments) == 0x08, "SoundFont layout drift");
static_assert(offsetof(GdxSoundFontMirror, drums) == 0x10, "SoundFont layout drift");
static_assert(offsetof(GdxSoundFontMirror, soundEffects) == 0x18, "SoundFont layout drift");
static_assert(sizeof(GdxSoundFontMirror) == 0x20, "SoundFont layout drift");

constexpr size_t kArenaBudget = 8u << 20;       // 8 MiB cumulative cap, separate from stage 3's 1 MiB
constexpr uint32_t kMaxSampleDecoded = 8u << 20; // per-sample decodedLength cap
constexpr uint32_t kMaxInstruments = 126;        // engine cap (gdx_audio_convert_font)
constexpr uint32_t kMaxEnvelopes = 64;
constexpr uint32_t kMaxSamples = 512;

// Envelope delay sentinels (decomp audio.h:1039-1042).
constexpr int16_t kAdsrDisable = 0;
constexpr int16_t kAdsrHang = -1;
constexpr int16_t kAdsrGoto = -2;
constexpr int16_t kAdsrRestart = -3;

// Index == FontId (decomp/include/sfx.h, EXPANSION_KIT branch: FONT_GUITAR=0 ..
// FONT_DDBGM_EAD_DEMO=22). Names are the enum identifiers verbatim — that is the form the audio
// dump manifests emit (dump/audio/fonts/ek_FONT_*.json) and tools/gen_soundfont_pack.py packs.
const char* const kFontNames[] = {
    "FONT_GUITAR",          "FONT_SOUND_EFFECTS",   "FONT_DDBGM_MUTE_CITY", "FONT_DDBGM_SILENCE",
    "FONT_DDBGM_SAND_OCEAN", "FONT_DDBGM_PORT_TOWN", "FONT_DDBGM_BIG_BLUE", "FONT_DDBGM_DEVILS_FOREST",
    "FONT_DDBGM_RED_CANYON", "FONT_DDBGM_SECTOR",   "FONT_DDBGM_WHITE_LAND", "FONT_DDBGM_RAINBOW_ROAD",
    "FONT_DDBGM_NEW_03",    "FONT_DDBGM_NEW_02",    "FONT_DDBGM_NEW_01",    "FONT_DDBGM_NEW_04",
    "FONT_DDBGM_TITLE",     "FONT_DDBGM_SELECT",    "FONT_DDBGM_OPTION",    "FONT_DDBGM_DEATHRACE",
    "FONT_DDBGM_COURSE_EDITOR", "FONT_DDBGM_MACHINE_EDITOR", "FONT_DDBGM_EAD_DEMO",
};
constexpr int32_t kFontCount = sizeof(kFontNames) / sizeof(kFontNames[0]);

// Per-epoch "does any mounted pack provide audio/font/<NAME>" probe, one per fontId.
struct FontProbe {
    uint32_t epoch = 0;
    bool probed = false;
    bool found = false;
};

struct FontState {
    bool seen = false;
    GdxSoundFontMirror* font = nullptr; // stable: &gAudioCtx.soundFontList[fontId]
    GdxInstrumentMirror** stockInstruments = nullptr;
    GdxDrumMirror** stockDrums = nullptr;
    GdxSoundEffectMirror* stockSfx = nullptr;
    FontProbe probe;
};

std::mutex gMutex;
FontState gFonts[kFontCount];
uint32_t gLastTickEpoch = 0;
size_t gArenaUsed = 0;
bool gBudgetLogged = false;

std::shared_ptr<Ship::ArchiveManager> archiveManager() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return nullptr;
    }
    return rm->GetArchiveManager();
}

std::string fontKey(int32_t fontId) {
    return std::string("audio/font/") + kFontNames[fontId];
}

int16_t rdLE16s(const uint8_t* p) {
    return (int16_t) rdLE16(p);
}

float rdLEf32(const uint8_t* p) {
    const uint32_t w = rdLE32(p);
    float f;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}

void logReject(int reason, int32_t fontId) {
    gdx_cki("[soundfont-pack] reject reason", reason);
    gdx_cki("[soundfont-pack]  fontId", fontId);
}

enum SfRejectReason {
    kSfRejectTooSmall = 1,
    kSfRejectMagic = 2,
    kSfRejectVersion = 3,
    kSfRejectReserved = 4,
    kSfRejectFontId = 5,
    kSfRejectCounts = 6,
    kSfRejectSizeProof = 7,
    kSfRejectBlobLayout = 8,
    kSfRejectSampleRef = 9,
    kSfRejectEnvRef = 10,
    kSfRejectEnvelope = 11,
    kSfRejectGsmp = 12,
    kSfRejectSampleLength = 13,
    kSfRejectCrc = 14,
    kSfRejectBudget = 15,
    kSfRejectArena = 16,
};

// Parsed, fully validated view of a GFT1 container; pointers reference the container bytes.
struct GftParse {
    uint32_t numInstruments;
    uint32_t numDrums;
    uint32_t numSfx;
    uint32_t numEnvelopes;
    uint32_t numSamples;
    const uint8_t* instruments; // numInstruments x 24 bytes
    const uint8_t* drums;       // numDrums x 12 bytes
    const uint8_t* sfx;         // numSfx x 8 bytes
    const uint8_t* envRec[kMaxEnvelopes]; // record start (pointCount u8 at [0])
    uint32_t envPoints[kMaxEnvelopes];
    uint32_t sampleOffset[kMaxSamples]; // absolute container offset of each GSMP blob
    uint32_t sampleSize[kMaxSamples];
};

bool sampleRefOk(int16_t ref, uint32_t numSamples) {
    return ref == -1 || (ref >= 0 && (uint32_t) ref < numSamples);
}

bool envRefOk(int16_t ref, uint32_t numEnvelopes) {
    return ref == -1 || (ref >= 0 && (uint32_t) ref < numEnvelopes);
}

// Full GFT1 v1 validation of the container at c[0,size) against the stock font counts. Returns 0
// and fills *out on success, else one of SfRejectReason. Pure: no allocation, no mutation.
int gft1Validate(const uint8_t* c, size_t size, int32_t fontId, const GdxSoundFontMirror* stock, GftParse* out) {
    if (size < 24) {
        return kSfRejectTooSmall;
    }
    if (std::memcmp(c, "GFT1", 4) != 0) {
        return kSfRejectMagic;
    }
    if (rdLE16(c + 4) != 1) {
        return kSfRejectVersion;
    }
    if (rdLE16(c + 6) != 0) {
        return kSfRejectReserved;
    }
    if (rdLE16(c + 8) != (uint32_t) fontId) {
        return kSfRejectFontId;
    }
    const uint32_t ni = c[10];
    const uint32_t nd = c[11];
    const uint32_t nsfx = c[12];
    const uint32_t nenv = c[13];
    const uint32_t nsmp = rdLE16(c + 14);
    if (ni != stock->numInstruments || nd != stock->numDrums || nsfx != stock->numSfx) {
        return kSfRejectCounts;
    }
    if (ni > kMaxInstruments || nenv < 1 || nenv > kMaxEnvelopes || nsmp < 1 || nsmp > kMaxSamples) {
        return kSfRejectCounts;
    }

    // ── Exact-size proof: walk the variable records with a bound check before every read ────────
    size_t p = 16;
    if (p + 24u * ni > size) {
        return kSfRejectSizeProof;
    }
    out->instruments = c + p;
    p += 24u * ni;
    if (p + 12u * nd > size) {
        return kSfRejectSizeProof;
    }
    out->drums = c + p;
    p += 12u * nd;
    if (p + 8u * nsfx > size) {
        return kSfRejectSizeProof;
    }
    out->sfx = c + p;
    p += 8u * nsfx;
    for (uint32_t e = 0; e < nenv; e++) {
        if (p + 4 > size) {
            return kSfRejectSizeProof;
        }
        const uint8_t* rec = c + p;
        const uint32_t points = rec[0];
        if (points < 1 || points > kMaxEnvelopes || rec[1] != 0 || rec[2] != 0 || rec[3] != 0) {
            return kSfRejectEnvelope;
        }
        if (p + 4 + 4u * points > size) {
            return kSfRejectSizeProof;
        }
        // Envelope semantics: delays are >0 or one of the four sentinels; an envelope that never
        // terminates/hangs/jumps within pointCount is rejected, not clamped.
        bool terminates = false;
        for (uint32_t i = 0; i < points; i++) {
            const int16_t delay = rdLE16s(rec + 4 + i * 4);
            const int16_t arg = rdLE16s(rec + 6 + i * 4);
            if (delay > 0) {
                continue;
            }
            if (delay != kAdsrDisable && delay != kAdsrHang && delay != kAdsrGoto && delay != kAdsrRestart) {
                return kSfRejectEnvelope;
            }
            if (delay == kAdsrGoto && (arg < 0 || (uint32_t) arg >= points)) {
                return kSfRejectEnvelope;
            }
            terminates = true;
        }
        if (!terminates) {
            return kSfRejectEnvelope;
        }
        out->envRec[e] = rec;
        out->envPoints[e] = points;
        p += 4 + 4u * points;
    }
    if (p + 8u * nsmp > size) {
        return kSfRejectSizeProof;
    }
    const uint8_t* sampleTable = c + p;
    p += 8u * nsmp;
    // Blob area runs to the trailing crc32; every blob must sit 4-aligned inside it.
    if (size < 4 || p > size - 4) {
        return kSfRejectSizeProof;
    }
    const size_t blobEnd = size - 4;
    for (uint32_t i = 0; i < nsmp; i++) {
        const uint32_t off = rdLE32(sampleTable + i * 8);
        const uint32_t sz = rdLE32(sampleTable + i * 8 + 4);
        if ((off & 3) != 0 || off < p || sz == 0 || (size_t) off + sz > blobEnd) {
            return kSfRejectBlobLayout;
        }
        out->sampleOffset[i] = off;
        out->sampleSize[i] = sz;
    }
    // Non-overlap: sort-and-scan over offsets.
    uint16_t order[kMaxSamples];
    for (uint32_t i = 0; i < nsmp; i++) {
        order[i] = (uint16_t) i;
    }
    for (uint32_t i = 1; i < nsmp; i++) {
        const uint16_t v = order[i];
        uint32_t j = i;
        while (j > 0 && out->sampleOffset[order[j - 1]] > out->sampleOffset[v]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    for (uint32_t i = 1; i < nsmp; i++) {
        const uint32_t prev = order[i - 1];
        if (out->sampleOffset[order[i]] < out->sampleOffset[prev] + out->sampleSize[prev]) {
            return kSfRejectBlobLayout;
        }
    }

    // ── Graph completeness (mirrors the gdx_audio_convert_font range rules) ─────────────────────
    for (uint32_t i = 0; i < ni; i++) {
        const uint8_t* r = out->instruments + i * 24;
        const int16_t refLo = rdLE16s(r + 0);
        const int16_t refNo = rdLE16s(r + 2);
        const int16_t refHi = rdLE16s(r + 4);
        const int16_t envRef = rdLE16s(r + 6);
        if (r[11] != 0) {
            return kSfRejectReserved;
        }
        if (refNo == -1) {
            if (refLo != -1 || refHi != -1) {
                return kSfRejectSampleRef; // an empty slot carries no pitch splits
            }
        } else if (!sampleRefOk(refNo, nsmp)) {
            return kSfRejectSampleRef;
        }
        if (!sampleRefOk(refLo, nsmp) || !sampleRefOk(refHi, nsmp)) {
            return kSfRejectSampleRef;
        }
        if ((r[8] != 0 && refLo == -1) || (r[9] != 0x7F && refHi == -1)) {
            return kSfRejectSampleRef;
        }
        if (!envRefOk(envRef, nenv)) {
            return kSfRejectEnvRef;
        }
    }
    for (uint32_t i = 0; i < nd; i++) {
        const uint8_t* r = out->drums + i * 12;
        if (!sampleRefOk(rdLE16s(r + 0), nsmp) || rdLE16s(r + 0) == -1) {
            return kSfRejectSampleRef; // drum sampleRef is required
        }
        if (r[6] != 0 || r[7] != 0) {
            return kSfRejectReserved;
        }
        if (!envRefOk(rdLE16s(r + 2), nenv)) {
            return kSfRejectEnvRef;
        }
    }
    for (uint32_t i = 0; i < nsfx; i++) {
        const uint8_t* r = out->sfx + i * 8;
        if (!sampleRefOk(rdLE16s(r + 0), nsmp)) {
            return kSfRejectSampleRef;
        }
        if (r[2] != 0 || r[3] != 0) {
            return kSfRejectReserved;
        }
    }

    // ── Embedded GSMP blobs: full stage-3 validation plus the per-sample length cap ─────────────
    for (uint32_t i = 0; i < nsmp; i++) {
        GdxGsmpView v;
        const int sub = gdxGsmpValidate(c + out->sampleOffset[i], out->sampleSize[i], &v);
        if (sub != 0) {
            gdx_cki("[soundfont-pack]  gsmp subreason", sub);
            return kSfRejectGsmp;
        }
        if (v.decodedLength > kMaxSampleDecoded) {
            return kSfRejectSampleLength;
        }
    }

    if (rdLE32(c + size - 4) != gdx_content_crc32(c, size - 4)) {
        return kSfRejectCrc;
    }

    out->numInstruments = ni;
    out->numDrums = nd;
    out->numSfx = nsfx;
    out->numEnvelopes = nenv;
    out->numSamples = nsmp;
    return 0;
}

// Loads, validates, arena-builds and commits the overlay for one font. Every failure leaves the
// font's current pointers untouched.
void applyOverlay(int32_t fontId, GdxSoundFontMirror* font) {
    auto am = archiveManager();
    if (am == nullptr) {
        return;
    }
    auto file = am->LoadFile(fontKey(fontId));
    if (file == nullptr || file->Buffer == nullptr) {
        return;
    }
    // Archive backends over-allocate Buffer (+4096 guard); TrueSize is the real entry size.
    const size_t size = (file->TrueSize > 0) ? file->TrueSize : file->Buffer->size();
    if (size == 0 || file->BufferOffset + size > file->Buffer->size()) {
        return;
    }
    const uint8_t* c = reinterpret_cast<const uint8_t*>(file->Buffer->data()) + file->BufferOffset;

    static GftParse sParse; // large; audio thread only
    const int reject = gft1Validate(c, size, fontId, font, &sParse);
    if (reject != 0) {
        logReject(reject, fontId);
        return;
    }
    const uint32_t ni = sParse.numInstruments;
    const uint32_t nd = sParse.numDrums;
    const uint32_t nsfx = sParse.numSfx;
    const uint32_t nenv = sParse.numEnvelopes;
    const uint32_t nsmp = sParse.numSamples;

    // ── Arena budget, then allocations (still before any mutation) ──────────────────────────────
    size_t totalPoints = 0;
    for (uint32_t e = 0; e < nenv; e++) {
        totalPoints += sParse.envPoints[e];
    }
    const size_t instPtrsBytes = ni * sizeof(GdxInstrumentMirror*);
    const size_t instsBytes = ni * sizeof(GdxInstrumentMirror);
    const size_t drumPtrsBytes = nd * sizeof(GdxDrumMirror*);
    const size_t drumsBytes = nd * sizeof(GdxDrumMirror);
    const size_t sfxBytes = nsfx * sizeof(GdxSoundEffectMirror);
    const size_t envBytes = totalPoints * sizeof(GdxEnvelopePointMirror);
    const size_t mirrorsBytes = nsmp * sizeof(GdxSampleMirror);
    size_t sampleBytes = 0;
    for (uint32_t i = 0; i < nsmp; i++) {
        GdxGsmpView v;
        gdxGsmpValidate(c + sParse.sampleOffset[i], sParse.sampleSize[i], &v); // already validated
        const size_t bufBytes = (((size_t) v.encodedSize + 15u) & ~(size_t) 15u) + 16;
        const size_t loopBytes = (v.loopCount != 0) ? sizeof(GdxSampleLoopMirror) : 0x10;
        sampleBytes += bufBytes + loopBytes + sizeof(GdxSampleBookMirror) + v.coefBytes;
    }
    const size_t totalBytes =
        instPtrsBytes + instsBytes + drumPtrsBytes + drumsBytes + sfxBytes + envBytes + mirrorsBytes + sampleBytes;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gArenaUsed + totalBytes > kArenaBudget) {
            if (!gBudgetLogged) {
                gBudgetLogged = true;
                gdx_ck("[soundfont-pack] arena budget (8 MiB) exhausted; further overlays refused");
            }
            logReject(kSfRejectBudget, fontId);
            return;
        }
    }

    auto alloc = [](size_t bytes) {
        return (bytes > 0) ? gdx_rdram_persist_alloc_raw(bytes, 16) : nullptr;
    };
    auto* instPtrs = static_cast<GdxInstrumentMirror**>(alloc(instPtrsBytes));
    auto* insts = static_cast<GdxInstrumentMirror*>(alloc(instsBytes));
    auto* drumPtrs = static_cast<GdxDrumMirror**>(alloc(drumPtrsBytes));
    auto* drums = static_cast<GdxDrumMirror*>(alloc(drumsBytes));
    auto* sfxArr = static_cast<GdxSoundEffectMirror*>(alloc(sfxBytes));
    auto* envBuf = static_cast<GdxEnvelopePointMirror*>(alloc(envBytes));
    auto* mirrors = static_cast<GdxSampleMirror*>(alloc(mirrorsBytes));
    if ((instPtrsBytes > 0 && (instPtrs == nullptr || insts == nullptr)) ||
        (drumPtrsBytes > 0 && (drumPtrs == nullptr || drums == nullptr)) ||
        (sfxBytes > 0 && sfxArr == nullptr) || envBuf == nullptr || mirrors == nullptr) {
        logReject(kSfRejectArena, fontId);
        return;
    }
    if (instsBytes > 0) {
        std::memset(insts, 0, instsBytes);
    }
    if (drumsBytes > 0) {
        std::memset(drums, 0, drumsBytes);
    }
    if (sfxBytes > 0) {
        std::memset(sfxArr, 0, sfxBytes);
    }
    std::memset(mirrors, 0, mirrorsBytes);

    // Envelopes: verbatim point copies, one contiguous run per envelope record.
    GdxEnvelopePointMirror* envArena[kMaxEnvelopes];
    GdxEnvelopePointMirror* envCursor = envBuf;
    for (uint32_t e = 0; e < nenv; e++) {
        envArena[e] = envCursor;
        std::memcpy(envCursor, sParse.envRec[e] + 4, sParse.envPoints[e] * sizeof(GdxEnvelopePointMirror));
        envCursor += sParse.envPoints[e];
    }

    // Samples: same arena build as stage 3 — ADPCM payload copy, loop, book, RAM-medium mirror.
    for (uint32_t i = 0; i < nsmp; i++) {
        GdxGsmpView v;
        gdxGsmpValidate(c + sParse.sampleOffset[i], sParse.sampleSize[i], &v); // already validated
        const size_t bufBytes = (((size_t) v.encodedSize + 15u) & ~(size_t) 15u) + 16;
        const size_t loopBytes = (v.loopCount != 0) ? sizeof(GdxSampleLoopMirror) : 0x10;
        const size_t bookBytes = sizeof(GdxSampleBookMirror) + v.coefBytes;
        auto* sampleBuf = static_cast<uint8_t*>(gdx_rdram_persist_alloc_raw(bufBytes, 16));
        auto* loopBuf = static_cast<GdxSampleLoopMirror*>(gdx_rdram_persist_alloc_raw(loopBytes, 16));
        auto* bookBuf = static_cast<GdxSampleBookMirror*>(gdx_rdram_persist_alloc_raw(bookBytes, 16));
        if (sampleBuf == nullptr || loopBuf == nullptr || bookBuf == nullptr) {
            logReject(kSfRejectArena, fontId);
            return;
        }
        std::memset(sampleBuf, 0, bufBytes);
        std::memcpy(sampleBuf, v.payload, v.encodedSize);
        std::memset(loopBuf, 0, loopBytes);
        loopBuf->start = v.loopStart;
        loopBuf->end = v.loopEnd;
        loopBuf->count = v.loopCount;
        if (v.loopCount != 0) {
            std::memcpy(loopBuf->predictorState, v.predictorState, sizeof(loopBuf->predictorState));
        }
        bookBuf->order = (int32_t) v.bookOrder;
        bookBuf->numPredictors = (int32_t) v.bookNpred;
        std::memcpy(bookBuf->book, v.coefs, v.coefBytes);

        GdxSampleMirror* m = &mirrors[i];
        m->codec = kCodecAdpcm;
        m->medium = kMediumRam; // synthesis reads the arena directly; no DmaSampleData
        m->unk_bit26 = 0;
        m->isRelocated = 1;
        m->size = v.encodedSize;
        m->sampleAddr = sampleBuf;
        m->loop = loopBuf;
        m->book = bookBuf;
    }

    // Instruments: -1 normal sampleRef leaves the pointer slot empty (stock's instOffset == 0).
    for (uint32_t i = 0; i < ni; i++) {
        const uint8_t* r = sParse.instruments + i * 24;
        const int16_t refLo = rdLE16s(r + 0);
        const int16_t refNo = rdLE16s(r + 2);
        const int16_t refHi = rdLE16s(r + 4);
        const int16_t envRef = rdLE16s(r + 6);
        if (refNo == -1) {
            instPtrs[i] = nullptr;
            continue;
        }
        GdxInstrumentMirror* inst = &insts[i];
        inst->isRelocated = 1;
        inst->normalRangeLo = r[8];
        inst->normalRangeHi = r[9];
        inst->adsrDecayIndex = r[10];
        inst->envelope = (envRef >= 0) ? envArena[envRef] : nullptr;
        if (refLo >= 0) {
            inst->lowPitchTunedSample.sample = &mirrors[refLo];
            inst->lowPitchTunedSample.tuning = rdLEf32(r + 12);
        }
        inst->normalPitchTunedSample.sample = &mirrors[refNo];
        inst->normalPitchTunedSample.tuning = rdLEf32(r + 16);
        if (refHi >= 0) {
            inst->highPitchTunedSample.sample = &mirrors[refHi];
            inst->highPitchTunedSample.tuning = rdLEf32(r + 20);
        }
        instPtrs[i] = inst;
    }
    for (uint32_t i = 0; i < nd; i++) {
        const uint8_t* r = sParse.drums + i * 12;
        const int16_t envRef = rdLE16s(r + 2);
        GdxDrumMirror* drum = &drums[i];
        drum->adsrDecayIndex = r[4];
        drum->pan = r[5];
        drum->isRelocated = 1;
        drum->tunedSample.sample = &mirrors[rdLE16s(r + 0)];
        drum->tunedSample.tuning = rdLEf32(r + 8);
        drum->envelope = (envRef >= 0) ? envArena[envRef] : nullptr;
        drumPtrs[i] = drum;
    }
    for (uint32_t i = 0; i < nsfx; i++) {
        const uint8_t* r = sParse.sfx + i * 8;
        const int16_t ref = rdLE16s(r + 0);
        if (ref >= 0) {
            sfxArr[i].tunedSample.sample = &mirrors[ref];
            sfxArr[i].tunedSample.tuning = rdLEf32(r + 4);
        }
    }

    // ── Commit: swap the converted font's graph pointers for the arena copies ───────────────────
    font->instruments = instPtrs;
    font->drums = drumPtrs;
    font->soundEffects = sfxArr;

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gArenaUsed += totalBytes;
    }
    gdx_cki("[soundfont-pack] applied fontId", fontId);
    gdx_cki("[soundfont-pack]  numSamples", (int) nsmp);
}

} // namespace

extern "C" int gdx_soundfont_packs_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.SoundfontPacks", 0) != 0;
}

extern "C" void GdxSoundfontPackApply(int32_t fontId, void* soundFont) {
    if (!gdx_soundfont_packs_enabled()) {
        return;
    }
    if (fontId < 0 || fontId >= kFontCount || soundFont == nullptr) {
        return; // nonsense args — not a pack matter, stay silent.
    }
    auto* font = static_cast<GdxSoundFontMirror*>(soundFont);
    const uint32_t epoch = GdxWorkshopPackEpoch();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        FontState& st = gFonts[fontId];
        if (!st.seen) {
            // First apply for this font: the just-published pointers are the stock graph; stash
            // them so a later reload with no overlay can restore them.
            st.seen = true;
            st.font = font;
            st.stockInstruments = font->instruments;
            st.stockDrums = font->drums;
            st.stockSfx = font->soundEffects;
        }
        FontProbe& probe = st.probe;
        if (!probe.probed || probe.epoch != epoch) {
            auto am = archiveManager();
            probe.found = (am != nullptr) && am->HasFile(fontKey(fontId));
            probe.epoch = epoch;
            probe.probed = true;
        }
        if (!probe.found) {
            return;
        }
    }
    applyOverlay(fontId, font);
}

extern "C" void GdxSoundfontPackTick(void) {
    const uint32_t epoch = GdxWorkshopPackEpoch();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (epoch == gLastTickEpoch) {
            return;
        }
        gLastTickEpoch = epoch;
    }
    const bool enabled = gdx_soundfont_packs_enabled() != 0;
    for (int32_t fontId = 0; fontId < kFontCount; fontId++) {
        FontState& st = gFonts[fontId];
        bool found;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!st.seen) {
                continue;
            }
            FontProbe& probe = st.probe;
            if (!probe.probed || probe.epoch != epoch) {
                auto am = archiveManager();
                probe.found = (am != nullptr) && am->HasFile(fontKey(fontId));
                probe.epoch = epoch;
                probe.probed = true;
            }
            found = probe.found;
        }
        if (found && enabled) {
            applyOverlay(fontId, st.font);
        } else if (st.font->instruments != st.stockInstruments || st.font->drums != st.stockDrums ||
                   st.font->soundEffects != st.stockSfx) {
            // Overlay gone (or switch off) after a reload: put the stock graph back.
            st.font->instruments = st.stockInstruments;
            st.font->drums = st.stockDrums;
            st.font->soundEffects = st.stockSfx;
            gdx_cki("[soundfont-pack] restored stock fontId", fontId);
        }
    }
}
