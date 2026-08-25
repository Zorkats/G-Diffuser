/* port/gdx_lua.h — Workshop Lua script packs (runtime in gdx_lua.cpp).
 *
 * Opt-in through gEnhancements.Workshop.Scripts (int, default 0), so a fresh gdiffuser.cfg.json
 * runs byte-identically to stock. When on, every enabled mods/ .o2r pack's scripts/ .lua entries
 * each run in their own sandboxed lua_State (read-only game-state API, no io/os/package/debug),
 * receiving the GameEvents race callbacks as gdx.onFrame / gdx.onRaceStart / gdx.onLap /
 * gdx.onFinish. Loading and reloads key off GdxWorkshopPackEpoch(), exactly like the audio pack
 * stages: "Reload packs" (or toggling the CVar) tears down every state and re-scans.
 *
 * The block below the extern "C" is the menu-facing script list (port/gdx_menu.cpp's DrawScripts). */

#ifndef GDX_LUA_H
#define GDX_LUA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when gEnhancements.Workshop.Scripts is on, 0 otherwise. Cheap CVar read; the expensive pack
 * scan runs once per workshop epoch, not per call. */
int gdx_lua_enabled(void);

/* Menu-facing script status. Buffer sizes are fixed so the query can be lock-copied without
 * allocation; strings are truncated to fit. */
#define GDX_LUA_PACK_LEN 128
#define GDX_LUA_NAME_LEN 128
#define GDX_LUA_ERROR_LEN 256

typedef struct GdxLuaScriptInfo {
    int errored;                     /* 0 running, 1 auto-disabled until the next reload */
    char pack[GDX_LUA_PACK_LEN];     /* pack id when declared, else the .o2r basename */
    char name[GDX_LUA_NAME_LEN];     /* script key inside the pack, e.g. "scripts/lap.lua" */
    char error[GDX_LUA_ERROR_LEN];   /* load/runtime error text when errored, else empty */
} GdxLuaScriptInfo;

/* Number of scripts discovered by the last scan (0 when the feature is off or nothing loaded). */
int GdxLuaScriptCount(void);

/* Lock-copies script `index`'s status into *out. Returns 1 on success, 0 when out of range. */
int GdxLuaGetScriptInfo(int index, GdxLuaScriptInfo* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_LUA_H */
