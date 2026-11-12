/* port/gdx_net_race_box.c -- the quit box of a G-NET room's race. See gdx_net_race_box.h.
 * Copyright (c) Zorkats */

#include "gdx_net_race_box.h"
#include "gdx_net_race_words.h"

#include "gdx_net_race_room.h"
#include "gdx_net_room_screen.h"
#include "gdx_net_room_view.h"

/* The race bridge (gdx_net_race_bridge.c), which has no header of its own. */
extern int gdx_net_race_own_state(void); /* 0 the machine drives, 1 it finished, 2 it crashed or retired */
extern void gdx_net_race_quit(void);     /* the next frame leaves the race for the room screen */
extern void gdx_net_race_hand_over(void); /* the same, and then the host's room goes to the next host */

enum { ROW_STAY = 0, ROW_ROOM = 1, ROW_LEAVE = 2 };
enum { OWN_DRIVES = 0, OWN_OUT = 2 };

static int sOpen;
static int sWait;         /* frames until the box takes a press */
static int sCursor;
static int sRows;         /* the rows of the box as it was last drawn up */
static int sOwn;          /* what became of our machine, as of the last frame */
static int sRaceKnown;
static unsigned sRace;    /* the race the state below belongs to */
static int sOutFrames;    /* frames since our machine went out, up to the time the box comes up */
static int sAutoSpent;    /* the box has been up since our machine went out: it does not come up again */
static int sHold;         /* frames the cursor has been pushed the same way */
static int sHoldDir;

static void close_box(void) {
    sOpen = 0;
    sWait = 0;
    sCursor = 0;
    sHold = 0;
    sHoldDir = 0;
}

void gdx_net_race_box_reset(void) {
    close_box();
    sRows = 0;
    sOwn = OWN_DRIVES;
    sRaceKnown = 0;
    sRace = 0;
    sOutFrames = 0;
    sAutoSpent = 0;
}

static void open_box(void) {
    sOpen = 1;
    sWait = GDX_NET_RACE_BOX_OPEN_FRAMES;
    sCursor = 0;
    if (sOwn == OWN_OUT) {
        sAutoSpent = 1;
    }
}

/* Which way the cursor goes this frame: -1 up, 1 down, 0 nowhere. A push moves it once, then again
 * after it has been held a while, then at a steady pace. */
static int cursor_step(int up, int down, int stickY) {
    int dir = 0;

    if (stickY > GDX_NET_RACE_BOX_STICK) {
        dir = -1;
    } else if (stickY < -GDX_NET_RACE_BOX_STICK) {
        dir = 1;
    }
    if (dir != 0) {
        sHold = (dir == sHoldDir) ? sHold + 1 : 1;
    } else if (up || down) {
        dir = up ? -1 : 1;
        sHold = 1;
    } else {
        sHold = 0;
    }
    sHoldDir = dir;
    if (sHold >= GDX_NET_RACE_BOX_REPEAT_AFTER + GDX_NET_RACE_BOX_REPEAT_EVERY) {
        sHold = GDX_NET_RACE_BOX_REPEAT_AFTER;
    }
    return (sHold == 1 || sHold == GDX_NET_RACE_BOX_REPEAT_AFTER) ? dir : 0;
}

int gdx_net_race_box_frame(int start, int accept, int back, int up, int down, int stickY) {
    GdxNetRaceRoom room;
    int step;

    gdx_lan_race_room(&room);
    if (!room.inRoom || !room.onGrid || room.phase != GDX_NET_RACE_PHASE_RACING) {
        gdx_net_race_box_reset();
        return GDX_NET_RACE_BOX_NOTHING;
    }
    if (!sRaceKnown || sRace != room.raceIndex) {
        gdx_net_race_box_reset(); /* a box left over from another race is not this race's */
        sRaceKnown = 1;
        sRace = room.raceIndex;
    }
    sRows = GDX_NET_RACE_BOX_ROWS;
    sOwn = gdx_net_race_own_state();

    /* The stick is followed while the box is closed or opening too, so a stick that was already leaning
       when the box came up does not move the cursor at once. Its hold stays short until the box takes a
       press: a lean held through the 60 frames of opening would otherwise reach its repeat just then. */
    step = cursor_step(up, down, stickY);
    if ((!sOpen || sWait > 0) && sHold > 2) {
        sHold = 2;
    }

    if (sOwn == OWN_OUT && !sAutoSpent) {
        if (sOpen) {
            sAutoSpent = 1; /* the box was up when the machine went out */
        } else if (++sOutFrames >= GDX_NET_RACE_BOX_AUTO_FRAMES) {
            open_box();
            return GDX_NET_RACE_BOX_OPENED;
        }
    }
    if (!sOpen) {
        if (start) {
            open_box();
            return GDX_NET_RACE_BOX_OPENED;
        }
        return GDX_NET_RACE_BOX_NOTHING;
    }
    if (sWait > 0) {
        sWait--;
        return GDX_NET_RACE_BOX_NOTHING;
    }
    if (back) {
        close_box();
        return GDX_NET_RACE_BOX_CLOSED;
    }
    if (accept || start) {
        const int row = sCursor;

        close_box();
        if (row == ROW_STAY) {
            return GDX_NET_RACE_BOX_CLOSED;
        }
        if (row == ROW_LEAVE && room.host) {
            /* The room goes on without this copy: the race reports its machine to the room first, then hands
               the room over (gdx_net_race_bridge.c). */
            gdx_net_room_screen_want_front();
            gdx_net_race_hand_over();
            return GDX_NET_RACE_BOX_LEFT;
        }
        if (row == ROW_LEAVE) {
            gdx_lan_room_stop(); /* the room first: a copy in no room has nothing to report to it */
            gdx_net_room_screen_want_front(); /* and on to the page with the rooms, not the mode cards */
        }
        gdx_net_race_quit();
        return GDX_NET_RACE_BOX_LEFT;
    }
    if (step != 0) {
        sCursor = (sCursor + step + sRows) % sRows;
        return GDX_NET_RACE_BOX_MOVED;
    }
    return GDX_NET_RACE_BOX_NOTHING;
}

int gdx_net_race_box_rows(void) {
    return sOpen ? sRows : 0;
}

int gdx_net_race_box_opening(void) {
    return sOpen ? sWait : 0;
}

int gdx_net_race_box_cursor(void) {
    return sCursor;
}

const char* gdx_net_race_box_text(int row) {
    if (row < 0 || row >= sRows) {
        return "";
    }
    switch (row) {
        case ROW_STAY:
            return sOwn == OWN_DRIVES ? "CONTINUE" : "WATCH";
        case ROW_ROOM:
            return "RETURN TO ROOM";
        default:
            return "LEAVE THE ROOM";
    }
}

int gdx_net_race_box_word(int row) {
    if (row < 0 || row >= sRows) {
        return GDX_NET_RACE_WORD_NONE;
    }
    switch (row) {
        case ROW_STAY:
            return sOwn == OWN_DRIVES ? GDX_NET_RACE_WORD_CONTINUE : GDX_NET_RACE_WORD_WATCH;
        case ROW_ROOM:
            return GDX_NET_RACE_WORD_RETURN_TO_ROOM;
        default:
            return GDX_NET_RACE_WORD_LEAVE_THE_ROOM;
    }
}

int gdx_net_race_box_holds_input(void) {
    return sOpen;
}
