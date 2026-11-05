#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the persisted SRAM image. Must stay equal to the decomp's SaveContext
// (decomp/include/fzx_save.h: profileSaves + ghostSave + characterSaves + cupSave),
// which is also the real F-Zero X cart's battery-backed SRAM size. Existing
// fzerox.sav files are rejected on a size mismatch.
#define GDX_SRAM_SIZE 0x8000u

// Loads saves/fzerox.sav into the in-memory image, or zero-fills it when no save
// exists yet or it is the wrong size. A legacy save from the older location is
// migrated in automatically. Idempotent -- called from the decomp's Sram_Init()
// (decomp/src/overlays/ovl_i2/save.c) and from gdx_sram_read/gdx_sram_write, so
// load order never matters.
void gdx_sram_init(void);

// SRAM byte-range primitives backing the decomp's Sram_ReadWrite. offset/size are
// relative to the GDX_SRAM_SIZE-byte SaveContext image; out-of-range requests are
// logged and ignored (read returns zeros) rather than touching memory outside it.
void gdx_sram_read(unsigned int offset, void* dst, unsigned int size);

// Write-through: persists the whole image to fzerox.sav immediately, no debounce
// needed at this size and call frequency.
void gdx_sram_write(unsigned int offset, const void* src, unsigned int size);

// Imports an emulator cartridge save (.sra, or a RetroArch parallel-n64 .srm) into the save in
// use. The file is converted and checked now, then waits beside the save as <save>.import until
// the next launch, when gdx_sram_init backs up the current save and swaps the import in. The
// running session's save is never touched, because the game still holds it in memory and would
// write its sections back over the import. Returns 1 when staged; message receives a one-line
// result either way.
int gdx_sram_import_stage(const char* sourcePathUtf8, char* message, size_t messageCap);

// 1 while an import waits for the next launch.
int gdx_sram_import_pending(void);

// Drops a waiting import. Returns 1 when nothing waits afterwards.
int gdx_sram_import_cancel(void);

// What this launch did with a waiting import: 0 nothing was waiting, 1 applied, -1 not applied.
// message receives the line the menu shows, and is left empty for 0.
int gdx_sram_import_launch_result(char* message, size_t messageCap);

#ifdef __cplusplus
}
#endif
