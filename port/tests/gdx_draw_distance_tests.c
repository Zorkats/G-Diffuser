/* Unit tests for port/gdx_draw_distance.c, compiled UNMODIFIED alongside this file.
 *
 * CVarGetInteger is stubbed below, so gdx_draw_distance_current is tested against chosen CVar values.
 * The target defines EXPANSION_KIT, as the shipped game build does.
 */
#include "../gdx_draw_distance.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sPercent = 100;
static int sExtended = 0;
static int sPercentSet = 1;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, #condition);       \
            sFailures++;                                                \
        }                                                               \
    } while (0)

int CVarGetInteger(const char* name, int defaultValue) {
    if (strcmp(name, "gEnhancements.Graphics.DrawDistance") == 0) {
        return sPercentSet ? sPercent : defaultValue;
    }
    if (strcmp(name, "gEnhancements.Graphics.ExtendedDrawDistance") == 0) {
        return sExtended;
    }
    return defaultValue;
}

static GdxDrawDistance resolve(int percent, int extended) {
    GdxDrawDistance out;

    memset(&out, 0x5A, sizeof(out));
    gdx_draw_distance_resolve(percent, extended, &out);
    return out;
}

/* 100% must be bit-identical to the stock path, with Extended on or off. */
static void testStockIsExact(void) {
    int extended;

    for (extended = 0; extended <= 1; extended++) {
        GdxDrawDistance d = resolve(100, extended);
        CHECK(d.percent == 100);
        CHECK(d.cullScale == 1.0f);
        CHECK(d.farScale == 1.0f);
        CHECK(d.machineScale == 1.0f);
        CHECK(d.bigBudget == 0);
    }
}

/* Without Extended only the chunk cull scales, and the range stays 100 to 200. */
static void testNormalRange(void) {
    GdxDrawDistance d = resolve(150, 0);
    CHECK(d.percent == 150);
    CHECK(d.cullScale == 1.5f);
    CHECK(d.farScale == 1.0f);
    CHECK(d.machineScale == 1.0f);
    CHECK(d.bigBudget == 0);

    d = resolve(200, 0);
    CHECK(d.cullScale == 2.0f);
    CHECK(d.farScale == 1.0f);

    d = resolve(400, 0);
    CHECK(d.percent == 200);
    CHECK(d.cullScale == 2.0f);
    CHECK(d.farScale == 1.0f);
    CHECK(d.bigBudget == 0);
}

/* With Extended the far plane, machine cutoff and budget follow the slider up to 400. */
static void testExtendedRange(void) {
    GdxDrawDistance d = resolve(101, 1);
    CHECK(d.percent == 101);
    CHECK(d.bigBudget == 1);
    CHECK(d.farScale == d.cullScale);

    d = resolve(300, 1);
    CHECK(d.percent == 300);
    CHECK(d.cullScale == 3.0f);
    CHECK(d.farScale == 3.0f);
    CHECK(d.machineScale == 3.0f);
    CHECK(d.bigBudget == 1);

    d = resolve(401, 1);
    CHECK(d.percent == 400);
    CHECK(d.cullScale == 4.0f);
    CHECK(d.farScale == 4.0f);

    d = resolve(2147483647, 1);
    CHECK(d.percent == 400);
}

/* Hand-edited values below the range clamp to stock. */
static void testLowValues(void) {
    GdxDrawDistance d = resolve(0, 0);
    CHECK(d.percent == 100);
    CHECK(d.cullScale == 1.0f);

    d = resolve(-5, 1);
    CHECK(d.percent == 100);
    CHECK(d.farScale == 1.0f);
    CHECK(d.bigBudget == 0);

    d = resolve(-2147483647 - 1, 1);
    CHECK(d.percent == 100);
}

/* Any nonzero Extended value counts as on. */
static void testExtendedTruthiness(void) {
    GdxDrawDistance d = resolve(250, 7);
    CHECK(d.percent == 250);
    CHECK(d.farScale == 2.5f);
}

static void testNullOut(void) {
    gdx_draw_distance_resolve(150, 1, NULL); /* must not crash */
    CHECK(1);
}

/* The CVar reader passes both settings through the same rules. */
static void testCurrent(void) {
    GdxDrawDistance d;

    sPercentSet = 0; /* CVar never set: the default is stock */
    sExtended = 0;
    gdx_draw_distance_current(&d);
    CHECK(d.percent == 100);
    CHECK(d.farScale == 1.0f);
    CHECK(gdx_draw_distance_far_scale() == 1.0f);

    sPercentSet = 1;
    sPercent = 350;
    sExtended = 0;
    gdx_draw_distance_current(&d);
    CHECK(d.percent == 200);
    CHECK(d.farScale == 1.0f);
    CHECK(gdx_draw_distance_far_scale() == 1.0f);

    sExtended = 1;
    gdx_draw_distance_current(&d);
    CHECK(d.percent == 350);
    CHECK(d.cullScale == 3.5f);
    CHECK(d.bigBudget == 1);
    CHECK(gdx_draw_distance_far_scale() == 3.5f);

    sPercent = 100;
    CHECK(gdx_draw_distance_far_scale() == 1.0f);
}

int main(void) {
    testStockIsExact();
    testNormalRange();
    testExtendedRange();
    testLowValues();
    testExtendedTruthiness();
    testNullOut();
    testCurrent();
    if (sFailures != 0) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
