/* port/tests/gdx_ek_text_tests.c -- unit tests for the composed Expansion Kit names and number badges.
 * Standalone: links gdx_ek_text.c. The font sheet and badges are synthetic, so no game asset is needed.
 */

#include "../gdx_ek_text.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

static int px(const unsigned char* image, int width, int x, int y) {
    unsigned char byte = image[(y * width + x) >> 1];

    return (x & 1) ? (byte & 0xF) : (byte >> 4);
}

static void set_px(unsigned char* image, int width, int x, int y, int value) {
    unsigned char* byte = &image[(y * width + x) >> 1];

    if (x & 1) {
        *byte = (unsigned char) ((*byte & 0xF0) | value);
    } else {
        *byte = (unsigned char) ((*byte & 0x0F) | (value << 4));
    }
}

static unsigned char sSheet[GDX_EK_FONT_SHEET_WIDTH * GDX_EK_FONT_SHEET_HEIGHT / 2];

/* Fills the columns first..last of a character's cell on rows 1 to 6 with value. */
static void sheet_glyph(char c, int first, int last, int value) {
    int cell = c - 0x20;
    int cellX = (cell % 16) * 8;
    int cellY = (cell / 16) * 8;
    int x;
    int y;

    for (x = first; x <= last; x++) {
        for (y = 1; y <= 6; y++) {
            set_px(sSheet, GDX_EK_FONT_SHEET_WIDTH, cellX + x, cellY + y, value);
        }
    }
}

static void test_text(void) {
    unsigned char out[GDX_EK_TEXT_BYTES(64)];
    int width;

    memset(sSheet, 0, sizeof(sSheet));
    sheet_glyph('A', 1, 5, 15); /* 5 columns wide */
    sheet_glyph('I', 3, 4, 9);  /* 2 columns wide */

    check(gdx_ek_text_measure(sSheet, "") == 0, "measure: empty text is 0");
    check(gdx_ek_text_measure(sSheet, NULL) == 0, "measure: NULL text is 0");
    check(gdx_ek_text_measure(sSheet, "A") == 5, "measure: one glyph is its ink width");
    check(gdx_ek_text_measure(sSheet, "AI") == 8, "measure: glyphs are one column apart");
    check(gdx_ek_text_measure(sSheet, "A I") == 11, "measure: a space adds three columns");
    check(gdx_ek_text_measure(sSheet, "A\x01I") == 11, "measure: a control character counts as a space");

    width = gdx_ek_text_compose(sSheet, "AI", 10, out, 64);
    check(width == 8, "compose: returns the drawn width");
    check(px(out, 64, 9, 3) == 0 && px(out, 64, 10, 3) == 15 && px(out, 64, 14, 3) == 15,
          "compose: A starts at the offset");
    check(px(out, 64, 15, 3) == 0, "compose: one blank column after A");
    check(px(out, 64, 16, 3) == 9 && px(out, 64, 17, 3) == 9 && px(out, 64, 18, 3) == 0, "compose: I follows");
    check(px(out, 64, 10, 0) == 0 && px(out, 64, 10, 7) == 0, "compose: rows outside the glyph stay blank");

    memset(out, 0xFF, sizeof(out));
    width = gdx_ek_text_compose(sSheet, "AI", 60, out, 64);
    check(width == 4, "compose: width clips at the image edge");
    check(px(out, 64, 63, 3) == 15 && px(out, 64, 0, 3) == 0, "compose: clears the image and does not wrap");
    check(gdx_ek_text_compose(sSheet, "A", 0, out, 63) == 0, "compose: refuses an odd width");
}

static unsigned char sBadges[GDX_EK_BADGE_STOCK_MAX + 1][GDX_EK_BADGE_BYTES];

/* A block digit: columns first..last on rows top..top+7, with a value that identifies its source badge. */
static void badge_block(int badge, int first, int last, int top, int value) {
    int x;
    int y;

    for (x = first; x <= last; x++) {
        for (y = top; y < top + 8; y++) {
            set_px(sBadges[badge], GDX_EK_BADGE_SIZE, x, y, value);
        }
    }
}

static void test_badges(void) {
    const unsigned char* badges[GDX_EK_BADGE_STOCK_MAX + 1];
    unsigned char out[GDX_EK_BADGE_BYTES];
    int n;

    memset(sBadges, 0, sizeof(sBadges));
    badges[0] = NULL;
    for (n = 1; n <= 9; n++) {
        badge_block(n, 5, 11, 3, n); /* single digits sit on row 3, one row high */
        badges[n] = sBadges[n];
    }
    for (n = 10; n <= 15; n++) {
        badge_block(n, 3, 7, 4, 1);           /* tens "1" */
        badge_block(n, 8, 13, 4, n - 10 + 1); /* condensed units */
        badges[n] = sBadges[n];
    }

    check(gdx_ek_badge_compose(badges, 15, out) && memcmp(out, sBadges[15], sizeof(out)) == 0,
          "badge: 15 is the stock image");
    check(gdx_ek_badge_compose(badges, 3, out) && memcmp(out, sBadges[3], sizeof(out)) == 0,
          "badge: 3 is the stock image");
    check(!gdx_ek_badge_compose(badges, 0, out), "badge: 0 is refused");
    check(!gdx_ek_badge_compose(badges, 100, out), "badge: 100 is refused");

    /* 23: tens from the units of "12" (5 wide, cols 8..13 -> 6 wide), units from "13" (6 wide). */
    check(gdx_ek_badge_compose(badges, 23, out), "badge: 23 composes");
    check(px(out, 16, 2, 4) == 3 && px(out, 16, 7, 4) == 3, "badge: tens 2 comes from the units of 12, left");
    check(px(out, 16, 8, 4) == 4 && px(out, 16, 13, 4) == 4, "badge: units 3 comes from 13, right");
    check(px(out, 16, 1, 4) == 0 && px(out, 16, 14, 4) == 0, "badge: 12 columns centered in 16");
    check(px(out, 16, 2, 3) == 0 && px(out, 16, 2, 11) == 3 && px(out, 16, 2, 12) == 0, "badge: top row is 4");

    /* 17: the tens 1 of "10", units 7 from the single-digit badge moved down to row 4. */
    check(gdx_ek_badge_compose(badges, 17, out), "badge: 17 composes");
    check(px(out, 16, 2, 4) == 1 && px(out, 16, 6, 4) == 1, "badge: tens 1 comes from 10");
    check(px(out, 16, 7, 4) == 7 && px(out, 16, 13, 4) == 7 && px(out, 16, 7, 3) == 0,
          "badge: single digit 7 is aligned to row 4");

    badges[12] = NULL;
    check(!gdx_ek_badge_compose(badges, 25, out), "badge: a missing source badge is refused");
}

static void test_marquee(void) {
    check(gdx_ek_marquee_offset(100, 0) == 0, "marquee: a name that fits does not move");
    check(gdx_ek_marquee_offset(0, 10) == 0, "marquee: starts at 0");
    check(gdx_ek_marquee_offset(44, 10) == 0, "marquee: holds at the start");
    check(gdx_ek_marquee_offset(47, 10) == 1, "marquee: one pixel every two frames");
    check(gdx_ek_marquee_offset(65, 10) == 10, "marquee: reaches the end");
    check(gdx_ek_marquee_offset(109, 10) == 10, "marquee: holds at the end");
    check(gdx_ek_marquee_offset(110, 10) == 0, "marquee: starts again");
}

int main(void) {
    test_text();
    test_badges();
    test_marquee();

    if (sFailures != 0) {
        fprintf(stderr, "%d EK text test(s) failed\n", sFailures);
        return 1;
    }
    printf("All EK text tests passed\n");
    return 0;
}
