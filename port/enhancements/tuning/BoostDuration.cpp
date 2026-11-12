/**
 * @file port/enhancements/tuning/BoostDuration.cpp
 * @brief Enhancement: override how many frames a boost lasts. First consumer of the
 *        port/enhancements event layer, and the reference example for writing another.
 *
 * Changes a gameplay value that lives inside the matching decompilation with no decomp diff of its
 * own. Its whole contract with the rest of the program is a file-scope static initialiser handing
 * GameEvents_AddInstaller a callback, plus one entry in port/CMakeLists.txt's
 * add_executable(G-Diffuser ...) list. Copy that pattern for a new enhancement; see
 * port/enhancements/events/GameEvents.h for why the event bus exists at all.
 *
 * CVAR
 * ----
 *   gEnhancements.Tuning.BoostDuration  (int, default 0)
 *     0 or below -> stock behaviour; the listener writes nothing at all.
 *     n > 0      -> a boost lasts n frames instead of the stock 100 (decomp/src/game/racer.c:387).
 *
 * Deliberately NO menu UI: set it from LUS's console (`set gEnhancements.Tuning.BoostDuration
 * 300`) or the CVar config file. A slider in port/gdx_menu.cpp is a separate follow-up.
 *
 * SCOPE: only the manual BTN_B boost is hooked (racer.c:3623). The dash-pad boost 18 lines later
 * (racer.c:3641) arms the same `boostTimer` from the same `sInitialBoostTimer` and is left stock,
 * so that a behaviour change during testing is unambiguously attributable to one code path.
 */

#include "enhancements/events/GameEvents.h"
#include "net/gdx_net_race.h"

#include "libultraship/bridge/consolevariablebridge.h"

namespace {

// ~28 minutes at 60Hz: far past any plausible use, comfortably inside s32, and low enough that a
// fat-fingered CVar is visibly clamped rather than silently accepted. An absurd override cannot
// divide by zero (racer.c:4460 and :6093 divide by `sInitialBoostTimer`, not by this value), but
// it can produce a boost that outlives the race.
constexpr int32_t kMaxBoostFrames = 100000;

void OnBoostStartListener(IEvent* event) {
    // reinterpret_cast, not static_cast: DEFINE_EVENT (EventTypes.h:107) EMBEDS IEvent as the first
    // member rather than deriving from it, so the two types are formally unrelated and static_cast
    // is ill-formed. Both structs are standard-layout with IEvent first, so the two pointers are
    // interconvertible ([basic.compound]). This is the cast SCRIPTING.md:345 prescribes, in C-cast
    // spelling.
    auto* e = reinterpret_cast<OnBoostStart*>(event);

    // Deliberately NOT filtered by `e->racerId`: Racer_UpdateFromControls runs for every machine on
    // the grid, CPU included, so this changes the whole field rather than handing the human player
    // an advantage. That keeps it a *tuning* value instead of a cheat, and it is what composes
    // sensibly with the ghost and replay systems. A player-only enhancement would check
    // `e->racerId < gNumPlayers` — see GameEvents.h's note on the racerId payload field.
    // In an online race every copy uses the host's value, which travels in the grid (G-NET O4.4).
    const int32_t configured =
        gdx_net_race_rule_boost_frames(CVarGetInteger("gEnhancements.Tuning.BoostDuration", 0));

    // Negative is not merely meaningless, it is dangerous: the surrounding decomp branch re-arms a
    // boost only when `racer->boostTimer == 0` (racer.c:3618), and the countdown decrements past a
    // negative start value without ever hitting zero, locking the racer out of boosting for the
    // rest of the race.
    if (configured <= 0) {
        return;
    }

    // This write lands on the caller's own `Racer::boostTimer`; see GameEvents.h for why
    // EventSystem::CallEvent makes that legal.
    *e->frames = (configured > kMaxBoostFrames) ? kMaxBoostFrames : configured;
}

void Install() {
    // EVENT_PRIORITY_NORMAL, not HIGH. The engine's dispatch order is the reverse of its
    // documentation (see the priority note in GameEvents.h): listeners run LOW -> NORMAL -> HIGH,
    // so HIGH would make this the LAST writer and let it stomp any future enhancement.
    //
    // Dropping the ListenerID is safe only because this enhancement is statically linked and lives
    // for the whole process, so there is no unload path that could leave the EventSystem holding a
    // dangling callback. An .o2r script mod must keep its ID and unregister in MOD_EXIT.
    REGISTER_LISTENER(OnBoostStart, EVENT_PRIORITY_NORMAL, OnBoostStartListener);
}

// Runs during static initialisation, before main() and therefore before GameEvents_Init, so the
// callback is queued rather than invoked (GameEvents.cpp's registry is a constant-initialised
// array precisely so this is safe at any link order).
//
// `const bool` at namespace scope has internal linkage, so this cannot collide with an identically
// shaped line in another enhancement file. The object exists only for its initialiser's side
// effect; [[maybe_unused]] silences -Wunused-variable.
[[maybe_unused]] const bool sInstalled = (GameEvents_AddInstaller(&Install), true);

} // namespace
