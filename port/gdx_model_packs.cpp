// port/gdx_model_packs.cpp — F4a: stock-machine model replacement via workshop packs.
//
// Pack key convention: "models/pack/machine/<name>/lod<N>" holds a DisplayList resource
// (the interpreter's G_DL_OTR_FILEPATH handler resolves it by this path), one entry per
// machine LOD the pack overrides. The merged ArchiveManager is consulted directly (packs
// are already mounted; last-wins follows the Workshop PackOrder), keyed by the workshop
// pack epoch so a "Reload packs" re-probes each key exactly once.
//
// Override mechanism: D_800CDDB0[slot*6+lod] is repointed at a two-command heap trampoline
// { G_DL_OTR_FILEPATH <path>, G_ENDDL }. The bridge translates the trampoline like any
// host-built list; the filepath command reaches the interpreter with the heap string
// pointer intact (the bridge's wide host-pointer forwarding), and the interpreter calls
// the pack list per frame — GetResourceRawPointer re-reads dirty resources, so Reload
// packs picks up new bytes with no further invalidation. Trampolines live on the plain
// heap: display lists are translated host-side, so the LLE RDRAM constraint that forces
// audio sample data into the arena does not apply; heap pointers classify as host-wide
// sources by default in the bridge (no host-range registration needed).

#include "gdx_model_packs.h"

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "port_log.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

// port/gdx_workshop.cpp; bumps on every GdxWorkshopReload.
extern "C" uint32_t GdxWorkshopPackEpoch(void);

// decomp/src/game/racer.c / common.c, compiled as C. D_800CDDB0 is Gfx*[31*6]; entries are
// read/written here as opaque pointers, so no decomp header is needed (same goal as gdx_lua.cpp).
extern "C" void* D_800CDDB0[];
extern "C" const char* gMachineNames[];

namespace {

constexpr int32_t kMachineCount = 35; // gMachineNames: 30 pilots + 5 super (common.c:100)
constexpr int32_t kSlotCount = 30;    // D_800CDDB0 has 6 entries per slot, 30 slots = 180 entries (racer.c:188)
constexpr int32_t kLodCount = 5;      // LOD1..LOD5; entry 5 is the shared empty list, never overridden

/* PORT Gfx ABI (decomp/include/PR/gbi.h): w0 u32 @0, 4 bytes pad, w1 uintptr @8, sizeof 16 —
   the kHostBuiltGfxStride the bridge reads host-built lists at. */
struct WideCmd {
    uint32_t w0;
    uint32_t pad;
    uintptr_t w1;
};
static_assert(sizeof(WideCmd) == 16, "must match the PORT Gfx stride the bridge reads");

/* LUS OTR literal opcode bytes (fast/lus_gbi.h; stable across header revisions — same
   citation as n64_gfx_bridge.cpp). 0x27 with w0 bit16 == 0 is the CALL form: the
   interpreter pushes a return (g_exec_stack.call) and the trampoline's ENDDL runs after
   the pack list completes. w1 is the host char* the handler feeds to
   ResourceManager::GetResourceRawPointer(fileName). */
constexpr uint32_t kDlOtrFilepathW0 = 0x27u << 24;
constexpr uint32_t kEndDlW0 = 0xDFu << 24; // G_ENDDL (F3DEX2)

constexpr size_t kPathCapacity = 128;

struct SlotLodRecord {
    bool registered = false;      // stock pointer snapshotted
    bool active = false;          // table entry currently repointed at the trampoline
    int32_t character = -1;       // post-remap index the path string was built for
    void* stock = nullptr;
    WideCmd* trampoline = nullptr; // 2 commands, owned by this module, never freed
    char* path = nullptr;          // key string, fixed capacity, rewritten in place
};

SlotLodRecord gRecords[kSlotCount][kLodCount];

// Per-epoch "does any mounted pack provide models/pack/machine/<name>/lod<N>" probe;
// HasFile walks every mounted pack's tables, so each key is probed at most once per epoch.
struct Probe {
    uint32_t epoch = 0;
    bool probed = false;
    bool found = false;
};
Probe gProbes[kMachineCount][kLodCount];

// Guards probes and records; the registration hook and the reload path can run on
// different threads (game vs menu).
std::mutex gMutex;

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

// gMachineNames entries are already lowercase; keys use '_' for spaces (blue_falcon).
void makeKey(int32_t character, int32_t lod, char* out, size_t outSize) {
    char name[64];
    size_t i = 0;
    for (const char* s = gMachineNames[character]; *s != '\0' && i < sizeof(name) - 1; s++) {
        name[i++] = (*s == ' ') ? '_' : *s;
    }
    name[i] = '\0';
    std::snprintf(out, outSize, "models/pack/machine/%s/lod%d", name, lod + 1);
}

// Caller holds gMutex.
bool probeFoundLocked(int32_t character, int32_t lod) {
    const uint32_t epoch = GdxWorkshopPackEpoch();
    Probe& probe = gProbes[character][lod];
    if (!probe.probed || probe.epoch != epoch) {
        char key[kPathCapacity];
        makeKey(character, lod, key, sizeof(key));
        auto am = archiveManager();
        probe.found = (am != nullptr) && am->HasFile(key);
        probe.epoch = epoch;
        probe.probed = true;
    }
    return probe.found;
}

// Caller holds gMutex.
void ensureTrampolineLocked(SlotLodRecord& rec) {
    if (rec.trampoline == nullptr) {
        rec.trampoline = new WideCmd[2];
        rec.path = new char[kPathCapacity];
        rec.trampoline[0] = { kDlOtrFilepathW0, 0, reinterpret_cast<uintptr_t>(rec.path) };
        rec.trampoline[1] = { kEndDlW0, 0, 0 };
    }
}

// Caller holds gMutex.
bool inBoundsLocked(int32_t slot, int32_t lod) {
    return slot >= 0 && slot < kSlotCount && lod >= 0 && lod < kLodCount;
}

// Caller holds gMutex.
void applyLocked(SlotLodRecord& rec, int32_t slot, int32_t lod) {
    if (!inBoundsLocked(slot, lod)) {
        return;
    }
    ensureTrampolineLocked(rec);
    if (!rec.registered) {
        rec.stock = D_800CDDB0[slot * 6 + lod];
        rec.registered = true;
    }
    // Do not clobber a non-stock pointer (e.g. a leftover trampoline or a torn
    // value); that would strand the original display-list buffer and can cause
    // the machine-draw functions to write machine geometry into our tiny heap
    // trampoline, overflowing it.
    if (rec.stock == nullptr) {
        return;
    }
    D_800CDDB0[slot * 6 + lod] = rec.trampoline;
    rec.active = true;
}

// Caller holds gMutex.
void restoreLocked(SlotLodRecord& rec, int32_t slot, int32_t lod) {
    if (!inBoundsLocked(slot, lod)) {
        return;
    }
    if (!rec.active || rec.stock == nullptr) {
        return;
    }
    D_800CDDB0[slot * 6 + lod] = rec.stock;
    rec.active = false;
}

} // namespace

extern "C" int gdx_model_packs_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.ModelPacks", 0) != 0;
}

extern "C" void GdxModelPacks_OverrideMachineLodLists(int32_t character, int32_t slot) {
    if (!gdx_model_packs_enabled()) {
        return;
    }
    if (character < 0 || character >= kMachineCount || slot < 0 || slot >= kSlotCount) {
        return;
    }

    std::lock_guard<std::mutex> lock(gMutex);
    for (int32_t lod = 0; lod < kLodCount; lod++) {
        if (!probeFoundLocked(character, lod)) {
            continue;
        }
        SlotLodRecord& rec = gRecords[slot][lod];
        ensureTrampolineLocked(rec);
        if (rec.character != character) {
            makeKey(character, lod, rec.path, kPathCapacity);
            rec.character = character;
        }
        const bool wasActive = rec.active;
        applyLocked(rec, slot, lod);
        if (!wasActive) {
            gdx_port_logf("[modelpacks] slot %d lod%d <- %s\n", slot, lod + 1, rec.path);
        }
    }
}

// Restore the stock LOD pointers for `slot` before the machine-draw functions
// run. racer.c repoints the slot at a heap trampoline after drawing so the
// bridge sees the pack list, but if the trampoline is left in place the next
// draw will write machine geometry into our 2-command heap allocation and
// overflow it. This keeps the stock buffer live during drawing.
extern "C" void GdxModelPacks_BeginMachineDraw(int32_t slot) {
    if (!gdx_model_packs_enabled()) {
        return;
    }
    if (slot < 0 || slot >= kSlotCount) {
        return;
    }

    std::lock_guard<std::mutex> lock(gMutex);
    for (int32_t lod = 0; lod < kLodCount; lod++) {
        SlotLodRecord& rec = gRecords[slot][lod];
        if (rec.active) {
            restoreLocked(rec, slot, lod);
        }
    }
}

extern "C" void GdxModelPacks_OnPacksReloaded(void) {
    const bool enabled = gdx_model_packs_enabled() != 0;

    std::lock_guard<std::mutex> lock(gMutex);
    for (int32_t slot = 0; slot < kSlotCount; slot++) {
        for (int32_t lod = 0; lod < kLodCount; lod++) {
            SlotLodRecord& rec = gRecords[slot][lod];
            if (!rec.registered || rec.character < 0) {
                continue;
            }
            // The epoch bump already happened, so this probe re-checks the mounted packs.
            const bool want = enabled && probeFoundLocked(rec.character, lod);
            if (want && !rec.active) {
                applyLocked(rec, slot, lod);
                gdx_port_logf("[modelpacks] reload: slot %d lod%d <- %s\n", slot, lod + 1, rec.path);
            } else if (!want && rec.active) {
                restoreLocked(rec, slot, lod);
                gdx_port_logf("[modelpacks] reload: slot %d lod%d restored to stock\n", slot, lod + 1);
            }
        }
    }
}
