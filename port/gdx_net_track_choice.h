/* port/gdx_net_track_choice.h -- the G-NET vote and pick, on the stock course select (O3 slice R2).
 * Copyright (c) Zorkats
 *
 * A room chooses its next track in one of two ways (net/gdx_net_room.h), by the host's course switch. By
 * default every racer votes and a lottery among the ballots decides; with the switch on, the racer who finished
 * last picks. Either way the player chooses on the stock course select: the four cups of the first row hold the
 * 24 stock tracks, the two 64DD cups of the second row hold the 12 disk tracks, and the place of the X Cup is
 * the ballot that lets the room draw. The second OK sends the choice. A voter then
 * stays on the screen, which shows the votes so far and, when the vote closes, turns to the track the lottery
 * drew. A ballot for a custom track this copy lacks counts in no cup, so the line of the votes so far says how
 * many there are.
 *
 * When the host allows custom tracks the Edit Cup is on the cup screen too, between the two 64DD cups, and each
 * of its tracks that a room can race may be chosen (gdx_net_own_tracks.h). Without them its place is a gap
 * that the cursor skips. A custom track the room draws shows
 * its title on every copy, also on those that lack it.
 *
 * B on the cups goes back to the room screen without choosing. The room screen has a row that opens this
 * screen again while the choice is still open.
 *
 * The room decides everything. This module tracks whether the course select is the room's, turns the room's
 * numbers into the lines the course select draws, and never sees a socket: gdx_net_room_view.h and the one
 * menu function below are its whole view of the world, so the tests replace them.
 *
 * Game thread only. */

#ifndef GDX_NET_TRACK_CHOICE_H
#define GDX_NET_TRACK_CHOICE_H

#include "gdx_net_room_view.h"
#include "gdx_net_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* gdx_net_track_choice_available */
#define GDX_NET_TRACK_CHOICE_NONE 0
#define GDX_NET_TRACK_CHOICE_VOTE 1
#define GDX_NET_TRACK_CHOICE_PICK 2

/* gdx_net_track_choice_update */
#define GDX_NET_TRACK_CHOICE_CHOOSE 0 /* the stock screen runs: the player browses and confirms */
#define GDX_NET_TRACK_CHOICE_WAIT 1   /* the choice is sent: no input */
#define GDX_NET_TRACK_CHOICE_SHOW 2   /* the lottery is over: turn to gdx_net_track_choice_shown_track() */
#define GDX_NET_TRACK_CHOICE_LEAVE 3  /* nothing left to choose: on to the machine select when the room's is
                                         open (gdx_net_track_choice_hold), else back to the room screen */
#define GDX_NET_TRACK_CHOICE_HOLD 4   /* held: the screen is on its way to the machine select, no input */

/* The X Cup's place on the cup screen. */
#define GDX_NET_TRACK_CHOICE_RANDOM_CUP 4
/* The Edit Cup's place, the first of the second row, and the number of its first track (COURSE_EDIT_1). */
#define GDX_NET_TRACK_CHOICE_EDIT_CUP 10
#define GDX_NET_TRACK_CHOICE_EDIT_TRACK 24
/* The places of the two 64DD cups, left and right of the Edit Cup, and their first tracks (COURSE_SILENCE_3
 * and the first of the second disk cup). */
#define GDX_NET_TRACK_CHOICE_DD_1_CUP 11
#define GDX_NET_TRACK_CHOICE_DD_2_CUP 12
#define GDX_NET_TRACK_CHOICE_DD_1_TRACK 30
#define GDX_NET_TRACK_CHOICE_DD_2_TRACK 36

/* The port menu steps aside for the course select (gdx_menu.cpp). */
void gdx_menu_request_close(void);

/* What the local player may choose in this room now: a ballot they have not cast yet, the pick that is
 * theirs, or nothing. */
int gdx_net_track_choice_available(const GdxNetRoomView* view);

/* Main menu, on its room screen, once per tick: 1 when the course select opens now as the room's. A player
 * who went back from it is not taken there again unless they ask (gdx_net_track_choice_request). Calling it
 * also records that the screen is not up, which is true whenever the main menu runs. */
int gdx_net_track_choice_poll_open(void);
/* The room screen's row: open the course select again at the next poll. */
void gdx_net_track_choice_request(void);

/* 1 while the course select is the room's and not the stock one. */
int gdx_net_track_choice_active(void);
/* 1 while it is the room's and the room takes custom tracks: the Edit Cup is on the cup screen. */
int gdx_net_track_choice_custom_tracks(void);
/* The course select is not the room's from here on. */
void gdx_net_track_choice_close(void);
/* B on the cups: the player goes back to the room screen without choosing. */
void gdx_net_track_choice_back(void);
/* The screen leaves for the room's machine select. It stays the room's until it is closed or the main menu
 * polls again, and it draws what it drew last, so it does not change while it fades out. */
void gdx_net_track_choice_hold(void);

/* Course select, once per tick while active: one of GDX_NET_TRACK_CHOICE_*. After a LEAVE the lines and the
 * shown track stay those of the update before it. */
int gdx_net_track_choice_update(void);
/* The track the room races next after an update that said SHOW: 0 to 23, 30 to 41 for a 64DD track, 24 to 29
 * for a track of this copy's Edit Cup, or -1 for a custom track this copy lacks, which only
 * gdx_net_track_choice_foreign_title names. */
int gdx_net_track_choice_shown_track(void);
/* The title of the custom track shown, in the title font, when this copy lacks it; NULL otherwise. The course
 * select draws it where the track's name goes. */
const char* gdx_net_track_choice_foreign_title(void);

/* 1 when the room races this track, so it can be chosen. track is 0 to 23 or 30 to 41, taken when the room's
 * pool has it, GDX_NET_ROOM_TRACK_RANDOM, which a room always takes, or 24 to 29, a track of the Edit Cup,
 * which a room takes when it takes custom tracks and the track can be read and sent. */
int gdx_net_track_choice_allowed(int track);
/* The second OK: sends the ballot or the pick. 1 when it was sent. A choice is final, so a second call for
 * the same choice does nothing and returns 0. */
int gdx_net_track_choice_confirm(int track);

/* What the X Cup's place is called on the course select, in the title font. */
const char* gdx_net_track_choice_random_title(void);

/* The room's lines for this frame of the course select, texts only, as of the last update. cup is the place
 * the cursor is on, 0 to 4, GDX_NET_TRACK_CHOICE_EDIT_CUP or a 64DD cup's place. trackNo is the track of that
 * cup on screen, 0 to 5, or -1 on the cup screen. Valid until the next call. */
const GdxNetRoomDrawItem* gdx_net_track_choice_draw(int cup, int trackNo, int* count);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_TRACK_CHOICE_H */
