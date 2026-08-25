// port/gdx_audio_seq_packs.cpp — Audio modding stage 2: sequence replacement overlays.
//
// Pack key convention: "audio/seq/<name>" holds raw native aseq bytes (no container, no header),
// one entry per sequence the pack overrides. The merged ArchiveManager is consulted directly
// (packs are already mounted; last-wins follows the Workshop PackOrder), keyed by the workshop
// pack epoch so a "Reload packs" re-probes each sequence exactly once.

#include "gdx_audio_seq_packs.h"

#include "port_log.h" // gdx_port_logf
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/File.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

// port/gdx_workshop.cpp; bumps on every GdxWorkshopReload.
extern "C" uint32_t GdxWorkshopPackEpoch(void);

namespace {

// Index == SeqId (decomp/include/sfx.h, EXPANSION_KIT branch: SEQ_GUITAR=0 ..
// SEQ_DDBGM_EAD_DEMO=22). tools/gen_sequence_pack.py carries the same list; keep them in sync.
const char* const kSeqNames[] = {
    "guitar",         "sound_effects",       "ddbgm_mute_city",    "ddbgm_silence",
    "ddbgm_sand_ocean", "ddbgm_port_town",   "ddbgm_big_blue",     "ddbgm_devils_forest",
    "ddbgm_red_canyon", "ddbgm_sector",      "ddbgm_white_land",   "ddbgm_rainbow_road",
    "ddbgm_new_03",     "ddbgm_new_02",      "ddbgm_new_01",       "ddbgm_new_04",
    "ddbgm_title",      "ddbgm_select",      "ddbgm_option",       "ddbgm_deathrace",
    "ddbgm_course_editor", "ddbgm_machine_editor", "ddbgm_ead_demo",
};
constexpr int32_t kSeqCount = sizeof(kSeqNames) / sizeof(kSeqNames[0]);

// Return the SeqId/FontId index for a trimmed font reference string, or -1 if unknown.
// Accepts either the short name ("ddbgm_mute_city") or the enum name ("FONT_DDBGM_MUTE_CITY").
int fontIdFromString(const std::string& s) {
    if (s.empty()) {
        return -1;
    }
    std::string key = s;
    {
        size_t pos = 0;
        while (pos < key.size() && std::isspace(static_cast<unsigned char>(key[pos]))) {
            pos++;
        }
        key.erase(0, pos);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
            key.pop_back();
        }
    }
    if (key.compare(0, 5, "FONT_") == 0) {
        key.erase(0, 5);
    }
    for (char& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (int i = 0; i < kSeqCount; i++) {
        if (key == kSeqNames[i]) {
            return i;
        }
    }
    return -1;
}

// Per-epoch "does any mounted pack provide audio/seq/<name>" probe. HasFile hits the archive
// tables of every mounted pack, so each seqId is probed at most once per workshop epoch; the
// LoadFile itself only runs on an actual (cache-miss) sequence load.
struct SeqProbe {
    uint32_t epoch = 0;
    bool probed = false;
    bool found = false;
};

std::mutex gProbeMutex;
SeqProbe gProbes[kSeqCount];

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

std::string seqKey(int32_t seqId) {
    return std::string("audio/seq/") + kSeqNames[seqId];
}

std::string seqFontKey(int32_t seqId) {
    return std::string("audio/seq/") + kSeqNames[seqId] + ".font";
}

} // namespace

extern "C" int gdx_seq_packs_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.SequencePacks", 0) != 0;
}

extern "C" int GdxSeqPackResolve(int32_t seqId, void* dst, size_t dstCapacity) {
    if (seqId < 0 || seqId >= kSeqCount || dst == nullptr) {
        return 0;
    }

    const uint32_t epoch = GdxWorkshopPackEpoch();
    {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        SeqProbe& probe = gProbes[seqId];
        if (!probe.probed || probe.epoch != epoch) {
            auto am = archiveManager();
            probe.found = (am != nullptr) && am->HasFile(seqKey(seqId));
            probe.epoch = epoch;
            probe.probed = true;
        }
        if (!probe.found) {
            return 0;
        }
    }

    auto am = archiveManager();
    if (am == nullptr) {
        return 0;
    }
    auto file = am->LoadFile(seqKey(seqId));
    if (file == nullptr || file->Buffer == nullptr) {
        return 0;
    }
    // Archive backends over-allocate Buffer (+4096 guard); TrueSize is the real entry size.
    const size_t size = (file->TrueSize > 0) ? file->TrueSize : file->Buffer->size();
    if (size == 0 || file->BufferOffset + size > file->Buffer->size()) {
        return 0;
    }
    // The slot was allocated for ALIGN16(stock size); anything larger would corrupt the heap.
    if (size > dstCapacity || ((size + 15) & ~size_t(15)) > dstCapacity) {
        gdx_port_logf("[seq-pack] rejected override for audio/seq/%s: %zu bytes exceed slot capacity %zu\n",
                      kSeqNames[seqId], size, dstCapacity);
        return 0;
    }
    std::memcpy(dst, file->Buffer->data() + file->BufferOffset, size);
    gdx_port_logf("[seq-pack] applied override for audio/seq/%s: %zu bytes (slot capacity %zu)\n",
                  kSeqNames[seqId], size, dstCapacity);
    return static_cast<int>(size);
}

extern "C" int GdxSeqPackGetFont(int32_t seqId) {
    if (seqId < 0 || seqId >= kSeqCount || !gdx_seq_packs_enabled()) {
        return -1;
    }

    auto am = archiveManager();
    if (am == nullptr || !am->HasFile(seqKey(seqId)) || !am->HasFile(seqFontKey(seqId))) {
        return -1;
    }

    auto file = am->LoadFile(seqFontKey(seqId));
    if (file == nullptr || file->Buffer == nullptr) {
        return -1;
    }

    const size_t size = (file->TrueSize > 0) ? file->TrueSize : file->Buffer->size();
    if (size == 0 || file->BufferOffset + size > file->Buffer->size()) {
        return -1;
    }

    std::string text;
    text.assign(reinterpret_cast<const char*>(file->Buffer->data() + file->BufferOffset), size);
    const int fontId = fontIdFromString(text);
    if (fontId < 0) {
        gdx_port_logf("[seq-pack] ignored malformed audio/seq/%s.font: '%s'\n", kSeqNames[seqId], text.c_str());
        return -1;
    }

    gdx_port_logf("[seq-pack] using font %s for sequence %s\n", kSeqNames[fontId], kSeqNames[seqId]);
    return fontId;
}
