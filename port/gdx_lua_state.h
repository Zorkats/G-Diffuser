/* port/gdx_lua_state.h — C++-clean boundary for the Lua script runtime's read-only view of live
 * game state. Implemented by gdx_lua_state.c, the small C shim that MAY include the decomp headers
 * (unk_structs.h only compiles under the decomp include environment, so port C++ TUs cannot touch
 * Racer directly — same reason gdx_palette.cpp mirrors structs instead). Everything crossing this
 * boundary is POD-by-value: scripts get per-call snapshots, never live pointers, so nothing here
 * can dangle across frames. */

#ifndef GDX_LUA_STATE_H
#define GDX_LUA_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field-for-field snapshot of the Racer (decomp/include/unk_structs.h:169) slots the script API
 * exposes. s16 fields are widened to int32_t at the boundary. */
typedef struct GdxLuaRacer {
    int32_t id;
    float speed;
    float energy;
    float maxEnergy;
    int32_t lap;
    int32_t lapsCompleted;
    int32_t position;
    int32_t raceTime; /* milliseconds */
    uint8_t finished;
    uint8_t crashed;
} GdxLuaRacer;

/* Copies racer `index`'s snapshot into *out. Returns 1 on success, 0 for an out-of-range index or
 * NULL out. */
int GdxLuaRacerSnapshot(int32_t index, GdxLuaRacer* out);

/* gGameFrameCount (decomp/src/sys/sys_gfx.c:255). */
uint32_t GdxLuaFrameCount(void);

/* gGameMode (decomp/src/game/game.c:9). */
int32_t GdxLuaGameMode(void);

/* TOTAL_RACER_COUNT (decomp/include/fzx_racer.h:6) — the grid size scripts may index into. */
int32_t GdxLuaRacerCount(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_LUA_STATE_H */
