/* port/gdx_custom_grid.c -- F3 custom grid: roster string CVar parse/format.
 *
 * See gdx_custom_grid.h for the config contract. Dependency-free C in the same spirit as
 * gdx_dev_gates.c: the only outside symbol is CVarGetString, declared inline the way the
 * decomp files declare CVarGetInteger, so this TU needs no libultraship headers.
 */

#define _CRT_SECURE_NO_WARNINGS /* snprintf/strtol below; harmless on non-MSVC */

#include "gdx_custom_grid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char* CVarGetString(const char* name, const char* defaultValue); /* libultraship consolevariablebridge.h */

#define GDX_CUSTOM_GRID_CVAR "gEnhancements.CustomGrid.Roster"

/* One "<char>:<skin>" token. Either field may be -1 or "r" (random); a token that does not
 * parse at all leaves both fields random. Surrounding spaces are tolerated. */
static void GdxCustomGrid_ParseSlot(const char* token, int* character, int* skin) {
    char* end;
    long value;

    *character = GDX_CUSTOM_GRID_RANDOM;
    *skin = GDX_CUSTOM_GRID_RANDOM;

    while (*token == ' ') {
        token++;
    }
    if (*token == '\0') {
        return;
    }

    if (*token == 'r' || *token == 'R') {
        token++;
    } else {
        value = strtol(token, &end, 10);
        if (end == token) {
            return;
        }
        if (value >= 0 && value < GDX_CUSTOM_GRID_SLOTS) {
            *character = (int) value;
        }
        token = end;
    }

    while (*token == ' ') {
        token++;
    }
    if (*token != ':') {
        return;
    }
    token++;
    while (*token == ' ') {
        token++;
    }

    if (*token == 'r' || *token == 'R') {
        return;
    }
    value = strtol(token, &end, 10);
    if (end == token) {
        return;
    }
    if (value >= 0 && value < GDX_CUSTOM_GRID_SKIN_COUNT) {
        *skin = (int) value;
    }
}

int GdxCustomGrid_GetRoster(int* characters, int* skins) {
    const char* roster;
    const char* p;
    int slot;

    for (slot = 0; slot < GDX_CUSTOM_GRID_SLOTS; slot++) {
        characters[slot] = GDX_CUSTOM_GRID_RANDOM;
        skins[slot] = GDX_CUSTOM_GRID_RANDOM;
    }

    roster = CVarGetString(GDX_CUSTOM_GRID_CVAR, "");
    if (roster == NULL || *roster == '\0') {
        return 0;
    }

    p = roster;
    slot = 0;
    while (slot < GDX_CUSTOM_GRID_SLOTS && *p != '\0') {
        const char* comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t) (comma - p) : strlen(p);
        char token[16];

        if (len >= sizeof(token)) {
            len = sizeof(token) - 1;
        }
        memcpy(token, p, len);
        token[len] = '\0';
        GdxCustomGrid_ParseSlot(token, &characters[slot], &skins[slot]);

        slot++;
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    return 1;
}

void GdxCustomGrid_FormatRoster(const int* characters, const int* skins, char* out, size_t outCap) {
    size_t used = 0;
    int slot;

    if (outCap == 0) {
        return;
    }
    out[0] = '\0';

    for (slot = 0; slot < GDX_CUSTOM_GRID_SLOTS; slot++) {
        int n = snprintf(out + used, outCap - used, "%s%d:%d", (slot != 0) ? "," : "", characters[slot], skins[slot]);
        if (n < 0 || (size_t) n >= outCap - used) {
            return; /* keep the longest prefix that fit */
        }
        used += (size_t) n;
    }
}
