/* port/gdx_net_text.h -- what a G-NET screen hands the game to draw, and text fitted to the stock fonts.
 * Copyright (c) Zorkats
 *
 * The G-NET screens are drawn with the game's own fonts. Their models (gdx_net_room_screen.h,
 * gdx_net_track_choice.h) know nothing of the display list: each frame they hand the game a list of texts
 * and rectangles in 320 x 240 screen pixels, and the game's overlay draws that list.
 *
 * The two stock fonts used draw capitals, digits and a space. The small one also has ' " & + - and a period,
 * the title one ' & - : . and a comma. A small letter comes out as katakana in the small font, a missing sign
 * as an "A", and any other character ends the string. So every text leaves a model through the filter below,
 * which makes letters capitals and turns what the font lacks into a space, and the screens' own texts are
 * written with what the small font has. A clock has a colon, so it goes in the title font. */

#ifndef GDX_NET_TEXT_H
#define GDX_NET_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_ROOM_DRAW_TEXT 0
#define GDX_NET_ROOM_DRAW_RECT 1

#define GDX_NET_ROOM_FONT_SMALL 0 /* FONT_SET_4: 8 pixel capitals */
#define GDX_NET_ROOM_FONT_TITLE 1 /* FONT_SET_3: 16 pixel capitals */

#define GDX_NET_ROOM_ALIGN_LEFT 0
#define GDX_NET_ROOM_ALIGN_CENTRE 1
#define GDX_NET_ROOM_ALIGN_RIGHT 2

#define GDX_NET_ROOM_COLOUR_WHITE 0
#define GDX_NET_ROOM_COLOUR_DIM 1
#define GDX_NET_ROOM_COLOUR_GOLD 2
#define GDX_NET_ROOM_COLOUR_RED 3
#define GDX_NET_ROOM_COLOUR_GREEN 4
#define GDX_NET_ROOM_COLOUR_CURSOR 5 /* the cycling highlight of the stock menus */

#define GDX_NET_ROOM_TEXT_BYTES 48

typedef struct GdxNetRoomDrawItem {
    unsigned char kind;
    unsigned char font;
    unsigned char align;
    unsigned char colour; /* text only */
    short x;              /* text: the left edge, the middle or the right edge, by align */
    short y;              /* text: the row its glyphs stand on. rectangle: the top */
    short w;              /* rectangle only */
    short h;
    unsigned rgba; /* rectangle only: 0xRRGGBBAA */
    char text[GDX_NET_ROOM_TEXT_BYTES];
} GdxNetRoomDrawItem;

/* Copies in to out as the font (GDX_NET_ROOM_FONT_*) draws it: small letters become capitals and every
 * character the font lacks becomes a space. Always NUL-terminated. */
void gdx_net_text_filter(int font, const char* in, char* out, int size);
/* Breaks text at spaces into lines of at most columns characters, each NUL-terminated in lineBytes bytes of
 * out. A '\n' in text ends a line where it stands. A word longer than a line is cut. Returns the number of
 * lines, at most maxLines; the rest is dropped. */
int gdx_net_text_wrap(const char* text, int columns, char* out, int lineBytes, int maxLines);
/* "0:42" from the time left, rounded up to a second. */
void gdx_net_text_format_timer(unsigned leftMs, char* out, int size);
/* A track's name as the game stores it ("devil's forest 2") with a capital at each word. The game writes the
 * two Sector tracks with a Greek letter in two bytes of its own encoding, which no screen of the port and
 * neither stock font used here can draw: they come out as "Sector Alpha" and "Sector Beta". Always
 * NUL-terminated. */
void gdx_net_text_track_name(const char* stock, char* out, int size);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_TEXT_H */
