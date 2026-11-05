/* Unit tests for port/gdx_menu_words.c, compiled UNMODIFIED alongside this file.
 *
 * Every texture is synthetic: blocks of test values placed at chosen columns, not art from any ROM.
 */
#include "../gdx_menu_words.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, #condition);       \
            sFailures++;                                                \
        }                                                               \
    } while (0)

/* Inks columns first..last with `value` on rows 4 to 11, the way a word sits in a stock row. */
static void inkWord(unsigned char* tex, int first, int last, unsigned char value) {
    int x, y;

    for (y = 4; y < 12; y++) {
        for (x = first; x <= last; x++) {
            tex[y * GDX_MENU_WORDS_WIDTH + x] = value;
        }
    }
}

static int columnValue(const unsigned char* tex, int x) {
    return tex[6 * GDX_MENU_WORDS_WIDTH + x];
}

static void testLastWordIsFound(void) {
    unsigned char tex[GDX_MENU_WORDS_SIZE] = { 0 };
    int first = -1, last = -1;

    inkWord(tex, 1, 24, 5);
    inkWord(tex, 27, 48, 7);
    CHECK(gdx_menu_words_last_word(tex, &first, &last) == 1);
    CHECK((first == 27) && (last == 48));
}

/* One pixel of ink in a column is enough to keep it inside the word. */
static void testSparseColumnStaysInWord(void) {
    unsigned char tex[GDX_MENU_WORDS_SIZE] = { 0 };
    int first = -1, last = -1;

    inkWord(tex, 1, 10, 5);
    inkWord(tex, 14, 20, 7);
    tex[15 * GDX_MENU_WORDS_WIDTH + 13] = 9;
    CHECK(gdx_menu_words_last_word(tex, &first, &last) == 1);
    CHECK((first == 13) && (last == 20));
}

static void testBlankOrSingleWordIsRejected(void) {
    unsigned char tex[GDX_MENU_WORDS_SIZE] = { 0 };
    int first = -1, last = -1;

    CHECK(gdx_menu_words_last_word(tex, &first, &last) == 0);
    inkWord(tex, 3, 40, 5);
    CHECK(gdx_menu_words_last_word(tex, &first, &last) == 0);
    CHECK(gdx_menu_words_last_word(NULL, &first, &last) == 0);
}

static void testComposeJoinsBothWords(void) {
    unsigned char left[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char right[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char out[GDX_MENU_WORDS_SIZE];
    int x;

    inkWord(left, 1, 24, 5);
    inkWord(left, 27, 48, 7);   /* 22 wide */
    inkWord(right, 1, 25, 5);
    inkWord(right, 29, 63, 8);  /* 35 wide */
    memset(out, 0xEE, sizeof(out));
    CHECK(gdx_menu_words_compose(left, right, out) == 1);

    CHECK(columnValue(out, 0) == 0);
    for (x = 1; x <= 22; x++) {
        CHECK(columnValue(out, x) == 7);
    }
    for (x = 23; x <= 25; x++) {
        CHECK(columnValue(out, x) == 0);
    }
    for (x = 26; x <= 60; x++) {
        CHECK(columnValue(out, x) == 8);
    }
    for (x = 61; x < GDX_MENU_WORDS_WIDTH; x++) {
        CHECK(columnValue(out, x) == 0);
    }
    /* Rows outside the words are background, not the 0xEE the buffer held. */
    CHECK(out[0] == 0);
    CHECK(out[GDX_MENU_WORDS_SIZE - 1] == 0);
}

/* A late first word moves the pair left so the second word still fits. */
static void testComposeShiftsToFit(void) {
    unsigned char left[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char right[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char out[GDX_MENU_WORDS_SIZE];

    inkWord(left, 10, 20, 5);
    inkWord(left, 27, 48, 7);
    inkWord(right, 1, 25, 5);
    inkWord(right, 29, 63, 8);
    CHECK(gdx_menu_words_compose(left, right, out) == 1);
    CHECK(columnValue(out, 4) == 7);
    CHECK(columnValue(out, 3) == 0);
    CHECK(columnValue(out, 63) == 8);
}

static void testComposeRejectsWordsThatDoNotFit(void) {
    unsigned char left[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char right[GDX_MENU_WORDS_SIZE] = { 0 };
    unsigned char out[GDX_MENU_WORDS_SIZE];

    inkWord(left, 0, 1, 5);
    inkWord(left, 3, 33, 7);    /* 31 wide */
    inkWord(right, 0, 1, 5);
    inkWord(right, 3, 33, 8);   /* 31 wide, 31 + 3 + 31 > 64 */
    memset(out, 0xEE, sizeof(out));
    CHECK(gdx_menu_words_compose(left, right, out) == 0);
    CHECK((out[0] == 0xEE) && (out[GDX_MENU_WORDS_SIZE - 1] == 0xEE));

    memset(right, 0, sizeof(right));
    CHECK(gdx_menu_words_compose(left, right, out) == 0);
    CHECK(gdx_menu_words_compose(left, right, NULL) == 0);
}

int main(void) {
    testLastWordIsFound();
    testSparseColumnStaysInWord();
    testBlankOrSingleWordIsRejected();
    testComposeJoinsBothWords();
    testComposeShiftsToFit();
    testComposeRejectsWordsThatDoNotFit();

    if (sFailures != 0) {
        printf("gdx_menu_words_tests: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("gdx_menu_words_tests: all passed\n");
    return 0;
}
