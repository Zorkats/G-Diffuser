/* port/gdx_net_race_box.h -- the quit box of a G-NET room's race (native room, slice R5).
 * Copyright (c) Zorkats
 *
 * A room's race has no pause, and nobody can end it for the others. What every racer has, the host
 * included, is a small box in the style of the stock Retry box: START opens it while the race runs on
 * behind it, and it comes up by itself a little after the copy's own machine crashed or retired. Its rows:
 *
 *   CONTINUE          closes the box. It reads WATCH once the machine has finished or is out.
 *   RETURN TO ROOM    leaves the race for the room screen. The machine is retired for the room unless it
 *                     had finished, and the player waits there for the results.
 *   LEAVE THE ROOM    leaves the room as well, for the front page of the room screen, where the rooms
 *                     are. A client only: the host closes the room from the room screen, where that is
 *                     asked twice.
 *
 * One press of A or START chooses, as in the stock box, and B closes. While the box is open the stick and
 * the buttons belong to it: the copy's own machine gets no input and coasts (gdx_net_race_box_holds_input).
 *
 * The room and the race decide everything. This module keeps the box: open or closed, the cursor, and
 * what a press does. The five functions it calls (gdx_lan_race_room, gdx_net_race_own_state,
 * gdx_net_race_quit, gdx_lan_room_stop, gdx_net_room_screen_want_front) are its whole view of the game,
 * so the tests replace them.
 *
 * The functions take and return plain numbers: the game's menu code is built without the port's headers
 * and declares them itself (decomp/src/overlays/ovl_i3/menus.c).
 *
 * Game thread only. */

#ifndef GDX_NET_RACE_BOX_H
#define GDX_NET_RACE_BOX_H

#ifdef __cplusplus
extern "C" {
#endif

/* The most rows the box has. */
#define GDX_NET_RACE_BOX_ROWS 3

/* Frames after the box opens before it takes a press: the press that opened it is not also an answer. */
#define GDX_NET_RACE_BOX_OPEN_FRAMES 12

/* Frames after the copy's own machine is out before the box comes up by itself. The stock Retry box
 * waits as long (sPlayerRetireTimer in menus.c). */
#define GDX_NET_RACE_BOX_AUTO_FRAMES 200

/* The cursor: how far the stick must lean, how long it is held before it repeats, and the frames between
 * repeats. The stock menus use the same three numbers. */
#define GDX_NET_RACE_BOX_STICK 50
#define GDX_NET_RACE_BOX_REPEAT_AFTER 60
#define GDX_NET_RACE_BOX_REPEAT_EVERY 16

/* What a frame did, for the sound. */
enum {
    GDX_NET_RACE_BOX_NOTHING = 0,
    GDX_NET_RACE_BOX_OPENED = 1,
    GDX_NET_RACE_BOX_MOVED = 2,  /* the cursor went to another row */
    GDX_NET_RACE_BOX_CLOSED = 3, /* the first row or B: back to the race */
    GDX_NET_RACE_BOX_LEFT = 4    /* this copy leaves the race */
};

/* One frame of a room's race. start, accept, back, up and down: that button went down on this frame.
 * stickY: the stick as the controller gives it, up positive. Returns GDX_NET_RACE_BOX_*. Outside a race
 * that runs (before GO, after the end, in no room) the box is closed and nothing opens it. */
int gdx_net_race_box_frame(int start, int accept, int back, int up, int down, int stickY);

/* The rows to draw: 0 while the box is closed, 2 on the host's copy, 3 on a client's. */
int gdx_net_race_box_rows(void);

/* The row the cursor is on, 0 at the top. */
int gdx_net_race_box_cursor(void);

/* A row's words, in characters the small stock font has. "" for a row the box does not have. */
const char* gdx_net_race_box_text(int row);

/* 1 while the box is open: the copy's own machine gets no controller input. */
int gdx_net_race_box_holds_input(void);

/* Closes the box and forgets the race it belonged to. The race bridge calls it when a race is set up and
 * when this copy leaves one. */
void gdx_net_race_box_reset(void);

#ifdef __cplusplus
}
#endif

#endif
