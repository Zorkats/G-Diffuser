#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gdx_course_editor_layout.h"

typedef int Gfx;
typedef int s32;
typedef unsigned char u8;
static int D_xk1_80032BD8 = 1;
static const char* D_xk1_800331F0[] = {"", ": Load this file?", ": Overwrite?", ": Delete?"};
static int rectangles;
static int draws;
static int minX, maxX, minY, maxY;
static int textX[4], textY[4], textWidth[4];
static u8 drawn[4][256];

static void rectangle(int left, int top, int right, int bottom) {
    left /= 4;
    top /= 4;
    right /= 4;
    bottom /= 4;
    assert(left >= 0 && top >= 0 && right <= 320 && bottom < 118);
    assert(right > left && bottom > top);
    if (!rectangles || left < minX) minX = left;
    if (!rectangles || right > maxX) maxX = right;
    if (!rectangles || top < minY) minY = top;
    if (!rectangles || bottom > maxY) maxY = bottom;
    rectangles++;
}

#define gSPDisplayList(pkt, ...) ((void)(pkt))
#define gDPSetPrimColor(pkt, ...) ((void)(pkt))
#define gDPPipeSync(pkt) ((void)(pkt))
#define gSPTextureRectangle(pkt, l, t, r, b, ...) ((void)(pkt), rectangle(l, t, r, b))
#define mfsStrLen(s) ((int)strlen((const char*)(s)))
#define mfsStrCpy(d, s) strcpy((char*)(d), (const char*)(s))
#define GDX_EK_FILE_NAME_MAX 20

static void func_xk1_80026830(char* src, u8* dst) {
    while (*src) {
        *dst++ = 0x82;
        *dst++ = (u8)*src++;
    }
    *dst = 0;
}

static int GdxGlyphStringWidth(u8* text) {
    int width = 0;
    while (*text) {
        if (*text & 0x80) {
            assert(text[1]);
            text += 2;
            width += 16;
        } else {
            text++;
            width += text[-1] == 'i' ? 4 : 8;
        }
    }
    return width;
}

static Gfx* func_xk1_800262F4(Gfx* gfx, int x, int y, u8* text) {
    assert(draws < 4);
    textX[draws] = x;
    textY[draws] = y;
    textWidth[draws] = GdxGlyphStringWidth(text);
    strcpy((char*)drawn[draws], (char*)text);
    draws++;
    return gfx;
}

static Gfx* func_xk1_800264C0(Gfx* gfx, int x, int y, int index) {
    return func_xk1_800262F4(gfx, x, y, (u8*)D_xk1_800331F0[index]);
}

#include "../../decomp/src/overlays/expansion_kit/course_file_prompt.inc"

// The English count rows as 191080.c measures them: Size before the node counter, the gadget
// counts before theirs. Size is the tightest.
static int englishLabelLimit(void) {
    const char* labels[] = {"Size", "Boost", "Object", "Jump", "Trap"};
    const int fields[] = {GDX_COURSE_INFO_SIZE_FIELD_X, 0x108, 0x108, 0x10E, 0x10E};
    int limit = GDX_COURSE_INFO_RIGHT;
    int i;
    for (i = 0; i < 5; i++) {
        int rowLimit = gdx_course_info_label_limit((const u8*)labels[i], 16, fields[i]);
        if (rowLimit < limit) limit = rowLimit;
    }
    return limit;
}

int main(void) {
    const int labelLimit = englishLabelLimit();
    const char* names[] = {"", "A", "DAMNATION", "WARIO-COLOSSEUM", "WARIO-COLOSSEUM ZORK", "ABCDEFGHIJKLMNOPQRS", "ABCDEFGHIJKLMN"};
    Gfx commands[256];
    int n, action, i;
    for (n = 0; n < (int)(sizeof(names) / sizeof(names[0])); n++) {
        for (action = 0; action < 3; action++) {
            Gfx* gfx = commands;
            u8 recovered[64] = {0};
            u8 expected[64];
            rectangles = draws = 0;
            func_xk1_8002C720(&gfx, 0, 72, (char*)names[n], action);
            assert(rectangles == 5 && draws >= 2 && draws <= 3);
            for (i = 0; i < draws; i++) {
                assert(textX[i] >= minX + 2 && textX[i] + textWidth[i] <= maxX - 2);
                assert(textY[i] >= minY + 2 && textY[i] + 16 <= maxY - 2);
                if (i < draws - 1) strcat((char*)recovered, (char*)drawn[i]);
            }
            func_xk1_80026830((char*)names[n], expected);
            assert(strcmp((char*)recovered, (char*)expected) == 0);
            assert(strcmp((char*)drawn[draws - 1], D_xk1_800331F0[action + 1]) == 0);
            if (n == 1) assert(textY[0] == textY[1] && minY == 70);
            if (draws > 2) assert(minY >= 62);
            if (n == 4) assert(draws == 3 && textY[0] < textY[1] && textY[1] < textY[2]);
        }
    }
    {
        char full[20];
        Gfx* gfx = commands;
        memset(full, 'W', sizeof(full));
        rectangles = draws = 0;
        func_xk1_8002C720(&gfx, 0, 72, full, 0);
        assert(draws == 3 && strlen((char*)drawn[0]) + strlen((char*)drawn[1]) == 40);
        assert(gdx_course_info_left((u8*)full, 20, labelLimit) == 125);
    }
    // Size ends 2 pixels before the node counter, and a four-character Japanese label measures the same.
    assert(labelLimit == 198);
    assert(gdx_course_info_label_limit((const u8*)"\x99\xB2\xDD\xC4", 16, GDX_COURSE_INFO_SIZE_FIELD_X) == labelLimit);
    // Short titles: the label rows set the width. Long ones: the title does.
    assert(gdx_course_info_left((u8*)"", 20, labelLimit) == 198);
    assert(gdx_course_info_left((u8*)"A", 20, labelLimit) == 198);
    assert(gdx_course_info_left((u8*)"DAMNATION", 20, labelLimit) == 198);
    assert(gdx_course_info_left((u8*)"WARIO-COLOSSEUM", 20, labelLimit) == 165);
    assert(gdx_course_info_left((u8*)"ABCDEFGHIJKLMNOPQRS", 20, labelLimit) == 133);
    for (n = 0; n <= 20; n++) {
        u8 title[21] = {0};
        int left;
        memset(title, 'W', n);
        left = gdx_course_info_left(title, 20, labelLimit);
        assert(left >= 125 && left <= 198);
        // The title ends on the length row's "M" and stays inside the panel.
        assert(gdx_course_info_title_x(title, 20) >= left + GDX_COURSE_INFO_PAD);
        assert(gdx_course_info_title_x(title, 20) + n * 8 == GDX_COURSE_INFO_TITLE_RIGHT);
        // Every label, drawn at the text column, ends before its count field.
        assert(left + GDX_COURSE_INFO_PAD + 4 * 8 <= GDX_COURSE_INFO_SIZE_FIELD_X - GDX_COURSE_INFO_LABEL_GAP);
        assert(left + GDX_COURSE_INFO_PAD + 6 * 8 <= 0x108 - GDX_COURSE_INFO_LABEL_GAP);
        // With a short title the box is tight: Size sits right against the counter.
        if (n <= 9) assert(left + GDX_COURSE_INFO_PAD + 4 * 8 == GDX_COURSE_INFO_SIZE_FIELD_X - GDX_COURSE_INFO_LABEL_GAP);
    }
    assert(gdx_course_info_title_x((u8*)"WARIO-COLOSSEUM ZORK", 20) == 133);
    {
        const char* original = D_xk1_800331F0[1];
        const int positions[] = {54, 72, 90};
        D_xk1_800331F0[1] = "\x82\xA0\x82\xA2\x82\xA4\x82\xA6";
        for (n = 0; n < 3; n++) {
            Gfx* gfx = commands;
            rectangles = draws = 0;
            func_xk1_8002C720(&gfx, 0, positions[n], "WARIO-COLOSSEUM ZORK", 0);
            assert(draws == 3 && minY == 62 && maxY == 114);
            assert(textWidth[2] == 64);
        }
        D_xk1_800331F0[1] = original;
    }
    D_xk1_80032BD8 = 0;
    {
        Gfx* gfx = commands;
        rectangles = draws = 0;
        func_xk1_8002C720(&gfx, 0, 72, "A", 0);
        assert(gfx == commands && !rectangles && !draws);
    }
    puts("All native Course Edit layout tests passed");
    return 0;
}
