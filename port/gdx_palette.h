/* port/gdx_palette.h -- F2 color palette editor: per-machine, per-skin RGB overrides.
 *
 * Stock machine colors live in the decomp's static sDefaultMachines[] table
 * (decomp/src/game/racer.c:474, red[4]/green[4]/blue[4] per machine). This module keeps a
 * sparse override table (30 machines x 4 skins), persists it to palette.txt next to the exe
 * (the same location convention gdx_ghost_io.c uses for ghost_export.gdg), and applies it
 * over gMachines[] at the end of func_8008D33C -- the machine-table fill -- so every later
 * consumer (racer->bodyR resolution, machine select) sees the overridden colors.
 *
 * File format: one override per line, "<machine> <skin> <R> <G> <B>" (decimal; machine
 * 0-29, skin 0-3, channels 0-255). Blank lines and '#' comments are ignored; malformed or
 * out-of-range lines are skipped, so a hand-edited file degrades to stock colors, never a
 * failed load.
 *
 * All types and prototypes are C-compatible; the decomp hook in racer.c links against
 * GdxPalette_ApplyToMachines with C linkage.
 */

#ifndef GDX_PALETTE_H
#define GDX_PALETTE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_PALETTE_MACHINE_COUNT 30
#define GDX_PALETTE_SKIN_COUNT 4

/* Replaces red/green/blue of every gMachines[] slot that still has CUSTOM_MACHINE_DEFAULT
 * with stock-or-override values. Custom machines (customType != CUSTOM_MACHINE_DEFAULT)
 * already carry player-chosen colors and are never touched. Called by the decomp hook and
 * by the menu after every edit so live colors track the table. */
void GdxPalette_ApplyToMachines(void);

/* Load/save palette.txt. Load is lazy-once: every getter and ApplyToMachines triggers it
 * on first use, so no explicit init call is required. Both return 1 on success; a missing
 * file is a successful load of zero overrides. */
int GdxPalette_Load(void);
int GdxPalette_Save(void);

/* Packed 0xRRGGBB accessors. Out-of-range machine/skin returns -1 (Get) or is ignored. */
int GdxPalette_HasOverride(int machine, int skin);
int GdxPalette_GetColor(int machine, int skin);      /* override if set, else stock */
int GdxPalette_GetStockColor(int machine, int skin);

/* Mutators apply to gMachines[] immediately and persist to palette.txt on every call. */
void GdxPalette_SetColor(int machine, int skin, int packedRgb);
void GdxPalette_ClearColor(int machine, int skin);
void GdxPalette_ClearMachine(int machine);

/* Resolves the palette.txt path (exe directory on Windows, CWD elsewhere). */
void GdxPalette_GetFilePath(char* outPath, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_PALETTE_H */
