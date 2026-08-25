/* port/gdx_achievements.h -- F10 playtime tracker + local achievements.
 *
 * Polls gSaveContext once per frame from the host loop and unlocks achievements on
 * predicate edge transitions (progress fields observed in the save image). No decomp
 * edits: the tracker reads the global directly through a mirrored layout, the same
 * boundary idiom gdx_palette.cpp uses for gMachines.
 *
 * Persistence is local-only: saves/achievements.txt next to fzerox.sav (the
 * sram_buffer.cpp saves/ convention -- exe dir on Windows, CWD on POSIX), written
 * via temp-file + rename so a crash mid-write cannot corrupt the previous state.
 * No network, no CVar writes from this TU; the master gate CVar is read live.
 *
 * Retroactive grant policy: on the FIRST tick that sees a live save (profile file
 * name non-zero -- all-zero BSS means Save_Init has not run yet), every predicate
 * already true is granted SILENTLY, without entering the newly-unlocked queue. A
 * player importing a populated fzerox.sav therefore gets their achievements but is
 * not buried under 20 toasts on first boot. Later transitions toast normally.
 * Unlocks are port-local and never re-lock: wiping fzerox.sav does not revoke them.
 *
 * All entry points lazy-init, so menu code can query without waiting on an explicit
 * startup call; GdxAchievements_Init/Shutdown exist for the host loop to own the
 * lifecycle (Shutdown persists any pending state).
 */

#ifndef GDX_ACHIEVEMENTS_H
#define GDX_ACHIEVEMENTS_H

#include <cstddef>
#include <cstdint>

struct GdxAchievementInfo {
    const char* id;          // stable string ID, e.g. "gp.jack.master" (never changes once shipped)
    const char* name;        // human-readable title
    const char* description; // one-line unlock condition
    bool unlocked;
    int64_t unlockUnixTime;  // seconds since epoch; 0 while locked
};

/* Idempotent. Init loads saves/achievements.txt; a missing or malformed file is a
 * fresh state, never an error. Shutdown persists pending state (also idempotent). */
void GdxAchievements_Init(void);
void GdxAchievements_Shutdown(void);

/* Per-frame entry point, to be called from the host loop's PerfTicks phase next to
 * gdx_disk_save_tick (port/main.cpp). Accumulates playtime (paused while the
 * G-Diffuser Menu window is visible), evaluates predicates, queues toasts, and
 * persists debounced. No-op when the gate CVar is off. */
void GdxAchievements_Tick(void);

/* Table is ordered and stable: index in [0, Count) identifies an achievement for
 * the duration of the process. Get returns false for an out-of-range index or a
 * null out pointer. */
size_t GdxAchievements_Count(void);
bool GdxAchievements_Get(size_t index, GdxAchievementInfo* out);

/* Wall-clock seconds. Total is persisted across runs; session is since process
 * start. Both exclude time with the menu open. */
uint64_t GdxAchievements_GetTotalPlaytimeSeconds(void);
uint64_t GdxAchievements_GetSessionPlaytimeSeconds(void);

/* Copies up to capacity newly-unlocked-since-last-drain entries into out, removes
 * them from the queue, and returns how many were written. The retroactive init
 * grant never enters this queue. */
size_t GdxAchievements_DrainNewlyUnlocked(GdxAchievementInfo* out, size_t capacity);

/* Resolves the achievements.txt path for diagnostics ("open folder" buttons). */
void GdxAchievements_GetFilePath(char* outPath, size_t outCap);

#endif /* GDX_ACHIEVEMENTS_H */
