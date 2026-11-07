/* port/gdx_test_drive.c -- CPUs in a Course Edit test drive. See gdx_test_drive.h. */
#include "gdx_test_drive.h"

static int sActive;
static int sCpus;
static int sSavedDifficulty;
static short sCpuLine[GDX_TEST_DRIVE_CPU_LINE_ENTRIES];

int gdx_test_drive_begin(int cpus, int difficulty, int gameDifficulty) {
    if (cpus < 0) {
        cpus = 0;
    } else if (cpus > GDX_TEST_DRIVE_MAX_CPUS) {
        cpus = GDX_TEST_DRIVE_MAX_CPUS;
    }
    if ((difficulty < 0) || (difficulty > GDX_TEST_DRIVE_DIFFICULTY_MAX)) {
        difficulty = GDX_TEST_DRIVE_DEFAULT_DIFFICULTY;
    }
    /* A second begin without an end keeps the difficulty saved by the first. */
    if (!sActive || (sCpus == 0)) {
        sSavedDifficulty = gameDifficulty;
    }
    sActive = 1;
    sCpus = cpus;
    return (cpus > 0) ? difficulty : gameDifficulty;
}

int gdx_test_drive_end(int gameDifficulty) {
    const int hadCpus = sActive && (sCpus > 0);

    sActive = 0;
    sCpus = 0;
    return hadCpus ? sSavedDifficulty : gameDifficulty;
}

int gdx_test_drive_cpus(void) {
    return sActive ? sCpus : 0;
}

int gdx_test_drive_total_racers(void) {
    return 1 + gdx_test_drive_cpus();
}

void gdx_test_drive_pole(int* positions, int count) {
    int i;
    int player;

    if ((positions == 0) || (count < 1)) {
        return;
    }
    player = positions[0];
    for (i = 1; i < count; i++) {
        if (positions[i] < player) {
            positions[i]++;
        }
    }
    positions[0] = 1;
}

int gdx_test_drive_cpu_character(int cpuCharacter, int playerCharacter, int rosterSkipped) {
    /* A super or custom machine is not a roster slot, so the roster left nothing out to give back. */
    if ((cpuCharacter == playerCharacter) && (rosterSkipped != playerCharacter) && (rosterSkipped >= 0) &&
        (rosterSkipped < GDX_TEST_DRIVE_ROSTER)) {
        return rosterSkipped;
    }
    return cpuCharacter;
}

short* gdx_test_drive_cpu_line(void) {
    return sCpuLine;
}
