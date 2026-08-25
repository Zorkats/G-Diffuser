/* port/gdx_lua_state.c — decomp-facing half of the Lua script runtime's state API.
 *
 * Compiled into the exe targets (which carry the decomp include dirs, port/CMakeLists.txt
 * target_include_directories(G-Diffuser ...)), so this TU includes unk_structs.h directly and
 * hands gdx_lua.cpp plain POD snapshots. The C++ runtime side deliberately does NOT include the
 * decomp headers — the same port/decomp boundary convention as decomp_port.c vs the port C++ TUs.
 *
 * _LANGUAGE_C is defined here because the US exe target does not pass it (only the gdiffuser_game
 * object library and the JP exe target do), and PR/ultratypes.h gates the s32/u32 typedefs on it.
 * PORT must be defined for the same reason: without it libc/stdint.h takes its N64-width s32
 * intptr_t branch, which both mismatches the host ABI and collides (C2371) with the MSVC <stdint.h>
 * pulled in by gdx_lua_state.h below. With PORT, the decomp s64 spellings (long long) are the same
 * type as MSVC's __int64, so the two headers coexist. The game objects compile with PORT too, so
 * the layouts this TU sees match the definitions it links against. */

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif
#ifndef PORT
#define PORT 1
#endif

#include "unk_structs.h"
#include "fzx_racer.h"

/* Last: its <stdint.h> must come after the decomp's libc/stdint.h so the duplicate intptr_t
 * typedefs resolve against the PORT-width spellings. */
#include "gdx_lua_state.h"

/* Declared inline, the decomp call-site idiom: gGameFrameCount lives in sys_gfx.c:255, gGameMode
 * in game.c:9, and neither is declared in a header this TU can include cleanly. */
extern u32 gGameFrameCount;
extern s32 gGameMode;

int GdxLuaRacerSnapshot(int32_t index, GdxLuaRacer* out) {
    const Racer* racer;

    if (out == NULL || index < 0 || index >= TOTAL_RACER_COUNT) {
        return 0;
    }

    racer = &gRacers[index];
    out->id = racer->id;
    out->speed = racer->speed;
    out->energy = racer->energy;
    out->maxEnergy = racer->maxEnergy;
    out->lap = racer->lap;
    out->lapsCompleted = racer->lapsCompleted;
    out->position = racer->position;
    out->raceTime = racer->raceTime;
    out->finished = (racer->stateFlags & RACER_STATE_FINISHED) != 0;
    out->crashed = (racer->stateFlags & RACER_STATE_CRASHED) != 0;
    return 1;
}

uint32_t GdxLuaFrameCount(void) {
    return gGameFrameCount;
}

int32_t GdxLuaGameMode(void) {
    return gGameMode;
}

int32_t GdxLuaRacerCount(void) {
    return TOTAL_RACER_COUNT;
}
