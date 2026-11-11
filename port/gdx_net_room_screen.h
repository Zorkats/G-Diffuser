/* port/gdx_net_room_screen.h -- the G-NET room screen of the main menu, game side.
 * Copyright (c) Zorkats
 *
 * The G-NET card of the main menu opens this screen. On it a player hosts a room, finds rooms on the network
 * or joins one by address, and then sees the room: its players, its state, the table of its race, its
 * messages and the rows that act on it. It is drawn with the game's own font and worked with the controller.
 *
 * This module is the whole screen except the drawing calls. Each tick it reads the room (gdx_net_room_view.h),
 * takes the buttons, and keeps which page and row the player is on. Each frame it hands the game a list of
 * texts and rectangles in 320 x 240 screen pixels; ovl_i6/main_menu.c draws them and knows nothing else. The
 * room and the settings are the functions of gdx_net_room_view.h, so the tests replace them.
 *
 * The draw list and the limits of the two stock fonts are in gdx_net_text.h.
 *
 * Game thread only. */

#ifndef GDX_NET_ROOM_SCREEN_H
#define GDX_NET_ROOM_SCREEN_H

#include "gdx_net_room_view.h"
#include "gdx_net_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The buttons of one tick. Directions repeat while held, as the stock menus have them; the other two are
 * presses. */
#define GDX_NET_ROOM_IN_UP 0x01u
#define GDX_NET_ROOM_IN_DOWN 0x02u
#define GDX_NET_ROOM_IN_LEFT 0x04u
#define GDX_NET_ROOM_IN_RIGHT 0x08u
#define GDX_NET_ROOM_IN_CONFIRM 0x10u
#define GDX_NET_ROOM_IN_BACK 0x20u

/* gdx_net_room_screen_update */
#define GDX_NET_ROOM_SCREEN_STAY 0
#define GDX_NET_ROOM_SCREEN_LEAVE 1 /* back to the cards of the main menu */

/* gdx_net_room_screen_take_sound */
#define GDX_NET_ROOM_SOUND_NONE 0
#define GDX_NET_ROOM_SOUND_MOVE 1
#define GDX_NET_ROOM_SOUND_CONFIRM 2
#define GDX_NET_ROOM_SOUND_BACK 3
#define GDX_NET_ROOM_SOUND_REFUSE 4

/* gdx_net_room_screen_page */
#define GDX_NET_ROOM_PAGE_FRONT 0   /* host, find, join by address, port */
#define GDX_NET_ROOM_PAGE_ADDRESS 1 /* join by address */
#define GDX_NET_ROOM_PAGE_ROOMS 2   /* the rooms found */
#define GDX_NET_ROOM_PAGE_FOUND 3   /* one of them */
#define GDX_NET_ROOM_PAGE_JOINING 4
#define GDX_NET_ROOM_PAGE_ROOM 5     /* in a room: its rows, and its players or the table of its race */
#define GDX_NET_ROOM_PAGE_PLAYER 6   /* one other player: mute, and for the host remove */
#define GDX_NET_ROOM_PAGE_CLOSED 7   /* the session ended on its own */
#define GDX_NET_ROOM_PAGE_MESSAGES 8 /* the room's last messages and the ones to send */

#define GDX_NET_ROOM_DRAW_MAX 96
/* A paragraph breaks at this many characters. A capital of the small font is at most 7 pixels wide and a
 * space is 3, so a line of words stays inside the 280 pixels between the screen's margins. */
#define GDX_NET_ROOM_WRAP_COLUMNS 40

/* 1 while a session exists: searching, joining, in a room, or one that ended and has not been read. The main
 * menu then shows this screen and not its cards. Also 1 after gdx_net_room_screen_want_front, until the
 * screen is opened. */
int gdx_net_room_screen_wanted(void);

/* The player left a room from inside its race (the quit box, gdx_net_race_box.h). The main menu that
 * follows comes up on this screen's front page, where the rooms are, and not on its cards. Once: opening
 * the screen forgets it. */
void gdx_net_room_screen_want_front(void);

/* The main menu shows the screen from now: the cursor goes to the top of whatever page the session gives. */
void gdx_net_room_screen_open(void);

/* Once per tick while the screen is up, with the GDX_NET_ROOM_IN_* buttons of that tick. */
int gdx_net_room_screen_update(unsigned input);

/* The sound the last update asked for, once. */
int gdx_net_room_screen_take_sound(void);

/* What to draw this frame, back to front. Valid until the next call. */
const GdxNetRoomDrawItem* gdx_net_room_screen_draw(int* count);

/* For the tests. The row is counted from the top of the page's rows, lists included. */
int gdx_net_room_screen_page(void);
int gdx_net_room_screen_row(void);
int gdx_net_room_screen_editing(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_ROOM_SCREEN_H */
