/* port/gdx_custom_grid.h -- F3 custom grid: handpicked AI opponents for GP/Practice/Death Race.
 *
 * Stock func_80089800 (decomp/src/game/racer.c) fills gRacers[1..29] with the 29 non-player
 * machines and then shuffles them. This module owns the roster config that replaces that:
 * one string CVar, "gEnhancements.CustomGrid.Roster", holding 30 comma-separated
 * "<char>:<skin>" slots in grid order (slot 0 is the player -- parsed but never applied, the
 * player's own machine always wins). -1 or "r" means random for either field. An empty string
 * is the feature switch: off, and the stock fill + shuffle runs untouched.
 *
 * Duplicates and the player's own machine are legal opponent picks, matching what VS mode
 * (func_80089934) already allows. Malformed or out-of-range entries degrade to random, so a
 * hand-edited CVar never breaks roster build.
 *
 * All types and prototypes are C-compatible; the decomp hook in racer.c links against
 * GdxCustomGrid_GetRoster with C linkage.
 */

#ifndef GDX_CUSTOM_GRID_H
#define GDX_CUSTOM_GRID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CUSTOM_GRID_SLOTS 30
#define GDX_CUSTOM_GRID_SKIN_COUNT 4
#define GDX_CUSTOM_GRID_RANDOM (-1)

/* Worst case is 30 x "-1:-1," plus NUL; 256 has generous headroom. */
#define GDX_CUSTOM_GRID_STRING_MAX 256

/* Parses the roster CVar. Always fills characters[]/skins[] (GDX_CUSTOM_GRID_SLOTS entries
 * each) with a machine index / skin index, or GDX_CUSTOM_GRID_RANDOM for random slots.
 * Returns 1 when the roster string is non-empty (feature on), 0 when empty (stock). */
int GdxCustomGrid_GetRoster(int* characters, int* skins);

/* Serializes 30 slots back to the "<char>:<skin>,..." string form. Truncation keeps the
 * longest prefix that fits and never overruns out. */
void GdxCustomGrid_FormatRoster(const int* characters, const int* skins, char* out, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_CUSTOM_GRID_H */
