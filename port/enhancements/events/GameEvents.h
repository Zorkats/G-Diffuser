/**
 * @file port/enhancements/events/GameEvents.h
 * @brief G-Diffuser gameplay event surface — the seam between the matching decomp and port/.
 *
 * WHY THIS EXISTS
 * ---------------
 * decomp/ is a *matching* decompilation: every non-PORT build must stay byte-identical to the
 * retail ROM, so enhancement logic MUST NOT live there. Do not patch decomp/ to change gameplay.
 * Each hook point gets exactly ONE `#ifdef PORT` one-liner in decomp/ that announces "this
 * happened, here is the value I am about to use" — decomp/src/game/racer.c:3623 for OnBoostStart,
 * and racer.c:763, :1703, :5587 for the pre-existing frame-interpolation cut shims. The logic
 * lives in port/enhancements/ as a listener. Anything more is another decomp diff to rebase on
 * top of upstream inspectredc/fzerox.
 *
 * WHY LUS'S EVENT BUS RATHER THAN A PORT-LOCAL CALLBACK LIST
 * ----------------------------------------------------------
 * libultraship already ships a publish/subscribe bus (libultraship/src/ship/events/
 * EventSystem.cpp, C entry points in libultraship/src/libultraship/bridge/eventsbridge.cpp), and:
 *   - EventSystem::CallEvent (EventSystem.cpp:55) hands every listener a NON-const `IEvent*`
 *     pointing straight at the caller's stack payload, so a listener can write through a pointer
 *     field and change a live game value — exactly what an enhancement needs.
 *   - It is already reachable from C (eventsbridge.h), already inspectable at runtime (LUS's
 *     EventDebuggerWindow walks the registry and shows per-call-site hit counts), and already
 *     exported for .o2r script mods, so third-party mods get these hooks free.
 *
 * TRANSLATION-UNIT CONTRACT
 * -------------------------
 * DEFINE_EVENT (libultraship/include/ship/events/EventTypes.h:107) emits the payload struct plus
 * DECLARE_EVENT, whose meaning flips on INIT_EVENT_IDS:
 *   - INIT_EVENT_IDS defined  -> `extern "C" EventID OnBoostStartID = -1;`  (a DEFINITION)
 *   - INIT_EVENT_IDS undefined-> `API_EXPORT EventID OnBoostStartID;`       (a DECLARATION; per
 *     [dcl.link]/7 a declaration directly inside a linkage-specification is treated as `extern`)
 * So exactly ONE translation unit — GameEvents.cpp — may `#define INIT_EVENT_IDS` before including
 * this header. Every other includer gets plain declarations and links against those definitions.
 *
 * LISTENER PRIORITY — READ THIS BEFORE PICKING ONE
 * ------------------------------------------------
 * The engine's documented contract (EventTypes.h:12-20, SCRIPTING.md "Event Priority") says
 * EVENT_PRIORITY_HIGH listeners run FIRST. The implementation does the opposite:
 * EventSystem::RegisterListener (EventSystem.cpp:24-29) inserts with a `std::lower_bound` whose
 * comparator is `existing.Priority < toInsert.Priority`, keeping the vector sorted ASCENDING, and
 * CallEvent iterates front-to-back. Real dispatch order is LOW -> NORMAL -> HIGH, so for a mutable
 * payload the HIGHEST priority listener writes LAST and wins. Until that is fixed upstream, assume
 * "last writer wins" and prefer EVENT_PRIORITY_NORMAL.
 */

#pragma once

#include <stdint.h>

#ifndef __cplusplus
/* IEvent::Cancelled (EventTypes.h:32) is a `bool`, but EventTypes.h only pulls <stdint.h>. Nothing
   includes this header from C today — the decomp side reaches the fire helpers through a local
   `extern` declaration instead (see the "DECOMP SIDE" note below). */
#include <stdbool.h>
#endif

/* eventsbridge.h rather than ship/events/EventTypes.h directly: EventTypes.h supplies only the
   DEFINE_EVENT / REGISTER_EVENT / CALL_EVENT / REGISTER_LISTENER macros, while the C functions
   those macros expand to are declared in the bridge. Pulling the bridge here means an enhancement
   .cpp gets the complete surface from this one include. */
#include "libultraship/bridge/eventsbridge.h"

/**
 * @brief Fired the instant a racer's boost timer is (re)armed, before the game uses the value.
 *
 * @param racerId `Racer::id` (decomp/include/unk_structs.h:170) of the machine that boosted.
 *                0..gNumPlayers-1 are human players; higher ids are CPU racers. A listener that
 *                only wants to affect the local player must check this — the fire site runs for
 *                every racer on the grid, CPU included.
 * @param frames  Pointer to the live `Racer::boostTimer` (unk_structs.h:232, `s32`) the game just
 *                wrote. A listener may overwrite `*frames`. Never null at the current fire site.
 *
 * Stock value is `sInitialBoostTimer` = 100 frames (decomp/src/game/racer.c:387). 100 is not just
 * a duration: racer.c:4455 compares `boostTimer` against `sInitialBoostTimer - 1` and
 * racer.c:4460/:6093 divide by `sInitialBoostTimer` to drive rumble strength and boost-flame
 * scale. Those read the *variable*, so a listener that rewrites only `*frames` leaves them
 * normalised against 100 — a longer boost keeps its visual/rumble ramp on the stock curve rather
 * than stretching it. Accepted cosmetic mismatch for a tuning knob.
 *
 * NOT cancellable — the fire site uses CALL_EVENT, not CALL_CANCELLABLE_EVENT. By the time it
 * fires, the sound-effect flag, shadow colour and energy gating are already committed by the
 * surrounding decomp branch, so `*frames = 1` is the closest honest equivalent to a cancel.
 */
DEFINE_EVENT(OnBoostStart,
             int32_t racerId;
             int32_t* frames;)

/**
 * @brief Fired once per game-thread frame, right before gGameFrameCount increments
 *        (decomp/src/sys/sys_gfx.c, Game_ThreadEntry's main loop).
 *
 * No payload. Runs on the game thread every frame regardless of game mode, so a listener must be
 * cheap and must gate itself on game state (e.g. gGameMode) if it only cares about races. Read-only
 * intent: there is nothing to mutate and it is not cancellable.
 */
DEFINE_EVENT(OnFrame, )

/**
 * @brief Fired exactly once when the race intro countdown reaches GO
 *        (decomp/src/game/racer.c, after the loop that arms every racer's
 *        RACER_STATE_FLAGS_400000).
 *
 * No payload. Under EXPANSION_KIT the surrounding decomp branch retries while the disk drive
 * reports busy, so this fires only on the pass where the retry lets the countdown continue.
 */
DEFINE_EVENT(OnRaceStart, )

/**
 * @brief Fired when a racer crosses the line and the game records a completed lap
 *        (decomp/src/game/racer.c, after `racer->completedLapsTime` is written).
 *
 * @param racerId `Racer::id`; see OnBoostStart for the CPU-vs-player caveat. Fires for every
 *                racer, not just the local player.
 * @param lap     The racer's lap counter after the increment (`racer->lap`); the lap just
 *                finished is `lap - 1`. On the finishing crossing this equals
 *                gTotalLapCount + 1 and OnRaceFinish also fires.
 *
 * NOT fired in GAMEMODE_RECORDS, and in practice/death-race-style modes the lap counter wraps
 * instead of finishing, so repeated fires with lap == 1 are normal there.
 */
DEFINE_EVENT(OnLapCompleted,
             int32_t racerId;
             int32_t lap;)

/**
 * @brief Fired when a racer completes the final lap and the game marks them finished
 *        (decomp/src/game/racer.c, after RACER_STATE_FINISHED is set).
 *
 * @param racerId  `Racer::id`; see OnBoostStart for the CPU-vs-player caveat.
 * @param raceTime The racer's final time in milliseconds (`racer->raceTime` as just committed).
 * @param position Finishing order, 1-based (`gRacersFinished + 1` at the fire site: the game
 *                 increments its finished counter a few lines later).
 */
DEFINE_EVENT(OnRaceFinish,
             int32_t racerId;
             int32_t raceTime;
             int32_t position;)

#ifdef __cplusplus
extern "C" {
#endif

/** @brief See GameEvents_AddInstaller. */
typedef void (*GameEventsInstaller)(void);

/**
 * @brief Registers every event type with LUS's EventSystem, then runs all pending installers.
 *
 * Idempotency is load-bearing: EventSystemRegisterEvent (eventsbridge.cpp:7 -> EventSystem.cpp:9)
 * unconditionally allocates a FRESH EventID per call despite its doc comment promising name-based
 * deduplication, so a second unguarded registration would orphan every listener already attached
 * to the old ID.
 *
 * Safe to call from anywhere once Ship::Context exists (the bridge dereferences
 * Ship::Context::GetInstance()->GetEventSystem()). Callers need not call it at all — the fire
 * helpers below self-initialise on first use, which is why this layer needs no edit to main.cpp.
 */
void GameEvents_Init(void);

/**
 * @brief Enqueues @p installer to be run by GameEvents_Init once the event IDs exist.
 *
 * Enhancement modules call this from a file-scope static initialiser, so dropping a new .cpp into
 * the build is all it takes to add an enhancement — nothing central has to learn its name. The
 * backing storage is a zero-initialised static array, constant-initialised before ANY dynamic
 * initialiser in the program runs, so this is immune to static-initialisation-order fiasco no
 * matter which object file the linker orders first.
 *
 * Installers registered after GameEvents_Init has already run are executed immediately, so a
 * lazily-loaded module cannot silently miss its window.
 */
void GameEvents_AddInstaller(GameEventsInstaller installer);

/**
 * @brief Fires OnBoostStart. Called from the PORT-gated one-liner in decomp/src/game/racer.c.
 *
 * DECOMP SIDE: the gdiffuser_game target compiles decomp/ with only the decomp include paths
 * (port/CMakeLists.txt:214-219) and no libultraship headers, so racer.c cannot include this file.
 * It declares this prototype inline at the call site instead, matching the shim idiom at
 * racer.c:1703. `s32` is `signed int` (decomp/include/common.h:6) and `int32_t` is `int` on every
 * toolchain this port builds with, so the two spellings are the same type and the C linkage name
 * matches exactly.
 */
void GameEvents_FireOnBoostStart(int32_t racerId, int32_t* frames);

/**
 * @brief Fires OnFrame. Called from the PORT-gated one-liner in decomp/src/sys/sys_gfx.c.
 * Same inline-declaration constraint as GameEvents_FireOnBoostStart: gdiffuser_game compiles
 * decomp/ without port/ include paths, so the decomp call site declares this prototype locally.
 */
void GameEvents_FireOnFrame(void);

/** @brief Fires OnRaceStart. Called from the PORT-gated one-liner in decomp/src/game/racer.c. */
void GameEvents_FireOnRaceStart(void);

/** @brief Fires OnLapCompleted. Called from the PORT-gated one-liner in decomp/src/game/racer.c. */
void GameEvents_FireOnLapCompleted(int32_t racerId, int32_t lap);

/** @brief Fires OnRaceFinish. Called from the PORT-gated one-liner in decomp/src/game/racer.c. */
void GameEvents_FireOnRaceFinish(int32_t racerId, int32_t raceTime, int32_t position);

#ifdef __cplusplus
}
#endif
