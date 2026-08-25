// port/gdx_lua.cpp — Workshop Lua script packs: one sandboxed lua_State per script.
//
// A mounted mods/*.o2r pack can carry scripts at "scripts/*.lua" (enumerated PER PACK through
// each archive's own ListFiles — never the merged VFS — so two packs shipping the same script
// name both run and every error is attributable to its pack). When the master CVar is on, each
// script gets its own state with a read-only `gdx` API and receives the GameEvents race callbacks
// as gdx.onFrame / gdx.onRaceStart / gdx.onLap / gdx.onFinish. Loading keys off
// GdxWorkshopPackEpoch(), so "Reload packs" tears down every state and re-scans with zero edits
// to gdx_workshop.cpp — the same per-epoch idiom as gdx_audio_seq_packs.cpp.
//
// SAFETY MODEL (scripts are third-party code running on the game's cooperative fiber — no
// preemption, so a stuck script would freeze the frame loop):
//   - Sandbox: no luaL_openlibs; only base (minus dofile/loadfile/load/require/collectgarbage),
//     string, table, math, utf8. load() in particular can execute precompiled bytecode, which
//     escapes any source-level sandbox, so it is removed.
//   - Instruction budget: LUA_MASKCOUNT hook armed with kInstructionBudget before EVERY pcall
//     (load and each callback); tripping it raises a Lua error caught by that script's pcall.
//   - Memory cap: the lua_Alloc counts bytes per state and refuses past kMaxScriptBytes; the
//     resulting "not enough memory" error takes the same pcall path.
//   - Any error marks the script errored and auto-disables it until the next reload; siblings
//     keep running.
//
// THREADING: all loads/teardowns/dispatches run on the game thread (the GameEvents fire sites).
// The menu thread only reads through the GdxLuaGetScriptInfo snapshot, which lock-copies.

#include "gdx_lua.h"
#include "gdx_lua_state.h"

#include "enhancements/events/GameEvents.h"
#include "gdx_workshop.h" // GdxWorkshopListPacks (per-pack enumeration + effective PackOrder)
#include "port_log.h"

#include "ship/resource/archive/O2rArchive.h"
#include "libultraship/bridge/consolevariablebridge.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <mutex>
#include <setjmp.h>
#include <string>
#include <vector>

// port/gdx_workshop.cpp; bumps on every GdxWorkshopReload.
extern "C" uint32_t GdxWorkshopPackEpoch(void);

namespace {

constexpr int kMaxScripts = 32;
// Per-pcall instruction ceiling: ~1M Lua VM instructions is a generous frame's worth of work for
// a read-only script, and the only thing standing between a bad `while true do end` and a frozen
// game. Armed before every load/callback, so the count resets per call rather than accumulating.
constexpr int kInstructionBudget = 1000000;
// Per-state heap ceiling, enforced by the counting allocator below.
constexpr size_t kMaxScriptBytes = 8 * 1024 * 1024;

struct ScriptSlot {
    lua_State* L = nullptr;
    std::string pack;   // pack id when declared, else the .o2r basename (error attribution)
    std::string name;   // script key inside the pack, e.g. "scripts/lap.lua"
    bool errored = false;
    std::string error;
    size_t allocBytes = 0; // live byte count; the lua_Alloc ud points here for the state's lifetime
};

std::mutex gMutex; // guards the slot array against the menu thread's snapshot reads
ScriptSlot gScripts[kMaxScripts];
int gScriptCount = 0;
bool gLoaded = false;
uint32_t gLoadedEpoch = 0;

// Panic escape hatch. luaD_throw aborts the process when an error is raised with no protected
// call on the stack; the panic handler runs first, and if it RETURNS the abort still happens.
// Every Lua entry point below arms this jump around its pcall, so the handler can longjmp back
// instead — the script is marked errored and the game survives. Unreachable in practice (all
// entry points are protected), so the abort fallback below is dead code by construction.
jmp_buf gPanicJmp;
bool gPanicJmpArmed = false;

int gdxLuaPanic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    gdx_port_logf("[lua] panic: %s\n", (msg != nullptr) ? msg : "(no message)");
    if (gPanicJmpArmed) {
        longjmp(gPanicJmp, 1);
    }
    abort(); // unreachable: every dispatch/load/teardown arms gPanicJmp first
}

// Counting allocator, one byte counter per state. osize is the old block size when ptr is real;
// when ptr is NULL it encodes an object TYPE (lua.h's LUA_T*), so it must not be subtracted.
void* gdxLuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    size_t* used = static_cast<size_t*>(ud);
    if (nsize == 0) {
        free(ptr);
        if (ptr != nullptr) {
            *used -= osize;
        }
        return nullptr;
    }
    const size_t newUsed = *used - ((ptr != nullptr) ? osize : 0) + nsize;
    if (newUsed > kMaxScriptBytes) {
        return nullptr; // Lua raises "not enough memory", caught by the script's own pcall
    }
    void* p = realloc(ptr, nsize);
    if (p != nullptr) {
        *used = newUsed;
    }
    return p;
}

void gdxLuaCountHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    luaL_error(L, "instruction budget exceeded (%d)", kInstructionBudget);
}

// ── Script-facing gdx.* API (v1: read-only) ─────────────────────────────────────────────────────

// Registry key holding the owning script's "<pack>/<name>" tag, so gdx.log lines are attributable.
constexpr const char* kScriptTagKey = "gdx.script_tag";

int l_gdx_log(lua_State* L) {
    size_t msgLen = 0;
    // luaL_tolstring renders any value like Lua's own print/tostring would.
    const char* msg = luaL_tolstring(L, 1, &msgLen);
    lua_getfield(L, LUA_REGISTRYINDEX, kScriptTagKey);
    const char* tag = lua_tostring(L, -1);
    gdx_port_logf("[lua:%s] %.*s\n", (tag != nullptr) ? tag : "?",
                  static_cast<int>(msgLen), (msg != nullptr) ? msg : "");
    return 0;
}

int l_gdx_frame(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(GdxLuaFrameCount()));
    return 1;
}

int l_gdx_mode(lua_State* L) {
    lua_pushinteger(L, GdxLuaGameMode());
    return 1;
}

int l_gdx_racerCount(lua_State* L) {
    lua_pushinteger(L, GdxLuaRacerCount());
    return 1;
}

void pushBoolField(lua_State* L, const char* key, int value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

void pushIntField(lua_State* L, const char* key, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

void pushNumField(lua_State* L, const char* key, lua_Number value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

int l_gdx_racer(lua_State* L) {
    const lua_Integer index = luaL_checkinteger(L, 1);
    GdxLuaRacer r;
    if (index < 0 || index > INT32_MAX || !GdxLuaRacerSnapshot(static_cast<int32_t>(index), &r)) {
        lua_pushnil(L); // out-of-range index is a nil, not an error: a grid probe loop must not die
        return 1;
    }
    lua_createtable(L, 0, 10);
    pushIntField(L, "id", r.id);
    pushNumField(L, "speed", r.speed);
    pushNumField(L, "energy", r.energy);
    pushNumField(L, "maxEnergy", r.maxEnergy);
    pushIntField(L, "lap", r.lap);
    pushIntField(L, "lapsCompleted", r.lapsCompleted);
    pushIntField(L, "position", r.position);
    pushIntField(L, "raceTime", r.raceTime);
    pushBoolField(L, "finished", r.finished);
    pushBoolField(L, "crashed", r.crashed);
    return 1;
}

const luaL_Reg kGdxApi[] = {
    { "log", l_gdx_log },         { "frame", l_gdx_frame },   { "mode", l_gdx_mode },
    { "racerCount", l_gdx_racerCount }, { "racer", l_gdx_racer }, { nullptr, nullptr },
};

// Minimal library set: base + string/table/math/utf8, with the escape hatches removed. Never
// luaL_openlibs — io/os/package/debug/coroutine stay out of v1 entirely.
void sandbox(lua_State* L, const char* tag) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    // load() can hand the VM precompiled bytecode (mode "bt" by default), and crafted bytecode
    // breaks out of any source-level sandbox. dofile/loadfile/require touch the host filesystem
    // or the (absent) package system. collectgarbage could force full-GC stalls every frame.
    for (const char* name : { "dofile", "loadfile", "load", "require", "collectgarbage" }) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    luaL_newlib(L, kGdxApi);
    lua_setglobal(L, "gdx");

    lua_pushstring(L, tag);
    lua_setfield(L, LUA_REGISTRYINDEX, kScriptTagKey);
}

// ── Loading / teardown (game thread only) ───────────────────────────────────────────────────────

void teardownLocked() {
    for (int i = 0; i < gScriptCount; i++) {
        if (gScripts[i].L != nullptr) {
            // lua_close is an unprotected Lua entry point (finalizers can raise), so it runs
            // under the panic jump like every dispatch.
            gPanicJmpArmed = true;
            if (setjmp(gPanicJmp) == 0) {
                lua_close(gScripts[i].L);
            }
            gPanicJmpArmed = false;
        }
        gScripts[i] = ScriptSlot{};
    }
    gScriptCount = 0;
}

void loadScriptLocked(const std::string& pack, const std::string& name, const char* data, size_t size) {
    ScriptSlot& slot = gScripts[gScriptCount];
    slot = ScriptSlot{};
    slot.pack = pack;
    slot.name = name;
    const std::string tag = pack + "/" + name;

    slot.L = lua_newstate(gdxLuaAlloc, &slot.allocBytes);
    if (slot.L == nullptr) {
        slot.errored = true;
        slot.error = "lua_newstate failed (out of memory)";
        gdx_port_logf("[lua] %s: %s\n", tag.c_str(), slot.error.c_str());
        gScriptCount++;
        return;
    }
    lua_atpanic(slot.L, gdxLuaPanic);
    sandbox(slot.L, tag.c_str());

    gPanicJmpArmed = true;
    int status;
    if (setjmp(gPanicJmp) == 0) {
        lua_sethook(slot.L, gdxLuaCountHook, LUA_MASKCOUNT, kInstructionBudget);
        // Mode "t": text chunks only — see the sandbox note on load() above.
        status = luaL_loadbufferx(slot.L, data, size, tag.c_str(), "t");
        if (status == LUA_OK) {
            status = lua_pcall(slot.L, 0, 0, 0); // top level: defines the gdx.on* callbacks
        }
        lua_sethook(slot.L, nullptr, 0, 0);
    } else {
        status = LUA_ERRERR;
    }
    gPanicJmpArmed = false;

    if (status != LUA_OK) {
        const char* msg = (status != LUA_ERRERR) ? lua_tostring(slot.L, -1) : "panic (see log)";
        slot.errored = true;
        slot.error = (msg != nullptr) ? msg : "(no error message)";
        gdx_port_logf("[lua] %s: %s\n", tag.c_str(), slot.error.c_str());
        lua_settop(slot.L, 0);
    }
    gScriptCount++;
}

void loadAllLocked() {
    std::vector<GdxWorkshopPackInfo> packs = GdxWorkshopListPacks();
    for (const auto& p : packs) {
        if (p.disabled) {
            continue;
        }
        Ship::O2rArchive archive(p.path);
        if (!archive.Open()) {
            continue;
        }
        auto files = archive.ListFiles("scripts/*");
        if (files == nullptr) {
            continue;
        }
        std::vector<std::string> names;
        for (const auto& kv : *files) {
            // The glob admits anything under scripts/; only *.lua is loadable source.
            if (kv.second.size() > 4 && kv.second.compare(kv.second.size() - 4, 4, ".lua") == 0) {
                names.push_back(kv.second);
            }
        }
        std::sort(names.begin(), names.end()); // stable load order inside a pack
        for (const std::string& name : names) {
            if (gScriptCount >= kMaxScripts) {
                gdx_port_logf("[lua] script cap (%d) reached; %s/%s and any further scripts skipped\n",
                              kMaxScripts, p.Identity().c_str(), name.c_str());
                return;
            }
            auto file = archive.LoadFile(name);
            if (file == nullptr || file->Buffer == nullptr) {
                continue;
            }
            // Archive backends over-allocate Buffer (+4096 guard); TrueSize is the real entry size.
            const size_t size = (file->TrueSize > 0) ? file->TrueSize : file->Buffer->size();
            if (size == 0 || file->BufferOffset + size > file->Buffer->size()) {
                continue;
            }
            loadScriptLocked(p.Identity(), name, file->Buffer->data() + file->BufferOffset, size);
        }
    }
}

// Cheap gate run at the head of every event dispatch. Reads one CVar and one epoch; when the
// feature is off and nothing is loaded this is the whole cost of the Lua layer per frame.
bool ensureScripts() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gdx_lua_enabled() == 0) {
        if (gLoaded) { // CVar flipped on->off: tear down immediately at this dispatch check
            teardownLocked();
            gLoaded = false;
        }
        return false;
    }
    const uint32_t epoch = GdxWorkshopPackEpoch();
    if (!gLoaded || gLoadedEpoch != epoch) { // first enable, or Reload packs bumped the epoch
        teardownLocked();
        loadAllLocked();
        gLoaded = true;
        gLoadedEpoch = epoch;
        if (gScriptCount > 0) {
            gdx_port_logf("[lua] loaded %d script(s)\n", gScriptCount);
        }
    }
    return gScriptCount > 0;
}

// ── Event dispatch ──────────────────────────────────────────────────────────────────────────────

// Calls gdx.<fn>(<args pushed by pushArgs>) on one script. Skips errored scripts and scripts that
// never defined the callback. Any Lua error disables just this script until the next reload.
void dispatch(ScriptSlot& slot, const char* fn, void (*pushArgs)(lua_State*, const void*), const void* argData,
              int nargs) {
    if (slot.L == nullptr || slot.errored) {
        return;
    }
    lua_State* L = slot.L;
    // Every access below is RAW: lua_getglobal/lua_getfield honour __index, and a metamethod that
    // raises (or indexing a non-table after `gdx = nil`) is an error OUTSIDE any pcall, which
    // lands in the panic handler. Raw gets can never raise.
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS); // _G
    lua_pushstring(L, "gdx");
    lua_rawget(L, -2);
    lua_remove(L, -2); // drop _G, keep gdx
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushstring(L, fn);
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    if (pushArgs != nullptr) {
        pushArgs(L, argData);
    }

    gPanicJmpArmed = true;
    int status;
    if (setjmp(gPanicJmp) == 0) {
        lua_sethook(L, gdxLuaCountHook, LUA_MASKCOUNT, kInstructionBudget);
        status = lua_pcall(L, nargs, 0, 0);
        lua_sethook(L, nullptr, 0, 0);
    } else {
        status = LUA_ERRERR;
        lua_settop(L, 0); // longjmp left the stack mid-call; the state is disabled below anyway
    }
    gPanicJmpArmed = false;

    if (status != LUA_OK) {
        const char* msg = (status != LUA_ERRERR) ? lua_tostring(L, -1) : "panic (see log)";
        slot.errored = true;
        slot.error = (msg != nullptr) ? msg : "(no error message)";
        gdx_port_logf("[lua] %s/%s: %s\n", slot.pack.c_str(), slot.name.c_str(), slot.error.c_str());
        lua_settop(L, 0);
    } else {
        lua_pop(L, 1); // the gdx table
    }
}

void dispatchAll(const char* fn, void (*pushArgs)(lua_State*, const void*), const void* argData, int nargs) {
    if (!ensureScripts()) {
        return;
    }
    for (int i = 0; i < gScriptCount; i++) {
        dispatch(gScripts[i], fn, pushArgs, argData, nargs);
    }
}

void pushTwoInts(lua_State* L, const void* data) {
    const int32_t* v = static_cast<const int32_t*>(data);
    lua_pushinteger(L, v[0]);
    lua_pushinteger(L, v[1]);
}

void pushThreeInts(lua_State* L, const void* data) {
    const int32_t* v = static_cast<const int32_t*>(data);
    lua_pushinteger(L, v[0]);
    lua_pushinteger(L, v[1]);
    lua_pushinteger(L, v[2]);
}

void onFrameListener(IEvent*) {
    dispatchAll("onFrame", nullptr, nullptr, 0);
}

void onRaceStartListener(IEvent*) {
    dispatchAll("onRaceStart", nullptr, nullptr, 0);
}

void onLapCompletedListener(IEvent* event) {
    // reinterpret_cast, not static_cast: DEFINE_EVENT embeds IEvent as the first member (see
    // BoostDuration.cpp for the full citation).
    auto* e = reinterpret_cast<OnLapCompleted*>(event);
    const int32_t args[] = { e->racerId, e->lap };
    dispatchAll("onLap", pushTwoInts, args, 2);
}

void onRaceFinishListener(IEvent* event) {
    auto* e = reinterpret_cast<OnRaceFinish*>(event);
    // Script-facing order is gdx.onFinish(racerId, position, raceTimeMs).
    const int32_t args[] = { e->racerId, e->position, e->raceTime };
    dispatchAll("onFinish", pushThreeInts, args, 3);
}

void Install() {
    // EVENT_PRIORITY_NORMAL: the bus's real dispatch order is LOW -> NORMAL -> HIGH (see the
    // priority note in GameEvents.h); read-only observers belong in the middle.
    REGISTER_LISTENER(OnFrame, EVENT_PRIORITY_NORMAL, onFrameListener);
    REGISTER_LISTENER(OnRaceStart, EVENT_PRIORITY_NORMAL, onRaceStartListener);
    REGISTER_LISTENER(OnLapCompleted, EVENT_PRIORITY_NORMAL, onLapCompletedListener);
    REGISTER_LISTENER(OnRaceFinish, EVENT_PRIORITY_NORMAL, onRaceFinishListener);
}

// Self-registration during static init, the same idiom as BoostDuration.cpp: dropping this file
// into the build is all it takes.
[[maybe_unused]] const bool sInstalled = (GameEvents_AddInstaller(&Install), true);

} // namespace

// ── Menu-facing query API (any thread; lock-copies out of the game-thread-owned slots) ──────────

extern "C" int gdx_lua_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.Scripts", 0) != 0;
}

extern "C" int GdxLuaScriptCount(void) {
    std::lock_guard<std::mutex> lock(gMutex);
    return gScriptCount;
}

extern "C" int GdxLuaGetScriptInfo(int index, GdxLuaScriptInfo* out) {
    if (out == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= gScriptCount) {
        return 0;
    }
    const ScriptSlot& slot = gScripts[index];
    out->errored = slot.errored ? 1 : 0;
    snprintf(out->pack, sizeof(out->pack), "%s", slot.pack.c_str());
    snprintf(out->name, sizeof(out->name), "%s", slot.name.c_str());
    snprintf(out->error, sizeof(out->error), "%s", slot.error.c_str());
    return 1;
}
