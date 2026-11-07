/* port/gdx_gnet_sign.c -- builds the G-NET main menu card. See gdx_gnet_sign.h.
 * Copyright (c) Zorkats
 */
#include "gdx_gnet_sign.h"

static int map_valid(const GdxGnetSignMap* map, const unsigned char* const* sources) {
    unsigned i;

    if (map->base >= GDX_GNET_SIGN_SOURCES || sources[map->base] == 0) {
        return 0;
    }
    if ((map->copyCount != 0 && map->copies == 0) || (map->literalCount != 0 && map->literals == 0)) {
        return 0;
    }
    for (i = 0; i < map->copyCount; i++) {
        const GdxGnetSignCopy* copy = &map->copies[i];

        if (copy->pixel >= GDX_GNET_SIGN_PIXELS || copy->from >= GDX_GNET_SIGN_PIXELS ||
            copy->source >= GDX_GNET_SIGN_SOURCES || sources[copy->source] == 0) {
            return 0;
        }
    }
    for (i = 0; i < map->literalCount; i++) {
        if (map->literals[i].pixel >= GDX_GNET_SIGN_PIXELS) {
            return 0;
        }
    }
    return 1;
}

int gdx_gnet_sign_compose(const GdxGnetSignMap* map, const unsigned char* const* sources, unsigned char* out) {
    const unsigned char* base;
    unsigned i;

    if (map == 0 || sources == 0 || out == 0 || !map_valid(map, sources)) {
        return 0;
    }
    base = sources[map->base];
    for (i = 0; i < GDX_GNET_SIGN_SIZE; i++) {
        out[i] = base[i];
    }
    for (i = 0; i < map->copyCount; i++) {
        const GdxGnetSignCopy* copy = &map->copies[i];
        const unsigned char* from = sources[copy->source] + copy->from * 2u;

        out[copy->pixel * 2u] = from[0];
        out[copy->pixel * 2u + 1u] = from[1];
    }
    for (i = 0; i < map->literalCount; i++) {
        const GdxGnetSignLiteral* literal = &map->literals[i];

        out[literal->pixel * 2u] = (unsigned char) (literal->value >> 8);
        out[literal->pixel * 2u + 1u] = (unsigned char) (literal->value & 0xFFu);
    }
    return 1;
}
