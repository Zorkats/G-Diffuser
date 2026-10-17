/* port/tests/gdx_edit_cup_tests.c -- unit tests for the Edit Cup CENT/CENL split.
 * Standalone: links gdx_edit_cup.c.
 */

#include "../gdx_edit_cup.h"

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

static char* row(char* wide, int i) {
    return wide + i * GDX_EDIT_CUP_NAME_SIZE;
}

static void set_row(char* wide, int i, const char* name) {
    memset(row(wide, i), 0, GDX_EDIT_CUP_NAME_SIZE);
    memcpy(row(wide, i), name, strlen(name));
}

static void test_sizes(void) {
    check(GDX_EDIT_CUP_CENT_SIZE == 216, "CENT keeps the stock 216 bytes");
    check(GDX_EDIT_CUP_WIDE_SIZE == 24 * 21, "wide image is 24 x 21");
    check(GDX_EDIT_CUP_CENL_SIZE == 4 + 24 * 21, "CENL is magic plus 24 x 21");
}

static void test_is_wide_cent(void) {
    check(gdx_edit_cup_is_wide_cent("CRS_ENTRY", "CENT", GDX_EDIT_CUP_WIDE_SIZE), "wide CENT save matches");
    check(!gdx_edit_cup_is_wide_cent("CRS_ENTRY", "CENT", GDX_EDIT_CUP_CENT_SIZE), "stock-size CENT passes through");
    check(!gdx_edit_cup_is_wide_cent("CRS_ENTRY", "CENL", GDX_EDIT_CUP_WIDE_SIZE), "other extension passes through");
    check(!gdx_edit_cup_is_wide_cent("DAMNATION", "CENT", GDX_EDIT_CUP_WIDE_SIZE), "other name passes through");
    check(!gdx_edit_cup_is_wide_cent(NULL, "CENT", GDX_EDIT_CUP_WIDE_SIZE), "NULL name passes through");
}

static void test_pack_split(void) {
    char wide[GDX_EDIT_CUP_WIDE_SIZE];
    unsigned char cent[GDX_EDIT_CUP_CENT_SIZE];
    unsigned char cenl[GDX_EDIT_CUP_CENL_SIZE];
    int anyLong;

    memset(wide, 0, sizeof(wide));
    set_row(wide, 0, "MUTE");
    set_row(wide, 1, "DAMNATION");
    set_row(wide, 2, "EXACTLY8");
    set_row(wide, 5, "TWENTYCHARACTERSLONG");
    set_row(wide, 23, "LASTROW");

    gdx_edit_cup_pack_cent(wide, cent);
    check(strcmp((const char*) cent, "MUTE") == 0, "short name stays in CENT");
    check(cent[1 * 9] == 0, "long name leaves its CENT row empty");
    check(memcmp(cent + 2 * 9, "EXACTLY8", 8) == 0 && cent[2 * 9 + 8] == 0, "8-character name fits CENT with its NUL");
    check(cent[5 * 9] == 0, "20-character name leaves its CENT row empty");
    check(strcmp((const char*) cent + 23 * 9, "LASTROW") == 0, "row 23 packs");

    anyLong = gdx_edit_cup_pack_cenl(wide, cenl);
    check(anyLong == 1, "CENL needed with a long name");
    check(memcmp(cenl, GDX_EDIT_CUP_CENL_MAGIC, 4) == 0, "CENL magic");
    check(strcmp((const char*) cenl + 4 + 1 * 21, "DAMNATION") == 0, "CENL holds the long name");
    check(strcmp((const char*) cenl + 4 + 5 * 21, "TWENTYCHARACTERSLONG") == 0, "CENL holds a 20-character name");
    check(cenl[4 + 0 * 21] == 0 && cenl[4 + 2 * 21] == 0, "CENL leaves short rows empty");
    check(gdx_edit_cup_cenl_valid(cenl, sizeof(cenl)), "packed CENL validates");
}

static void test_pack_no_long(void) {
    char wide[GDX_EDIT_CUP_WIDE_SIZE];
    unsigned char cenl[GDX_EDIT_CUP_CENL_SIZE];

    memset(wide, 0, sizeof(wide));
    set_row(wide, 0, "MUTE");
    check(gdx_edit_cup_pack_cenl(wide, cenl) == 0, "no CENL needed without long names");
}

static void test_round_trip(void) {
    char wide[GDX_EDIT_CUP_WIDE_SIZE];
    char back[GDX_EDIT_CUP_WIDE_SIZE];
    unsigned char cent[GDX_EDIT_CUP_CENT_SIZE];
    unsigned char cenl[GDX_EDIT_CUP_CENL_SIZE];
    int filled;

    memset(wide, 0, sizeof(wide));
    set_row(wide, 0, "MUTE");
    set_row(wide, 1, "DAMNATION");
    set_row(wide, 4, "TWENTYCHARACTERSLONG");
    set_row(wide, 7, "OTHERCUP");
    gdx_edit_cup_pack_cent(wide, cent);
    gdx_edit_cup_pack_cenl(wide, cenl);

    memset(back, 0x55, sizeof(back));
    gdx_edit_cup_unpack_cent(cent, back);
    check(row(back, 1)[0] == 0, "CENT alone leaves the long row empty");
    filled = gdx_edit_cup_merge_cenl(cenl, back);
    check(filled == 2, "merge fills both long rows");
    check(memcmp(wide, back, sizeof(wide)) == 0, "CENT + CENL restores the wide image byte for byte");
}

static void test_cent_wins(void) {
    char wide[GDX_EDIT_CUP_WIDE_SIZE];
    unsigned char cent[GDX_EDIT_CUP_CENT_SIZE];
    unsigned char cenl[GDX_EDIT_CUP_CENL_SIZE];

    /* CENL written with a long name in row 1, then an older build put a short name there. */
    memset(wide, 0, sizeof(wide));
    set_row(wide, 1, "DAMNATION");
    gdx_edit_cup_pack_cenl(wide, cenl);
    memset(cent, 0, sizeof(cent));
    memcpy(cent + 1 * 9, "SHORT", 5);

    gdx_edit_cup_unpack_cent(cent, wide);
    check(gdx_edit_cup_merge_cenl(cenl, wide) == 0, "stray CENL row loses to a CENT name");
    check(strcmp(row(wide, 1), "SHORT") == 0, "CENT name kept");
}

static void test_unpack_unterminated(void) {
    char wide[GDX_EDIT_CUP_WIDE_SIZE];
    unsigned char cent[GDX_EDIT_CUP_CENT_SIZE];

    memset(cent, 'A', sizeof(cent));
    gdx_edit_cup_unpack_cent(cent, wide);
    check(strlen(row(wide, 0)) == 8, "unterminated CENT row is cut at 8");
    check(row(wide, 23)[8] == 0, "every unpacked row ends with a NUL");
}

static void test_cenl_invalid(void) {
    unsigned char cenl[GDX_EDIT_CUP_CENL_SIZE];
    char wide[GDX_EDIT_CUP_WIDE_SIZE];

    memset(wide, 0, sizeof(wide));
    gdx_edit_cup_pack_cenl(wide, cenl);
    check(!gdx_edit_cup_cenl_valid(cenl, sizeof(cenl) - 1), "short CENL refused");
    check(!gdx_edit_cup_cenl_valid(NULL, sizeof(cenl)), "NULL CENL refused");
    cenl[0] = 'X';
    check(!gdx_edit_cup_cenl_valid(cenl, sizeof(cenl)), "bad magic refused");
    cenl[0] = 'G';
    memset(cenl + 4 + 3 * 21, 'B', 21);
    check(!gdx_edit_cup_cenl_valid(cenl, sizeof(cenl)), "row with no NUL refused");
}

int main(void) {
    test_sizes();
    test_is_wide_cent();
    test_pack_split();
    test_pack_no_long();
    test_round_trip();
    test_cent_wins();
    test_unpack_unterminated();
    test_cenl_invalid();
    if (sFailures != 0) {
        fprintf(stderr, "%d failure(s)\n", sFailures);
        return 1;
    }
    printf("all gdx_edit_cup tests passed\n");
    return 0;
}
