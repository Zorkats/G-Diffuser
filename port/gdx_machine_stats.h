/* port/gdx_machine_stats.h -- F-Zero X machine stat overrides (body, boost, grip, weight).
 *
 * Stock machine stats live in the decomp's static sDefaultMachines[] table
 * (decomp/src/game/racer.c:491-525). This module keeps a sparse override table
 * (30 machines x 4 stats), persists it to machine_stats.txt next to the exe
 * (the same location convention gdx_palette.c uses for palette.txt), and
 * applies it over gMachines[] at the end of func_8008D33C -- the machine-table
 * fill -- so every later consumer (racer physics, machine select) sees the
 * overridden values.
 *
 * File format: one override per line, "<machine> <body> <boost> <grip> <weight>"
 * (decimal; machine 0-29, body/boost/grip 0-4 matching s8 machineStats[3] where
 * 0=A and 4=E, weight any s16). Blank lines and '#' comments are ignored;
 * malformed or out-of-range lines are skipped, so a hand-edited file degrades
 * to stock stats, never a failed load.
 *
 * All types and prototypes are C-compatible; the decomp hook in racer.c links
 * against GdxMachineStats_ApplyToMachines with C linkage.
 */

#ifndef GDX_MACHINE_STATS_H
#define GDX_MACHINE_STATS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MACHINE_STATS_MACHINE_COUNT 30
#define GDX_MACHINE_STATS_GRADE_MIN 0 /* A */
#define GDX_MACHINE_STATS_GRADE_MAX 4 /* E */

/* Apply enabled overrides over gMachines[]. Called by the decomp hook and by
 * the menu after every edit so live stats track the table. Unlike the palette
 * editor, this intentionally touches all 30 roster slots (custom/super copies
 * included) because overrides are meant to overwrite whatever the fill produced. */
void GdxMachineStats_ApplyToMachines(void);

/* Load/save machine_stats.txt. Load is lazy-once: every getter and
 * ApplyToMachines triggers it on first use. Both return 1 on success; a missing
 * file is a successful load of zero overrides. */
int GdxMachineStats_Load(void);
int GdxMachineStats_Save(void);

/* Per-machine override accessors. HasOverride returns 0/1. GetOverride fills the
 * four stat values for an enabled override and returns 1; for a disabled (or
 * out-of-range) override it returns 0 and leaves the outputs unchanged.
 * GetStock fills the four stock values for a valid machine and returns 1,
 * otherwise returns 0. */
int GdxMachineStats_HasOverride(int machine);
int GdxMachineStats_GetOverride(int machine, int* body, int* boost, int* grip, int* weight);
int GdxMachineStats_GetStock(int machine, int* body, int* boost, int* grip, int* weight);

/* Mutators apply to gMachines[] immediately and persist to machine_stats.txt on
 * every call. SetOverride enables the override with the supplied values.
 * ClearMachine disables the override for that machine and restores stock. */
void GdxMachineStats_SetOverride(int machine, int body, int boost, int grip, int weight);
void GdxMachineStats_ClearMachine(int machine);

/* Resolves the machine_stats.txt path (exe directory on Windows, CWD elsewhere). */
void GdxMachineStats_GetFilePath(char* outPath, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MACHINE_STATS_H */
