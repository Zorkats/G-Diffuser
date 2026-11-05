/* Column scan behind port/gdx_menu_words.h. */
#include "gdx_menu_words.h"

#include <stddef.h>
#include <string.h>

static int gdxColumnBlank(const unsigned char* tex, int x) {
    int y;

    for (y = 0; y < GDX_MENU_WORDS_HEIGHT; y++) {
        if (tex[y * GDX_MENU_WORDS_WIDTH + x] != 0) {
            return 0;
        }
    }
    return 1;
}

static int gdxFirstInkedColumn(const unsigned char* tex) {
    int x;

    for (x = 0; x < GDX_MENU_WORDS_WIDTH; x++) {
        if (!gdxColumnBlank(tex, x)) {
            return x;
        }
    }
    return -1;
}

int gdx_menu_words_last_word(const unsigned char* tex, int* first, int* last) {
    int x;
    int end = -1;

    if ((tex == NULL) || (first == NULL) || (last == NULL)) {
        return 0;
    }
    for (x = GDX_MENU_WORDS_WIDTH - 1; x >= 0; x--) {
        if (!gdxColumnBlank(tex, x)) {
            end = x;
            break;
        }
    }
    if (end < 0) {
        return 0;
    }
    for (x = end - 1; x >= 0; x--) {
        if (gdxColumnBlank(tex, x)) {
            /* A blank column with nothing inked to its left is margin, not a gap between words. */
            if (gdxFirstInkedColumn(tex) > x) {
                return 0;
            }
            *first = x + 1;
            *last = end;
            return 1;
        }
    }
    return 0;
}

int gdx_menu_words_compose(const unsigned char* left, const unsigned char* right, unsigned char* out) {
    int leftFirst, leftLast, rightFirst, rightLast;
    int leftWidth, rightWidth, start, y;

    if ((out == NULL) || !gdx_menu_words_last_word(left, &leftFirst, &leftLast) ||
        !gdx_menu_words_last_word(right, &rightFirst, &rightLast)) {
        return 0;
    }
    leftWidth = leftLast - leftFirst + 1;
    rightWidth = rightLast - rightFirst + 1;
    if (leftWidth + GDX_MENU_WORDS_GAP + rightWidth > GDX_MENU_WORDS_WIDTH) {
        return 0;
    }

    /* Start where the stock row's text starts, so the label lines up with the rows above it, unless
     * that would push the second word off the right edge. */
    start = gdxFirstInkedColumn(left);
    if (start + leftWidth + GDX_MENU_WORDS_GAP + rightWidth > GDX_MENU_WORDS_WIDTH) {
        start = GDX_MENU_WORDS_WIDTH - (leftWidth + GDX_MENU_WORDS_GAP + rightWidth);
    }

    memset(out, 0, GDX_MENU_WORDS_SIZE);
    for (y = 0; y < GDX_MENU_WORDS_HEIGHT; y++) {
        const int row = y * GDX_MENU_WORDS_WIDTH;

        memcpy(&out[row + start], &left[row + leftFirst], (size_t) leftWidth);
        memcpy(&out[row + start + leftWidth + GDX_MENU_WORDS_GAP], &right[row + rightFirst], (size_t) rightWidth);
    }
    return 1;
}
