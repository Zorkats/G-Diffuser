/* port/tests/gdx_main_menu_grid_tests.c -- unit tests for the main menu card grid (gdx_main_menu_grid.h) and the
 * G-NET card composer (gdx_gnet_sign.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_main_menu_grid.c, gdx_gnet_sign.c and the generated gdx_gnet_sign_map.c. The sign sources
 * are synthesized; no stock pixel is used. Expected values are written out by hand from the headers.
 */

#include "../gdx_gnet_sign.h"
#include "../gdx_main_menu_grid.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define BIT(card) (1u << (card))
/* Course Edit (4) and Create Machine (7) without the 64DD disk. */
#define NO_DISK (GDX_MAIN_MENU_GRID_ALL & ~(BIT(4) | BIT(7)))

static void test_positions(void) {
    CHECK(gdx_main_menu_grid_left(0) == 56);
    CHECK(gdx_main_menu_grid_left(1) == 136);
    CHECK(gdx_main_menu_grid_left(2) == 216);
    CHECK(gdx_main_menu_grid_left(8) == 216);
    CHECK(gdx_main_menu_grid_top(0) == 38);
    CHECK(gdx_main_menu_grid_top(4) == 104);
    CHECK(gdx_main_menu_grid_top(8) == 170);
    /* Out of range falls back to the first card. */
    CHECK(gdx_main_menu_grid_left(9) == 56);
    CHECK(gdx_main_menu_grid_top(-1) == 38);
    /* The bottom row, 86 * 0.75 = 64.5 pixels tall, ends inside the 240-pixel screen. */
    CHECK(gdx_main_menu_grid_top(8) + 65 <= 240);
}

static void test_move_all_available(void) {
    const unsigned all = GDX_MAIN_MENU_GRID_ALL;

    CHECK(gdx_main_menu_grid_move(0, GDX_MAIN_MENU_GRID_RIGHT, all) == 1);
    CHECK(gdx_main_menu_grid_move(2, GDX_MAIN_MENU_GRID_RIGHT, all) == 2);
    CHECK(gdx_main_menu_grid_move(3, GDX_MAIN_MENU_GRID_LEFT, all) == 3);
    CHECK(gdx_main_menu_grid_move(5, GDX_MAIN_MENU_GRID_LEFT, all) == 4);
    CHECK(gdx_main_menu_grid_move(1, GDX_MAIN_MENU_GRID_DOWN, all) == 4);
    CHECK(gdx_main_menu_grid_move(4, GDX_MAIN_MENU_GRID_DOWN, all) == 7);
    CHECK(gdx_main_menu_grid_move(7, GDX_MAIN_MENU_GRID_DOWN, all) == 7);
    CHECK(gdx_main_menu_grid_move(8, GDX_MAIN_MENU_GRID_UP, all) == 5);
    CHECK(gdx_main_menu_grid_move(0, GDX_MAIN_MENU_GRID_UP, all) == 0);
    CHECK(gdx_main_menu_grid_move(6, GDX_MAIN_MENU_GRID_RIGHT, all) == 7);
    CHECK(gdx_main_menu_grid_move(7, GDX_MAIN_MENU_GRID_RIGHT, all) == GDX_MAIN_MENU_GNET_CARD);
    /* Unknown direction and an out-of-range card change nothing. */
    CHECK(gdx_main_menu_grid_move(4, 7, all) == 4);
    CHECK(gdx_main_menu_grid_move(9, GDX_MAIN_MENU_GRID_LEFT, all) == 9);
    CHECK(gdx_main_menu_grid_move(-1, GDX_MAIN_MENU_GRID_DOWN, all) == -1);
}

static void test_move_without_disk(void) {
    /* Left and right step over the missing cards. */
    CHECK(gdx_main_menu_grid_move(3, GDX_MAIN_MENU_GRID_RIGHT, NO_DISK) == 5);
    CHECK(gdx_main_menu_grid_move(5, GDX_MAIN_MENU_GRID_LEFT, NO_DISK) == 3);
    CHECK(gdx_main_menu_grid_move(6, GDX_MAIN_MENU_GRID_RIGHT, NO_DISK) == 8);
    CHECK(gdx_main_menu_grid_move(8, GDX_MAIN_MENU_GRID_LEFT, NO_DISK) == 6);
    /* Down from the middle column: Course Edit is missing, so the nearest card, the left one on a tie. */
    CHECK(gdx_main_menu_grid_move(1, GDX_MAIN_MENU_GRID_DOWN, NO_DISK) == 3);
    CHECK(gdx_main_menu_grid_move(3, GDX_MAIN_MENU_GRID_DOWN, NO_DISK) == 6);
    CHECK(gdx_main_menu_grid_move(5, GDX_MAIN_MENU_GRID_DOWN, NO_DISK) == 8);
    CHECK(gdx_main_menu_grid_move(8, GDX_MAIN_MENU_GRID_UP, NO_DISK) == 5);
    /* A whole row missing is skipped. */
    {
        const unsigned middleGone = GDX_MAIN_MENU_GRID_ALL & ~(BIT(3) | BIT(4) | BIT(5));

        CHECK(gdx_main_menu_grid_move(1, GDX_MAIN_MENU_GRID_DOWN, middleGone) == 7);
        CHECK(gdx_main_menu_grid_move(8, GDX_MAIN_MENU_GRID_UP, middleGone) == 2);
    }
    /* Nothing to reach. */
    CHECK(gdx_main_menu_grid_move(0, GDX_MAIN_MENU_GRID_RIGHT, BIT(0)) == 0);
    CHECK(gdx_main_menu_grid_move(0, GDX_MAIN_MENU_GRID_DOWN, BIT(0)) == 0);
}

static void test_settle(void) {
    CHECK(gdx_main_menu_grid_settle(4, GDX_MAIN_MENU_GRID_ALL) == 4);
    CHECK(gdx_main_menu_grid_settle(4, NO_DISK) == 3);
    CHECK(gdx_main_menu_grid_settle(7, NO_DISK) == 6);
    CHECK(gdx_main_menu_grid_settle(7, BIT(8) | BIT(1)) == 8);
    CHECK(gdx_main_menu_grid_settle(4, BIT(1)) == 1);
    CHECK(gdx_main_menu_grid_settle(12, NO_DISK) == 0);
    CHECK(gdx_main_menu_grid_settle(-3, BIT(2)) == 2);
    CHECK(gdx_main_menu_grid_settle(5, 0u) == 0);
}

/* Synthetic sign s, pixel i: a big-endian value no two (s, i) pairs share. */
static unsigned short synth(int s, int i) {
    return (unsigned short) ((s << 13) | i);
}

static unsigned char sSources[GDX_GNET_SIGN_SOURCES][GDX_GNET_SIGN_SIZE];
static unsigned char sOut[GDX_GNET_SIGN_SIZE];

static void fill_sources(void) {
    int s;
    int i;

    for (s = 0; s < GDX_GNET_SIGN_SOURCES; s++) {
        for (i = 0; i < GDX_GNET_SIGN_PIXELS; i++) {
            sSources[s][i * 2] = (unsigned char) (synth(s, i) >> 8);
            sSources[s][i * 2 + 1] = (unsigned char) (synth(s, i) & 0xFF);
        }
    }
}

static unsigned short out_at(int i) {
    return (unsigned short) ((sOut[i * 2] << 8) | sOut[i * 2 + 1]);
}

static void test_compose_small_map(void) {
    static const GdxGnetSignCopy copies[] = { { 10, 2, 300 }, { 11, 7, 11 } };
    static const GdxGnetSignLiteral literals[] = { { 12, 0x07DB }, { 5503, 0x0000 } };
    const GdxGnetSignMap map = { 6, copies, 2, literals, 2 };
    const unsigned char* sources[GDX_GNET_SIGN_SOURCES];
    int s;

    fill_sources();
    for (s = 0; s < GDX_GNET_SIGN_SOURCES; s++) {
        sources[s] = sSources[s];
    }
    memset(sOut, 0xEE, sizeof(sOut));
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 1);
    CHECK(out_at(0) == synth(6, 0));
    CHECK(out_at(9) == synth(6, 9));
    CHECK(out_at(10) == synth(2, 300));
    CHECK(out_at(11) == synth(7, 11));
    CHECK(out_at(12) == 0x07DB);
    CHECK(sOut[24] == 0x07 && sOut[25] == 0xDB); /* big-endian, as the archive stores it */
    CHECK(out_at(13) == synth(6, 13));
    CHECK(out_at(5503) == 0x0000);

    /* A source the map does not use may be missing; one it uses may not. */
    sources[0] = 0;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 1);
    sources[2] = 0;
    memset(sOut, 0xEE, sizeof(sOut));
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    CHECK(sOut[0] == 0xEE && sOut[GDX_GNET_SIGN_SIZE - 1] == 0xEE);
    sources[2] = sSources[2];
    sources[6] = 0;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
}

static void test_compose_bad_maps(void) {
    static const GdxGnetSignCopy badSource[] = { { 1, 8, 1 } };
    static const GdxGnetSignCopy badFrom[] = { { 1, 0, GDX_GNET_SIGN_PIXELS } };
    static const GdxGnetSignCopy badPixel[] = { { GDX_GNET_SIGN_PIXELS, 0, 1 } };
    static const GdxGnetSignLiteral badLiteral[] = { { GDX_GNET_SIGN_PIXELS, 1 } };
    const unsigned char* sources[GDX_GNET_SIGN_SOURCES];
    GdxGnetSignMap map = { 0, 0, 0, 0, 0 };
    int s;

    fill_sources();
    for (s = 0; s < GDX_GNET_SIGN_SOURCES; s++) {
        sources[s] = sSources[s];
    }
    memset(sOut, 0xEE, sizeof(sOut));
    CHECK(gdx_gnet_sign_compose(0, sources, sOut) == 0);
    CHECK(gdx_gnet_sign_compose(&map, 0, sOut) == 0);
    CHECK(gdx_gnet_sign_compose(&map, sources, 0) == 0);
    map.base = 8;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    map.base = 0;
    map.copies = badSource;
    map.copyCount = 1;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    map.copies = badFrom;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    map.copies = badPixel;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    map.copies = 0;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0); /* count without a table */
    map.copyCount = 0;
    map.literals = badLiteral;
    map.literalCount = 1;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 0);
    CHECK(sOut[0] == 0xEE);
    /* The base alone is a plain copy. */
    map.literalCount = 0;
    CHECK(gdx_gnet_sign_compose(&map, sources, sOut) == 1);
    CHECK(memcmp(sOut, sSources[0], GDX_GNET_SIGN_SIZE) == 0);
}

static void test_generated_map(void) {
    const GdxGnetSignMap* map = &gGdxGnetSignMap;
    const unsigned char* sources[GDX_GNET_SIGN_SOURCES];
    unsigned i;
    int previous = -1;
    int sorted = 1;
    int alphaOk = 1;
    static unsigned char seen[GDX_GNET_SIGN_PIXELS];
    int unique = 1;
    int s;

    CHECK(map->base < GDX_GNET_SIGN_SOURCES);
    CHECK(map->copyCount > 0);
    CHECK(map->literalCount > 0);
    CHECK(map->copyCount + map->literalCount < GDX_GNET_SIGN_PIXELS);
    for (i = 0; i < map->copyCount; i++) {
        if ((int) map->copies[i].pixel <= previous) {
            sorted = 0;
        }
        previous = map->copies[i].pixel;
        if (seen[map->copies[i].pixel]++) {
            unique = 0;
        }
    }
    previous = -1;
    for (i = 0; i < map->literalCount; i++) {
        if ((int) map->literals[i].pixel <= previous) {
            sorted = 0;
        }
        previous = map->literals[i].pixel;
        if (seen[map->literals[i].pixel]++) {
            unique = 0;
        }
        /* An opaque literal has its alpha bit; a transparent one is all zero. */
        if (!(map->literals[i].value & 1u) && map->literals[i].value != 0) {
            alphaOk = 0;
        }
    }
    CHECK(sorted);
    CHECK(unique);
    CHECK(alphaOk);

    fill_sources();
    for (s = 0; s < GDX_GNET_SIGN_SOURCES; s++) {
        sources[s] = sSources[s];
    }
    CHECK(gdx_gnet_sign_compose(map, sources, sOut) == 1);
}

int main(void) {
    test_positions();
    test_move_all_available();
    test_move_without_disk();
    test_settle();
    test_compose_small_map();
    test_compose_bad_maps();
    test_generated_map();
    if (sFailures != 0) {
        printf("gdx_main_menu_grid_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_main_menu_grid_tests: all %d checks passed\n", sChecks);
    return 0;
}
