// port/gdx_audio_sample_packs.cpp — Audio modding stage 3: per-sample replacement overlays.
//
// Pack key convention: "audio/sample/<bankName>__0x<bankOffset>__<stockSize>" — bankName from the
// 25-entry gSampleBankTable (decomp/src/audio/disk/audio_tables.c), bankOffset and stockSize from
// the stock sample's in-bank offset and byte size (torch's dumper emits the same key shape,
// torch/src/gdx/dump_audio.cpp sampleKey). The merged ArchiveManager is consulted directly (packs
// already mounted; last-wins follows the Workshop PackOrder), with a per-epoch probe cache keyed
// (bankId, bankOffset) so a "Reload packs" re-probes each sample exactly once.
//
// The frozen GSMP v1 container layout and its validation live in gdx_audio_gsmp.h (shared with
// the stage-4 soundfont packs, which embed GSMP blobs in their GFT1 containers).
//
// Applied overrides live in the persistent RDRAM arena (gdx_rdram_persist_alloc_raw) — the same
// lifetime class as the font conversions that reference them, and registered for the LLE low-32
// lookup. CRT-heap memory would break the LLE; never malloc here.

#include "gdx_audio_sample_packs.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/File.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include "n64_rdram.h"       // gdx_rdram_persist_alloc_raw (after <cstddef>, per the header's contract)
#include "gdx_audio_gsmp.h"  // GSMP container validation + Sample/Loop/Book mirrors (shared with stage 4)

// port/gdx_workshop.cpp; bumps on every GdxWorkshopReload.
extern "C" uint32_t GdxWorkshopPackEpoch(void);
// port/n64_sched.c; the gdx log shim the decomp TUs use.
extern "C" void gdx_ck(const char* s);
extern "C" void gdx_cki(const char* s, int v);

namespace {

constexpr size_t kArenaBudget = 1u << 20; // 1 MiB cumulative cap for all sample-pack arena bytes

// Index == SampleBankId (decomp/include/sfx.h, EXPANSION_KIT branch: SAMPLE_SOUND_EFFECTS=0 ..
// SAMPLE_DDBGM_EAD_DEMO=24). Names are the enum identifiers verbatim — that is the form the
// audio dump manifests emit (dump/audio/manifest.tsv) and tools/gen_sample_pack.py packs.
const char* const kBankNames[] = {
    "SAMPLE_SOUND_EFFECTS",     "SAMPLE_BGM",               "SAMPLE_GUITAR",
    "SAMPLE_DD_SOUND_EFFECTS",  "SAMPLE_DDBGM_MUTE_CITY",   "SAMPLE_DDBGM_SILENCE",
    "SAMPLE_DDBGM_SAND_OCEAN",  "SAMPLE_DDBGM_PORT_TOWN",   "SAMPLE_DDBGM_BIG_BLUE",
    "SAMPLE_DDBGM_DEVILS_FOREST", "SAMPLE_DDBGM_RED_CANYON", "SAMPLE_DDBGM_SECTOR",
    "SAMPLE_DDBGM_WHITE_LAND",  "SAMPLE_DDBGM_RAINBOW_ROAD", "SAMPLE_DDBGM_NEW_03",
    "SAMPLE_DDBGM_NEW_02",      "SAMPLE_DDBGM_NEW_01",      "SAMPLE_DDBGM_NEW_04",
    "SAMPLE_DDBGM_TITLE",       "SAMPLE_DDBGM_SELECT",      "SAMPLE_DDBGM_OPTION",
    "SAMPLE_DDBGM_DEATHRACE",   "SAMPLE_DDBGM_COURSE_EDITOR", "SAMPLE_DDBGM_MACHINE_EDITOR",
    "SAMPLE_DDBGM_EAD_DEMO",
};
constexpr int32_t kBankCount = sizeof(kBankNames) / sizeof(kBankNames[0]);

// Per-epoch "does any mounted pack provide audio/sample/<key>" probe, keyed (bankId, bankOffset).
struct SampleProbe {
    uint32_t epoch = 0;
    bool probed = false;
    bool found = false;
};

std::mutex gProbeMutex;
std::unordered_map<uint64_t, SampleProbe> gProbes;
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

std::string sampleKey(int32_t bankId, uint32_t bankOffset, uint32_t stockSize) {
    char tail[128];
    std::snprintf(tail, sizeof(tail), "%s__0x%X__%u", kBankNames[bankId], bankOffset, stockSize);
    return std::string("audio/sample/") + tail;
}

void logReject(int reason, int32_t bankId, uint32_t bankOffset) {
    gdx_cki("[sample-pack] reject reason", reason);
    gdx_cki("[sample-pack]  bankId", bankId);
    gdx_cki("[sample-pack]  bankOffset", (int) bankOffset);
}

} // namespace

void GdxSamplePackApply(int32_t fontId, int32_t bankId, uint32_t bankOffset, uint32_t stockSize, void* samplePtr) {
    if (CVarGetInteger("gEnhancements.Workshop.SamplePacks", 0) == 0) {
        return;
    }
    auto* sample = static_cast<GdxSampleMirror*>(samplePtr);
    if (sample == nullptr || bankId < 0 || bankId >= kBankCount || stockSize == 0) {
        return; // Device-addressed sample or nonsense args — not a pack matter, stay silent.
    }
    if (sample->codec != kCodecAdpcm) {
        logReject(kRejectStockCodec, bankId, bankOffset);
        return;
    }
    if (sample->size != stockSize) {
        logReject(kRejectSizeProof, bankId, bankOffset);
        return;
    }

    const uint64_t probeKey = ((uint64_t) (uint32_t) bankId << 32) | bankOffset;
    const uint32_t epoch = GdxWorkshopPackEpoch();
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        SampleProbe& probe = gProbes[probeKey];
        if (!probe.probed || probe.epoch != epoch) {
            auto am = archiveManager();
            probe.found = (am != nullptr) && am->HasFile(sampleKey(bankId, bankOffset, stockSize));
            probe.epoch = epoch;
            probe.probed = true;
        }
        if (!probe.found) {
            return;
        }
    }

    auto am = archiveManager();
    if (am == nullptr) {
        return;
    }
    auto file = am->LoadFile(sampleKey(bankId, bankOffset, stockSize));
    if (file == nullptr || file->Buffer == nullptr) {
        return;
    }
    // Archive backends over-allocate Buffer (+4096 guard); TrueSize is the real entry size.
    const size_t size = (file->TrueSize > 0) ? file->TrueSize : file->Buffer->size();
    if (size == 0 || file->BufferOffset + size > file->Buffer->size()) {
        return;
    }
    const uint8_t* c = reinterpret_cast<const uint8_t*>(file->Buffer->data()) + file->BufferOffset;

    // ── Validate the container fully BEFORE touching the sample (never half-apply) ──────────────
    GdxGsmpView gsmp;
    const int reject = gdxGsmpValidate(c, size, &gsmp);
    if (reject != 0) {
        logReject(reject, bankId, bankOffset);
        return;
    }

    // ── Arena budget, then allocations (still before any mutation) ──────────────────────────────
    const size_t sampleBytes = ((size_t) gsmp.encodedSize + 15u) & ~(size_t) 15u; // ALIGN16
    const size_t sampleBufBytes = sampleBytes + 16; // DMA paths over-read; keep RAM equally safe
    const size_t loopBytes = (gsmp.loopCount != 0) ? sizeof(GdxSampleLoopMirror) : 0x10;
    const size_t bookBytes = sizeof(GdxSampleBookMirror) + gsmp.coefBytes;
    const size_t totalBytes = sampleBufBytes + loopBytes + bookBytes;
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        if (gArenaUsed + totalBytes > kArenaBudget) {
            if (!gBudgetLogged) {
                gBudgetLogged = true;
                gdx_ck("[sample-pack] arena budget (1 MiB) exhausted; further overrides refused");
            }
            logReject(kRejectBudget, bankId, bankOffset);
            return;
        }
    }

    auto* sampleBuf = static_cast<uint8_t*>(gdx_rdram_persist_alloc_raw(sampleBufBytes, 16));
    auto* loopBuf = static_cast<GdxSampleLoopMirror*>(gdx_rdram_persist_alloc_raw(loopBytes, 16));
    auto* bookBuf = static_cast<GdxSampleBookMirror*>(gdx_rdram_persist_alloc_raw(bookBytes, 16));
    if (sampleBuf == nullptr || loopBuf == nullptr || bookBuf == nullptr) {
        logReject(kRejectArena, bankId, bankOffset);
        return;
    }

    std::memset(sampleBuf, 0, sampleBufBytes);
    std::memcpy(sampleBuf, gsmp.payload, gsmp.encodedSize);
    std::memset(loopBuf, 0, loopBytes);
    loopBuf->start = gsmp.loopStart;
    loopBuf->end = gsmp.loopEnd;
    loopBuf->count = gsmp.loopCount;
    if (gsmp.loopCount != 0) {
        std::memcpy(loopBuf->predictorState, gsmp.predictorState, sizeof(loopBuf->predictorState));
    }
    bookBuf->order = (int32_t) gsmp.bookOrder;
    bookBuf->numPredictors = (int32_t) gsmp.bookNpred;
    std::memcpy(bookBuf->book, gsmp.coefs, gsmp.coefBytes);

    // ── Commit: point the converted sample at the arena copies ──────────────────────────────────
    sample->size = gsmp.encodedSize;
    sample->sampleAddr = sampleBuf;
    sample->medium = kMediumRam; // synthesis reads the arena directly; no DmaSampleData
    sample->unk_bit26 = 0;       // stay out of the ROM->RAM preload collection
    sample->loop = loopBuf;
    sample->book = bookBuf;
    sample->isRelocated = 1;

    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        gArenaUsed += totalBytes;
    }
    gdx_cki("[sample-pack] applied fontId", fontId);
    gdx_cki("[sample-pack]  bankId", bankId);
    gdx_cki("[sample-pack]  bankOffset", (int) bankOffset);
}
