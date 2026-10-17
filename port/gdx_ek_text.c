/* port/gdx_ek_text.c -- Expansion Kit menu text built from the stock textures. See gdx_ek_text.h. */

#include "gdx_ek_text.h"

#include <string.h>

#define GDX_EK_FONT_CELL 8
#define GDX_EK_FONT_COLUMNS (GDX_EK_FONT_SHEET_WIDTH / GDX_EK_FONT_CELL)
#define GDX_EK_FONT_FIRST 0x20
#define GDX_EK_FONT_LAST 0x7F
/* Blank columns a space adds on top of the one that follows every glyph. MUTE CITY in the stock name
 * image has four blank columns between E and C. */
#define GDX_EK_TEXT_SPACE 3
#define GDX_EK_BADGE_TOP_ROW 4
#define GDX_EK_BADGE_UNITS_COLUMN 8
#define GDX_EK_MARQUEE_HOLD 45

static int i4_get(const unsigned char* image, int width, int x, int y) {
    unsigned char byte = image[(y * width + x) >> 1];

    return (x & 1) ? (byte & 0xF) : (byte >> 4);
}

static void i4_set(unsigned char* image, int width, int x, int y, int value) {
    unsigned char* byte = &image[(y * width + x) >> 1];

    if (x & 1) {
        *byte = (unsigned char) ((*byte & 0xF0) | (value & 0xF));
    } else {
        *byte = (unsigned char) ((*byte & 0x0F) | ((value & 0xF) << 4));
    }
}

/* Ink columns of a font cell, as first and last column inside the cell. Returns 0 for a blank cell. */
static int font_cell_ink(const unsigned char* sheet, int cell, int* first, int* last) {
    int cellX = (cell % GDX_EK_FONT_COLUMNS) * GDX_EK_FONT_CELL;
    int cellY = (cell / GDX_EK_FONT_COLUMNS) * GDX_EK_FONT_CELL;
    int x;
    int y;
    int found = 0;

    for (x = 0; x < GDX_EK_FONT_CELL; x++) {
        for (y = 0; y < GDX_EK_FONT_CELL; y++) {
            if (i4_get(sheet, GDX_EK_FONT_SHEET_WIDTH, cellX + x, cellY + y) != 0) {
                if (!found) {
                    *first = x;
                    found = 1;
                }
                *last = x;
                break;
            }
        }
    }
    return found;
}

/* Lays out text from column xOffset, drawing into out when it is not NULL. Returns the total width. */
static int text_layout(const unsigned char* sheet, const char* text, int xOffset, unsigned char* out,
                       int outWidth) {
    int x = 0;
    int width = 0;
    int i;

    if (sheet == NULL || text == NULL) {
        return 0;
    }
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char) text[i];
        int cell;
        int first = 0;
        int last = 0;

        if (c < GDX_EK_FONT_FIRST || c > GDX_EK_FONT_LAST) {
            c = ' ';
        }
        cell = c - GDX_EK_FONT_FIRST;
        if (!font_cell_ink(sheet, cell, &first, &last)) {
            x += GDX_EK_TEXT_SPACE;
            continue;
        }
        if (out != NULL) {
            int cellX = (cell % GDX_EK_FONT_COLUMNS) * GDX_EK_FONT_CELL;
            int cellY = (cell / GDX_EK_FONT_COLUMNS) * GDX_EK_FONT_CELL;
            int gx;
            int gy;

            for (gx = first; gx <= last; gx++) {
                int dx = xOffset + x + (gx - first);

                if (dx < 0 || dx >= outWidth) {
                    continue;
                }
                for (gy = 0; gy < GDX_EK_TEXT_HEIGHT; gy++) {
                    i4_set(out, outWidth, dx, gy, i4_get(sheet, GDX_EK_FONT_SHEET_WIDTH, cellX + gx, cellY + gy));
                }
            }
        }
        x += last - first + 1;
        width = x;
        x += 1;
    }
    return width;
}

int gdx_ek_text_measure(const unsigned char* fontSheet, const char* text) {
    return text_layout(fontSheet, text, 0, NULL, 0);
}

int gdx_ek_text_compose(const unsigned char* fontSheet, const char* text, int xOffset, unsigned char* out,
                        int outWidth) {
    int width;

    if (out == NULL || outWidth <= 0 || (outWidth & 1) != 0) {
        return 0;
    }
    memset(out, 0, (size_t) GDX_EK_TEXT_BYTES(outWidth));
    width = text_layout(fontSheet, text, xOffset, out, outWidth);
    if (xOffset + width > outWidth) {
        width = outWidth - xOffset;
    }
    return width > 0 ? width : 0;
}

/* A digit cut from a stock badge: its source image and ink columns. */
typedef struct GdxEkDigit {
    const unsigned char* image;
    int first;
    int last;
} GdxEkDigit;

/* Ink columns of image between columns from and to. Returns 0 when that span is blank. */
static int badge_ink(const unsigned char* image, int from, int to, int* first, int* last) {
    int x;
    int y;
    int found = 0;

    for (x = from; x <= to; x++) {
        for (y = 0; y < GDX_EK_BADGE_SIZE; y++) {
            if (i4_get(image, GDX_EK_BADGE_SIZE, x, y) != 0) {
                if (!found) {
                    *first = x;
                    found = 1;
                }
                *last = x;
                break;
            }
        }
    }
    return found;
}

static int badge_digit(const unsigned char* const badges[GDX_EK_BADGE_STOCK_MAX + 1], int digit, int tens,
                       GdxEkDigit* out) {
    int from;
    int to;

    if (tens && digit == 1) {
        out->image = badges[10]; /* the 1 of "10" */
        from = 0;
        to = GDX_EK_BADGE_UNITS_COLUMN - 1;
    } else if (digit <= 5) {
        out->image = badges[10 + digit]; /* the units digit of "10" to "15" */
        from = GDX_EK_BADGE_UNITS_COLUMN;
        to = GDX_EK_BADGE_SIZE - 1;
    } else {
        out->image = badges[digit]; /* the single-digit badge */
        from = 0;
        to = GDX_EK_BADGE_SIZE - 1;
    }
    return out->image != NULL && badge_ink(out->image, from, to, &out->first, &out->last);
}

static void badge_blit(unsigned char* out, const GdxEkDigit* digit, int destX) {
    int top = GDX_EK_BADGE_SIZE;
    int x;
    int y;

    for (y = 0; y < GDX_EK_BADGE_SIZE && top == GDX_EK_BADGE_SIZE; y++) {
        for (x = digit->first; x <= digit->last; x++) {
            if (i4_get(digit->image, GDX_EK_BADGE_SIZE, x, y) != 0) {
                top = y;
                break;
            }
        }
    }
    for (y = 0; y < GDX_EK_BADGE_SIZE; y++) {
        int dy = y + GDX_EK_BADGE_TOP_ROW - top;

        if (dy < 0 || dy >= GDX_EK_BADGE_SIZE) {
            continue;
        }
        for (x = digit->first; x <= digit->last; x++) {
            int dx = destX + (x - digit->first);
            int value = i4_get(digit->image, GDX_EK_BADGE_SIZE, x, y);

            /* Neighbouring digits of the stock badges touch; the brighter pixel wins where they meet. */
            if (value != 0 && dx >= 0 && dx < GDX_EK_BADGE_SIZE &&
                value > i4_get(out, GDX_EK_BADGE_SIZE, dx, dy)) {
                i4_set(out, GDX_EK_BADGE_SIZE, dx, dy, value);
            }
        }
    }
}

int gdx_ek_badge_compose(const unsigned char* const badges[GDX_EK_BADGE_STOCK_MAX + 1], int number,
                         unsigned char* out) {
    GdxEkDigit tens;
    GdxEkDigit units;
    int tensWidth;
    int total;
    int left;

    if (badges == NULL || out == NULL || number < 1 || number > GDX_EK_BADGE_MAX) {
        return 0;
    }
    if (number <= GDX_EK_BADGE_STOCK_MAX) {
        if (badges[number] == NULL) {
            return 0;
        }
        memcpy(out, badges[number], GDX_EK_BADGE_BYTES);
        return 1;
    }
    if (!badge_digit(badges, number / 10, 1, &tens) || !badge_digit(badges, number % 10, 0, &units)) {
        return 0;
    }
    tensWidth = tens.last - tens.first + 1;
    total = tensWidth + (units.last - units.first + 1);
    left = (GDX_EK_BADGE_SIZE - total) / 2;
    memset(out, 0, GDX_EK_BADGE_BYTES);
    badge_blit(out, &tens, left);
    badge_blit(out, &units, left + tensWidth);
    return 1;
}

int gdx_ek_marquee_offset(int frame, int range) {
    int period;
    int t;

    if (range <= 0 || frame < 0) {
        return 0;
    }
    period = GDX_EK_MARQUEE_HOLD + range * 2 + GDX_EK_MARQUEE_HOLD;
    t = frame % period;
    if (t < GDX_EK_MARQUEE_HOLD) {
        return 0;
    }
    t -= GDX_EK_MARQUEE_HOLD;
    if (t < range * 2) {
        return t / 2;
    }
    return range;
}
