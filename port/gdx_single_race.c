/* port/gdx_single_race.c -- Single Race rules for the one-player Practice race.
 *
 * Enhancements > Single Race turns a one-player Practice race into a GP-style race. The stock
 * Practice code paths stay in place. The hooks in decomp/src/game/racer.c and
 * decomp/src/overlays/ovl_i3/{cpu,hud,menus}.c ask this file which rules apply to the race in
 * progress.
 *
 * Eligibility: GAMEMODE_PRACTICE, one player, outside the title demo. Any other mode keeps the
 * stock rules whatever the options say.
 *
 * Rules while active:
 *   Laps       3, 5, 7 or 9 finish the race GP-style: the race ends on the last lap and the GP
 *              results layout is used, without the cup, points or GP progression. Unlimited keeps
 *              the Practice lap wrap and never finishes.
 *   Racers     1 + CPU count. The default of 29 CPUs gives the stock 30-racer grid.
 *   CPU AI     CPUs race the normal race AI instead of the Practice pace table.
 *   Boost      Finite races follow GP boost rules: boost is granted per completed lap.
 *   Positions  Position markers, the top-6 list and the position readout are shown.
 *   Rivals     Optional random rival: one random CPU is marked for the whole race.
 *
 * Save rule: a Single Race never writes to the save. It does not touch the Practice best lap
 * (racer.c skips the best-lap compare and hud.c hides the readouts), Time Attack records, ghosts,
 * GP records, cup progress or points. The only state kept here is the per-race snapshot and the
 * lap-time table, both rebuilt at every race start, so Retry re-reads the options.
 */
#include <time.h>

#include "gdx_single_race.h"

extern int CVarGetInteger(const char* name, int defaultValue); // libultraship consolevariablebridge.h

#define SR_CPU_MAX 29
#define SR_LAP_CHOICES 5
#define SR_STOCK_RACERS 30
#define SR_STOCK_LAPS 3

/* Laps combo index to lap count. Index 4 is Unlimited, stored as 0. */
static const int sLapChoices[SR_LAP_CHOICES] = { 3, 5, 7, 9, 0 };

static int sActive;
static int sLapSetting; /* 3, 5, 7 or 9 for a finite race; 0 for Unlimited or when inactive */
static int sRacerCount;
static int sRivalIndex;
static unsigned int sRandState;
static int sLapTimes[GDX_SINGLE_RACE_MAX_RACERS][GDX_SINGLE_RACE_MAX_LAPS];

static unsigned int SingleRace_NextRandom(void) {
    sRandState = sRandState * 1103515245u + 12345u;
    return (sRandState >> 16) & 0x7FFFu;
}

void gdx_single_race_begin_race(int eligible) {
    int enabled = CVarGetInteger("gEnhancements.SingleRace.Enabled", 0);
    int lapIndex = CVarGetInteger("gEnhancements.SingleRace.Laps", 0);
    int cpuCount = CVarGetInteger("gEnhancements.SingleRace.CpuCount", SR_CPU_MAX);
    int randomRivals = CVarGetInteger("gEnhancements.SingleRace.RandomRivals", 0);
    int racer;
    int lap;

    for (racer = 0; racer < GDX_SINGLE_RACE_MAX_RACERS; racer++) {
        for (lap = 0; lap < GDX_SINGLE_RACE_MAX_LAPS; lap++) {
            sLapTimes[racer][lap] = 0;
        }
    }

    sActive = (eligible && enabled) ? 1 : 0;
    sLapSetting = 0;
    sRacerCount = SR_STOCK_RACERS;
    sRivalIndex = -1;

    if (!sActive) {
        return;
    }

    if ((lapIndex < 0) || (lapIndex >= SR_LAP_CHOICES)) {
        lapIndex = 0;
    }
    if (cpuCount < 0) {
        cpuCount = 0;
    }
    if (cpuCount > SR_CPU_MAX) {
        cpuCount = SR_CPU_MAX;
    }

    sLapSetting = sLapChoices[lapIndex];
    sRacerCount = 1 + cpuCount;

    if (randomRivals && (sRacerCount >= 2)) {
        sRandState = sRandState * 2654435761u + (unsigned int) time(NULL);
        sRivalIndex = 1 + (int) (SingleRace_NextRandom() % (unsigned int) (sRacerCount - 1));
    }
}

int gdx_single_race_active(void) {
    return sActive;
}

int gdx_single_race_finite(void) {
    return (sActive && (sLapSetting != 0)) ? 1 : 0;
}

int gdx_single_race_lap_count(void) {
    return gdx_single_race_finite() ? sLapSetting : SR_STOCK_LAPS;
}

int gdx_single_race_racer_count(void) {
    return sActive ? sRacerCount : SR_STOCK_RACERS;
}

int gdx_single_race_random_rival(void) {
    return sActive ? sRivalIndex : -1;
}

void gdx_single_race_store_lap_time(int racerId, int lapIndex, int lapTime) {
    if (!sActive) {
        return;
    }
    if ((racerId < 0) || (racerId >= GDX_SINGLE_RACE_MAX_RACERS)) {
        return;
    }
    if ((lapIndex < 0) || (lapIndex >= GDX_SINGLE_RACE_MAX_LAPS)) {
        return;
    }
    sLapTimes[racerId][lapIndex] = lapTime;
}

int gdx_single_race_get_lap_time(int racerId, int lapIndex) {
    if ((racerId < 0) || (racerId >= GDX_SINGLE_RACE_MAX_RACERS)) {
        return 0;
    }
    if ((lapIndex < 0) || (lapIndex >= GDX_SINGLE_RACE_MAX_LAPS)) {
        return 0;
    }
    return sLapTimes[racerId][lapIndex];
}
