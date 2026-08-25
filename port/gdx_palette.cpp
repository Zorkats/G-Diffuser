/* port/gdx_palette.cpp -- F2 color palette editor implementation.
 *
 * See gdx_palette.h for the format and API contract. This TU belongs to the G-Diffuser
 * host-CRT executable, not the gdiffuser_game decomp object library, so the standard file
 * API is available here -- the same split port/sram_buffer.cpp and port/gdx_ghost_io.c use.
 *
 * PORT/DECOMP BOUNDARY: like gdx_ghost_io.c, this file deliberately does NOT include the
 * decomp headers (unk_structs.h only compiles under the gdiffuser_game target's macro set).
 * It mirrors the Machine layout and declares raw externs instead; the static_assert below
 * turns any drift against decomp/include/unk_structs.h:460-470 into a compile error.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fprintf below; harmless on non-MSVC */

#include "gdx_palette.h"

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
 * size exactly 0x16). Only customType and red/green/blue are read or written here. */
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

/* CUSTOM_MACHINE_DEFAULT, decomp/include/fzx_machine.h:24. Only these slots carry stock
 * table colors; super and edited (custom) machines use other enum values and are skipped. */
#define GDX_CUSTOM_MACHINE_DEFAULT 0

#define GDX_PALETTE_FILENAME "palette.txt"
#define GDX_PALETTE_NO_OVERRIDE (-1)

extern "C" {
/* Both defined in decomp/src/game/racer.c (gMachines at :99, sDefaultMachines at :474).
 * sDefaultMachines has 33 entries (30 roster + 3 super); only 0-29 are ever indexed here. */
extern GdxMachine gMachines[GDX_PALETTE_MACHINE_COUNT];
extern GdxMachine sDefaultMachines[];
}

/* Packed 0xRRGGBB per slot, or GDX_PALETTE_NO_OVERRIDE. */
static int32_t sOverrides[GDX_PALETTE_MACHINE_COUNT][GDX_PALETTE_SKIN_COUNT];
static int sLoaded = 0;

static int GdxPalette_InRange(int machine, int skin) {
    return machine >= 0 && machine < GDX_PALETTE_MACHINE_COUNT && skin >= 0 && skin < GDX_PALETTE_SKIN_COUNT;
}

void GdxPalette_GetFilePath(char* outPath, size_t outCap) {
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
        fileNameLen = strlen(GDX_PALETTE_FILENAME);
        if (exeDirLen + fileNameLen + 1 > outCap) {
            goto fallback;
        }
        memcpy(outPath, exePath, exeDirLen);
        memcpy(outPath + exeDirLen, GDX_PALETTE_FILENAME, fileNameLen + 1); /* + NUL */
        return;
    }

fallback:
#endif
    /* CWD-relative fallback, same as gdx_ghost_default_path on non-Windows. */
    snprintf(outPath, outCap, "%s", GDX_PALETTE_FILENAME);
}

int GdxPalette_Load(void) {
    char path[1024];
    FILE* f;
    char line[128];
    int machine;
    int skin;

    for (machine = 0; machine < GDX_PALETTE_MACHINE_COUNT; machine++) {
        for (skin = 0; skin < GDX_PALETTE_SKIN_COUNT; skin++) {
            sOverrides[machine][skin] = GDX_PALETTE_NO_OVERRIDE;
        }
    }
    sLoaded = 1;

    GdxPalette_GetFilePath(path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) {
        return 1; /* no file yet: all-stock palette is a valid state */
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        int r;
        int g;
        int b;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (sscanf(line, "%d %d %d %d %d", &machine, &skin, &r, &g, &b) != 5) {
            continue; /* malformed line: skip, keep the rest of the file usable */
        }
        if (!GdxPalette_InRange(machine, skin) || r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            continue;
        }
        sOverrides[machine][skin] = (r << 16) | (g << 8) | b;
    }
    fclose(f);
    return 1;
}

int GdxPalette_Save(void) {
    char path[1024];
    FILE* f;
    int machine;
    int skin;

    GdxPalette_GetFilePath(path, sizeof(path));
    f = fopen(path, "w");
    if (f == NULL) {
        return 0;
    }

    fprintf(f, "# G-Diffuser machine palette overrides (F2)\n");
    fprintf(f, "# <machine 0-29> <skin 0-3> <R> <G> <B>\n");
    for (machine = 0; machine < GDX_PALETTE_MACHINE_COUNT; machine++) {
        for (skin = 0; skin < GDX_PALETTE_SKIN_COUNT; skin++) {
            int32_t packed = sOverrides[machine][skin];
            if (packed != GDX_PALETTE_NO_OVERRIDE) {
                fprintf(f, "%d %d %d %d %d\n", machine, skin, (packed >> 16) & 0xFF, (packed >> 8) & 0xFF,
                        packed & 0xFF);
            }
        }
    }
    fclose(f);
    return 1;
}

static void GdxPalette_EnsureLoaded(void) {
    if (!sLoaded) {
        GdxPalette_Load();
    }
}

void GdxPalette_ApplyToMachines(void) {
    int machine;
    int skin;

    GdxPalette_EnsureLoaded();

    for (machine = 0; machine < GDX_PALETTE_MACHINE_COUNT; machine++) {
        /* Rewrite baseline + overrides in one pass: idempotent both at table-fill time
         * (the decomp hook) and after a menu edit that REMOVED an override, where the
         * stock channel value has to be written back over the old override. */
        if (gMachines[machine].customType != GDX_CUSTOM_MACHINE_DEFAULT) {
            continue;
        }
        for (skin = 0; skin < GDX_PALETTE_SKIN_COUNT; skin++) {
            int32_t packed = sOverrides[machine][skin];
            if (packed != GDX_PALETTE_NO_OVERRIDE) {
                gMachines[machine].red[skin] = (uint8_t) ((packed >> 16) & 0xFF);
                gMachines[machine].green[skin] = (uint8_t) ((packed >> 8) & 0xFF);
                gMachines[machine].blue[skin] = (uint8_t) (packed & 0xFF);
            } else {
                gMachines[machine].red[skin] = sDefaultMachines[machine].red[skin];
                gMachines[machine].green[skin] = sDefaultMachines[machine].green[skin];
                gMachines[machine].blue[skin] = sDefaultMachines[machine].blue[skin];
            }
        }
    }
}

int GdxPalette_HasOverride(int machine, int skin) {
    GdxPalette_EnsureLoaded();
    if (!GdxPalette_InRange(machine, skin)) {
        return 0;
    }
    return sOverrides[machine][skin] != GDX_PALETTE_NO_OVERRIDE;
}

int GdxPalette_GetStockColor(int machine, int skin) {
    if (!GdxPalette_InRange(machine, skin)) {
        return -1;
    }
    return ((int) sDefaultMachines[machine].red[skin] << 16) | ((int) sDefaultMachines[machine].green[skin] << 8) |
           (int) sDefaultMachines[machine].blue[skin];
}

int GdxPalette_GetColor(int machine, int skin) {
    GdxPalette_EnsureLoaded();
    if (!GdxPalette_InRange(machine, skin)) {
        return -1;
    }
    if (sOverrides[machine][skin] != GDX_PALETTE_NO_OVERRIDE) {
        return sOverrides[machine][skin];
    }
    return GdxPalette_GetStockColor(machine, skin);
}

void GdxPalette_SetColor(int machine, int skin, int packedRgb) {
    GdxPalette_EnsureLoaded();
    if (!GdxPalette_InRange(machine, skin)) {
        return;
    }
    sOverrides[machine][skin] = packedRgb & 0xFFFFFF;
    GdxPalette_ApplyToMachines();
    GdxPalette_Save();
}

void GdxPalette_ClearColor(int machine, int skin) {
    GdxPalette_EnsureLoaded();
    if (!GdxPalette_InRange(machine, skin)) {
        return;
    }
    if (sOverrides[machine][skin] == GDX_PALETTE_NO_OVERRIDE) {
        return; /* nothing changed; do not rewrite the file */
    }
    sOverrides[machine][skin] = GDX_PALETTE_NO_OVERRIDE;
    GdxPalette_ApplyToMachines();
    GdxPalette_Save();
}

void GdxPalette_ClearMachine(int machine) {
    int skin;
    int hadAny = 0;

    GdxPalette_EnsureLoaded();
    if (machine < 0 || machine >= GDX_PALETTE_MACHINE_COUNT) {
        return;
    }
    for (skin = 0; skin < GDX_PALETTE_SKIN_COUNT; skin++) {
        if (sOverrides[machine][skin] != GDX_PALETTE_NO_OVERRIDE) {
            sOverrides[machine][skin] = GDX_PALETTE_NO_OVERRIDE;
            hadAny = 1;
        }
    }
    if (hadAny) {
        GdxPalette_ApplyToMachines();
        GdxPalette_Save();
    }
}
