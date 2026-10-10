/* port/gdx_machine_stats.cpp -- F-Zero X machine stat overrides implementation.
 *
 * See gdx_machine_stats.h for the format and API contract. This TU belongs to the G-Diffuser
 * host-CRT executable, not the gdiffuser_game decomp object library, so the standard file
 * API is available here -- the same split port/gdx_palette.cpp and port/gdx_ghost_io.c use.
 *
 * PORT/DECOMP BOUNDARY: like gdx_palette.cpp, this file deliberately does NOT include the
 * decomp headers (unk_structs.h only compiles under the gdiffuser_game target's macro set).
 * It mirrors the Machine layout and declares raw externs instead; the static_assert below
 * turns any drift against decomp/include/unk_structs.h:460-470 into a compile error.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fprintf below; harmless on non-MSVC */

#include "gdx_machine_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* Mirrors Machine, decomp/include/unk_structs.h:460-470 (s16/u8 fields, natural alignment,
 * size exactly 0x16). Only machineStats[3] and weight are read or written here. */
typedef struct GdxMachine {
    int16_t customType;
    uint8_t shadowType;
    uint8_t boostersType;
    uint8_t red[4];
    uint8_t green[4];
    uint8_t blue[4];
    uint8_t number;
    int8_t machineStats[3];
    int16_t weight;
} GdxMachine;

static_assert(sizeof(GdxMachine) == 0x16, "GdxMachine must match decomp Machine (0x16)");

#define GDX_MACHINE_STATS_FILENAME "machine_stats.txt"

/* Both defined in decomp/src/game/racer.c (gMachines at :99, sDefaultMachines at :491).
 * sDefaultMachines has 33 entries (30 roster + 3 super); only 0-29 are ever indexed here. */
extern "C" {
extern GdxMachine gMachines[GDX_MACHINE_STATS_MACHINE_COUNT];
extern GdxMachine sDefaultMachines[];
}

typedef struct GdxMachineStatsOverride {
    int enabled;
    int body;
    int boost;
    int grip;
    int weight;
} GdxMachineStatsOverride;

static GdxMachineStatsOverride sOverrides[GDX_MACHINE_STATS_MACHINE_COUNT];
static int sLoaded = 0;

static int GdxMachineStats_InRange(int machine) {
    return machine >= 0 && machine < GDX_MACHINE_STATS_MACHINE_COUNT;
}

static int GdxMachineStats_GradeInRange(int grade) {
    return grade >= GDX_MACHINE_STATS_GRADE_MIN && grade <= GDX_MACHINE_STATS_GRADE_MAX;
}

void GdxMachineStats_GetFilePath(char* outPath, size_t outCap) {
    if (outPath == NULL || outCap == 0) {
        return;
    }

#ifdef _WIN32
    {
        char exePath[MAX_PATH];
        char* slash;
        size_t exeDirLen;
        size_t fileNameLen;
        DWORD n = GetModuleFileNameA(NULL, exePath, (DWORD) sizeof(exePath));

        if (n == 0 || n >= sizeof(exePath)) {
            goto fallback;
        }
        slash = strrchr(exePath, '\\');
        if (slash == NULL) {
            goto fallback;
        }
        exeDirLen = (size_t) (slash - exePath) + 1; /* keep the trailing backslash */
        fileNameLen = strlen(GDX_MACHINE_STATS_FILENAME);
        if (exeDirLen + fileNameLen + 1 > outCap) {
            goto fallback;
        }
        memcpy(outPath, exePath, exeDirLen);
        memcpy(outPath + exeDirLen, GDX_MACHINE_STATS_FILENAME, fileNameLen + 1); /* + NUL */
        return;
    }

fallback:
#endif
    /* CWD-relative fallback, same as gdx_ghost_default_path on non-Windows. */
    snprintf(outPath, outCap, "%s", GDX_MACHINE_STATS_FILENAME);
}

int GdxMachineStats_Load(void) {
    char path[1024];
    FILE* f;
    char line[128];
    int machine;

    for (machine = 0; machine < GDX_MACHINE_STATS_MACHINE_COUNT; machine++) {
        sOverrides[machine].enabled = 0;
        sOverrides[machine].body = 0;
        sOverrides[machine].boost = 0;
        sOverrides[machine].grip = 0;
        sOverrides[machine].weight = 0;
    }
    sLoaded = 1;

    GdxMachineStats_GetFilePath(path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) {
        return 1; /* no file yet: all-stock stats is a valid state */
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        int body;
        int boost;
        int grip;
        int weight;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (sscanf(line, "%d %d %d %d %d", &machine, &body, &boost, &grip, &weight) != 5) {
            continue; /* malformed line: skip, keep the rest of the file usable */
        }
        if (!GdxMachineStats_InRange(machine) || !GdxMachineStats_GradeInRange(body) ||
            !GdxMachineStats_GradeInRange(boost) || !GdxMachineStats_GradeInRange(grip) || weight < INT16_MIN ||
            weight > INT16_MAX) {
            continue;
        }
        sOverrides[machine].enabled = 1;
        sOverrides[machine].body = body;
        sOverrides[machine].boost = boost;
        sOverrides[machine].grip = grip;
        sOverrides[machine].weight = (int16_t) weight;
    }
    fclose(f);
    return 1;
}

int GdxMachineStats_Save(void) {
    char path[1024];
    FILE* f;
    int machine;

    GdxMachineStats_GetFilePath(path, sizeof(path));
    f = fopen(path, "w");
    if (f == NULL) {
        return 0;
    }

    fprintf(f, "# G-Diffuser machine stat overrides\n");
    fprintf(f, "# <machine 0-29> <body 0-4> <boost 0-4> <grip 0-4> <weight>\n");
    fprintf(f, "# body/boost/grip use 0=A .. 4=E, matching the game's machineStats[] storage\n");
    for (machine = 0; machine < GDX_MACHINE_STATS_MACHINE_COUNT; machine++) {
        GdxMachineStatsOverride* over = &sOverrides[machine];
        if (over->enabled) {
            fprintf(f, "%d %d %d %d %d\n", machine, over->body, over->boost, over->grip, over->weight);
        }
    }
    fclose(f);
    return 1;
}

static void GdxMachineStats_EnsureLoaded(void) {
    if (!sLoaded) {
        GdxMachineStats_Load();
    }
}

void GdxMachineStats_ApplyToMachines(void) {
    int machine;

    GdxMachineStats_EnsureLoaded();

    for (machine = 0; machine < GDX_MACHINE_STATS_MACHINE_COUNT; machine++) {
        GdxMachineStatsOverride* over = &sOverrides[machine];
        if (!over->enabled) {
            /* Stock parity: with no overrides the hook must no-op. */
            continue;
        }
        gMachines[machine].machineStats[0] = (int8_t) over->body;
        gMachines[machine].machineStats[1] = (int8_t) over->boost;
        gMachines[machine].machineStats[2] = (int8_t) over->grip;
        gMachines[machine].weight = (int16_t) over->weight;
    }
}

int GdxMachineStats_HasOverride(int machine) {
    GdxMachineStats_EnsureLoaded();
    if (!GdxMachineStats_InRange(machine)) {
        return 0;
    }
    return sOverrides[machine].enabled;
}

int GdxMachineStats_GetOverride(int machine, int* body, int* boost, int* grip, int* weight) {
    GdxMachineStats_EnsureLoaded();
    if (!GdxMachineStats_InRange(machine) || !sOverrides[machine].enabled) {
        return 0;
    }
    if (body != NULL) {
        *body = sOverrides[machine].body;
    }
    if (boost != NULL) {
        *boost = sOverrides[machine].boost;
    }
    if (grip != NULL) {
        *grip = sOverrides[machine].grip;
    }
    if (weight != NULL) {
        *weight = sOverrides[machine].weight;
    }
    return 1;
}

int GdxMachineStats_GetStock(int machine, int* body, int* boost, int* grip, int* weight) {
    if (!GdxMachineStats_InRange(machine)) {
        return 0;
    }
    if (body != NULL) {
        *body = (int) sDefaultMachines[machine].machineStats[0];
    }
    if (boost != NULL) {
        *boost = (int) sDefaultMachines[machine].machineStats[1];
    }
    if (grip != NULL) {
        *grip = (int) sDefaultMachines[machine].machineStats[2];
    }
    if (weight != NULL) {
        *weight = (int) sDefaultMachines[machine].weight;
    }
    return 1;
}

void GdxMachineStats_SetOverride(int machine, int body, int boost, int grip, int weight) {
    GdxMachineStats_EnsureLoaded();
    if (!GdxMachineStats_InRange(machine) || !GdxMachineStats_GradeInRange(body) ||
        !GdxMachineStats_GradeInRange(boost) || !GdxMachineStats_GradeInRange(grip) || weight < INT16_MIN ||
        weight > INT16_MAX) {
        return;
    }
    sOverrides[machine].enabled = 1;
    sOverrides[machine].body = body;
    sOverrides[machine].boost = boost;
    sOverrides[machine].grip = grip;
    sOverrides[machine].weight = weight;
    GdxMachineStats_ApplyToMachines();
    GdxMachineStats_Save();
}

void GdxMachineStats_ClearMachine(int machine) {
    GdxMachineStats_EnsureLoaded();
    if (!GdxMachineStats_InRange(machine)) {
        return;
    }
    if (!sOverrides[machine].enabled) {
        return; /* nothing changed; do not rewrite the file */
    }
    sOverrides[machine].enabled = 0;
    /* ApplyToMachines only writes enabled overrides, so restore stock for this machine
     * explicitly — otherwise gMachines keeps the old override until the next roster init. */
    gMachines[machine].machineStats[0] = sDefaultMachines[machine].machineStats[0];
    gMachines[machine].machineStats[1] = sDefaultMachines[machine].machineStats[1];
    gMachines[machine].machineStats[2] = sDefaultMachines[machine].machineStats[2];
    gMachines[machine].weight = sDefaultMachines[machine].weight;
    GdxMachineStats_ApplyToMachines();
    GdxMachineStats_Save();
}
