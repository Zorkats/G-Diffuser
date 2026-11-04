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

int main(void) {
    assert(gdx_course_info_left((u8*)"A", 20) == 168);
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
            if (n == 1) assert(textY[0] == textY[1]);
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
        assert(gdx_course_info_left((u8*)full, 20) == 120);
    }
    assert(gdx_course_info_left((u8*)"A", 20) == 168);
    assert(gdx_course_info_left((u8*)"", 20) == 168);
    assert(gdx_course_info_left((u8*)"WARIO-COLOSSEUM", 20) == 160);
    assert(gdx_course_info_left((u8*)"ABCDEFGHIJKLMNOPQRS", 20) == 128);
    for (n = 0; n <= 20; n++) {
        u8 title[21] = {0};
        int left;
        memset(title, 'W', n);
        left = gdx_course_info_left(title, 20);
        assert(left >= 120 && left <= 168);
        assert(left + 8 + n * 8 <= 288);
        assert(left + 8 + 6 * 8 < 240);
    }
    {
        const char* original = D_xk1_800331F0[1];
        const int positions[] = {54, 72, 90};
        D_xk1_800331F0[1] = "\x82\xA0\x82\xA2\x82\xA4\x82\xA6";
        for (n = 0; n < 3; n++) {
            Gfx* gfx = commands;
            rectangles = draws = 0;
            func_xk1_8002C720(&gfx, 0, positions[n], "WARIO-COLOSSEUM ZORK", 0);
            assert(draws == 3 && maxY == positions[n] + 18);
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
