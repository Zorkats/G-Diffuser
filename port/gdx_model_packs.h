// port/gdx_model_packs.h — F4a: stock-machine model replacement via workshop packs.
//
// A mounted mods/*.o2r pack can carry display lists at "models/pack/machine/<name>/lod<N>"
// (<name> = gMachineNames entry, lowercase with spaces as '_', e.g. blue_falcon; N = 1..5).
// When the master switch is on, the LOD-list registration hooks in decomp/src/game/racer.c
// repoint D_800CDDB0[slot*6+lod] at a heap trampoline that executes the pack list through
// the interpreter's OTR filepath handler. The stock pointer is restored when the switch is
// off or the key disappears; both apply on the next machine/screen entry or Reload packs.
// Custom (machine-editor) machines are never touched — F4a covers stock machines only.

#ifndef GDX_MODEL_PACKS_H
#define GDX_MODEL_PACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when gEnhancements.Workshop.ModelPacks is on, 0 otherwise.
int gdx_model_packs_enabled(void);

// Called from racer.c after a machine's five LOD display lists are (re)built into slot
// D_800CDDB0[slot*6+0..4]. character is the post-remap effective index (super machines
// already resolved). No-op when the master switch is off.
void GdxModelPacks_OverrideMachineLodLists(int32_t character, int32_t slot);

// Called from racer.c before the machine-draw functions write into slot
// D_800CDDB0[slot*6+0..4]. Restores any stock pointers that were repointed at pack
// trampolines so the draw functions do not overflow the heap trampoline.
void GdxModelPacks_BeginMachineDraw(int32_t slot);

// Called by GdxWorkshopReload after the pack epoch bump: restores the stock pointer for
// keys no longer present (or when the switch is off) and re-applies present ones for slots
// already registered.
void GdxModelPacks_OnPacksReloaded(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_MODEL_PACKS_H
