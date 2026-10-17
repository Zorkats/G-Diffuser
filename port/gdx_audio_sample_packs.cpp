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
#include <set>
#include <string>
#include <unordered_map>

#include "port_log.h" // gdx_port_logf
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/File.h"
#include "ship/resource/archive/Archive.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include "gdx_workshop.h"    // GdxWorkshopModsDir: the mods/ containment rule GdxWorkshopReload uses
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

// Mounted-pack diagnostics. The game loads only "audio/sample/<key>" entries, so a pack that keeps
// raw WAVs at its root (or key-named entries outside that folder) mounts fine and applies nothing.
// These log once per pack, never once per sample probe.
bool gDiagScanned = false;
uint32_t gDiagEpoch = 0;
bool gOffHintLogged = false;
std::set<std::string> gWarnedPacks; // the four diagnostic globals are guarded by gProbeMutex

std::string asciiLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// Lower-case, with the "./" prefix that zip tools add to root-level entries removed.
std::string normalizeEntry(const std::string& raw) {
    std::string name = asciiLower(raw);
    while (name.rfind("./", 0) == 0) {
        name.erase(0, 2);
    }
    return name;
}

// A bare sample key ("<bank>__0x<offset>__<size>", no extension) in any folder.
bool looksLikeSampleKey(const std::string& name) {
    const size_t slash = name.rfind('/');
    const std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
    const size_t sizeSep = base.rfind("__");
    if (sizeSep == std::string::npos || sizeSep + 2 >= base.size() || base.find("__0x") == std::string::npos) {
        return false;
    }
    for (size_t i = sizeSep + 2; i < base.size(); ++i) {
        if (base[i] < '0' || base[i] > '9') {
            return false;
        }
    }
    return true;
}

struct PackEntryStats {
    int sampleEntries = 0; // entries under audio/sample/ that the game can load
    int strayWavs = 0;     // .wav entries; raw WAVs are never loaded, wherever they sit
    int strayKeys = 0;     // key-shaped entries outside audio/sample/ (converted, but misplaced)
    std::string example;   // first stray entry as stored in the archive, for the warning
};

// Walks the archive's in-memory file table (filled at mount; nothing is extracted).
PackEntryStats scanPackEntries(Ship::Archive& archive) {
    PackEntryStats stats;
    auto files = archive.ListFiles();
    if (files == nullptr) {
        return stats;
    }
    for (const auto& entry : *files) {
        const std::string name = normalizeEntry(entry.second);
        const bool isWav = name.size() > 4 && name.compare(name.size() - 4, 4, ".wav") == 0;
        if (!isWav && name.rfind("audio/sample/", 0) == 0) {
            ++stats.sampleEntries;
            continue;
        }
        if (isWav) {
            ++stats.strayWavs;
        } else if (looksLikeSampleKey(name)) {
            ++stats.strayKeys;
        } else {
            continue;
        }
        if (stats.example.empty()) {
            stats.example = entry.second;
        }
    }
    return stats;
}

// Runs once per pack epoch. Only archives under mods/ count, with the same containment test that
// GdxWorkshopReload uses, so stock archives never trigger a diagnostic.
void diagnoseMountedPacks(uint32_t epoch, bool channelOn) {
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        if (gDiagScanned && gDiagEpoch == epoch) {
            return;
        }
    }
    auto am = archiveManager();
    auto archives = (am != nullptr) ? am->GetArchives() : nullptr;
    if (archives == nullptr) {
        return;
    }
    const std::string modsKey = asciiLower(GdxWorkshopModsDir(false));
    int samplePacks = 0;
    for (const auto& ar : *archives) {
        if (ar == nullptr || modsKey.empty() || asciiLower(ar->GetPath()).find(modsKey) == std::string::npos) {
            continue;
        }
        const PackEntryStats stats = scanPackEntries(*ar);
        const int strays = stats.strayWavs + stats.strayKeys;
        if (stats.sampleEntries + strays == 0) {
            continue;
        }
        ++samplePacks;
        if (strays == 0) {
            continue;
        }
        bool firstWarning = false;
        {
            std::lock_guard<std::mutex> lock(gProbeMutex);
            firstWarning = gWarnedPacks.insert(ar->GetPath()).second;
        }
        if (firstWarning) {
            gdx_port_logf("[sample-pack] WARNING: %s has %d raw .wav or misplaced sample file(s) outside "
                          "audio/sample/ (first: %s). The game loads only converted audio/sample/<key> "
                          "entries, so these are ignored. Rebuild the pack from a folder of WAVs with "
                          "tools/gen_sample_pack.py.\n",
                          ar->GetPath().c_str(), strays, stats.example.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        gDiagScanned = true;
        gDiagEpoch = epoch;
    }
    if (samplePacks == 0 || channelOn) {
        return;
    }
    bool firstHint = false;
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        firstHint = !gOffHintLogged;
        gOffHintLogged = true;
    }
    if (firstHint) {
        gdx_port_logf("[sample-pack] %d sample pack(s) mounted but gEnhancements.Workshop.SamplePacks is off, "
                      "so none are applied. Turn on Workshop > Mods > Enable sample packs.\n",
                      samplePacks);
    }
}

} // namespace

void GdxSamplePackApply(int32_t fontId, int32_t bankId, uint32_t bankOffset, uint32_t stockSize, void* samplePtr) {
    // Diagnostics run ahead of the channel gate so an off channel still gets its one-shot hint.
    const bool channelOn = CVarGetInteger("gEnhancements.Workshop.SamplePacks", 0) != 0;
    diagnoseMountedPacks(GdxWorkshopPackEpoch(), channelOn);
    if (!channelOn) {
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
