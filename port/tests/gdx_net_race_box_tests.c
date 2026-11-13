/* port/tests/gdx_net_race_box_tests.c -- unit tests for the quit box of a room's race (gdx_net_race_box.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_race_box.c and gdx_net_race_words.c. The room and the race are the six functions below. Expected
 * values are written out by hand from the header.
 *
 * The module keeps the box between calls, so every test starts from a reset and a race number of its own.
 */

#include "../gdx_net_race_box.h"
#include "../gdx_net_race_words.h"
#include "../gdx_net_race_room.h"
#include "../gdx_net_room_view.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

/* The room and the race, as the module sees them. */
static GdxNetRaceRoom sFakeRoom;
static int sOwnState;
static int sQuitCalls;
static int sStopCalls;
static int sFrontCalls;  /* the main menu was asked to come up on the front page */
static int sCalls;       /* the quit and the stop, in the order they came */
static int sStopWasCall; /* which call the room's stop was, 1 for the first */
static int sQuitWasCall;
static int sHandOverCalls;

void gdx_lan_race_room(GdxNetRaceRoom* out) {
    *out = sFakeRoom;
}

int gdx_net_race_own_state(void) {
    return sOwnState;
}

void gdx_net_race_quit(void) {
    sQuitCalls++;
    sQuitWasCall = ++sCalls;
}

void gdx_net_race_hand_over(void) {
    sHandOverCalls++;
}

void gdx_lan_room_stop(void) {
    sStopCalls++;
    sStopWasCall = ++sCalls;
}

void gdx_net_room_screen_want_front(void) {
    sFrontCalls++;
}

enum { DRIVES = 0, FINISHED = 1, OUT = 2 };

static void racing(unsigned raceIndex, int host) {
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    sFakeRoom.inRoom = 1;
    sFakeRoom.host = host;
    sFakeRoom.phase = GDX_NET_RACE_PHASE_RACING;
    sFakeRoom.raceIndex = raceIndex;
    sFakeRoom.onGrid = 1;
    sOwnState = DRIVES;
    sQuitCalls = sStopCalls = sFrontCalls = sCalls = sStopWasCall = sQuitWasCall = sHandOverCalls = 0;
    gdx_net_race_box_reset();
}

/* One frame with one thing pressed. */
static int idle(void) {
    return gdx_net_race_box_frame(0, 0, 0, 0, 0, 0);
}
static int start(void) {
    return gdx_net_race_box_frame(1, 0, 0, 0, 0, 0);
}
static int choose(void) {
    return gdx_net_race_box_frame(0, 1, 0, 0, 0, 0);
}
static int back(void) {
    return gdx_net_race_box_frame(0, 0, 1, 0, 0, 0);
}
static int up(void) {
    return gdx_net_race_box_frame(0, 0, 0, 1, 0, 0);
}
static int down(void) {
    return gdx_net_race_box_frame(0, 0, 0, 0, 1, 0);
}
static int stick(int y) {
    return gdx_net_race_box_frame(0, 0, 0, 0, 0, y);
}

/* Runs frames with nothing pressed and returns how many of them did something. */
static int idle_frames(int frames) {
    int events = 0;
    int i;

    for (i = 0; i < frames; i++) {
        events += idle() != GDX_NET_RACE_BOX_NOTHING;
    }
    return events;
}

/* Opens the box with START and lets the frames pass in which it takes no press. */
static void open_ready(void) {
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(idle_frames(GDX_NET_RACE_BOX_OPEN_FRAMES) == 0);
}

static void test_closed_until_start(void) {
    racing(10, 0);
    CHECK(idle_frames(600) == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    /* Nothing but START opens it. */
    CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(back() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(up() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(down() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(stick(80) == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
}

static void test_start_opens_it(void) {
    racing(11, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(gdx_net_race_box_holds_input() == 1);
    CHECK(strcmp(gdx_net_race_box_text(0), "CONTINUE") == 0);
    CHECK(strcmp(gdx_net_race_box_text(1), "RETURN TO ROOM") == 0);
    CHECK(strcmp(gdx_net_race_box_text(2), "LEAVE THE ROOM") == 0);
    CHECK(strcmp(gdx_net_race_box_text(3), "") == 0);
    CHECK(strcmp(gdx_net_race_box_text(-1), "") == 0);
    /* It stays open by itself. */
    CHECK(idle_frames(900) == 0);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(gdx_net_race_box_holds_input() == 1);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
}

static void test_the_host_has_three_rows(void) {
    racing(12, 1);
    open_ready();
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(strcmp(gdx_net_race_box_text(0), "CONTINUE") == 0);
    CHECK(strcmp(gdx_net_race_box_text(1), "RETURN TO ROOM") == 0);
    CHECK(strcmp(gdx_net_race_box_text(2), "LEAVE THE ROOM") == 0);
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    /* The second row takes the host out of the race and leaves the room open. */
    CHECK(choose() == GDX_NET_RACE_BOX_LEFT);
    CHECK(sQuitCalls == 1 && sStopCalls == 0 && sFrontCalls == 0 && sHandOverCalls == 0);
    CHECK(gdx_net_race_box_rows() == 0);
}

/* LEAVE THE ROOM on the host's copy hands the room over (D28.5): the room is not stopped, since it runs the
 * race on for the others, and the race is left by the hand-over call alone. */
static void test_the_host_hands_the_room_over(void) {
    racing(13, 1);
    open_ready();
    CHECK(up() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2);
    CHECK(choose() == GDX_NET_RACE_BOX_LEFT);
    CHECK(sHandOverCalls == 1 && sStopCalls == 0 && sQuitCalls == 0 && sFrontCalls == 1);
    CHECK(gdx_net_race_box_rows() == 0 && gdx_net_race_box_holds_input() == 0);
}

static void test_the_press_that_opened_is_not_an_answer(void) {
    int i;

    racing(13, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    /* For the frames after it opened no press counts, START, A and B alike. */
    for (i = 0; i < GDX_NET_RACE_BOX_OPEN_FRAMES; i++) {
        const int r = (i % 3 == 0) ? start() : (i % 3 == 1) ? choose() : back();

        CHECK(r == GDX_NET_RACE_BOX_NOTHING);
        CHECK(gdx_net_race_box_rows() == 3);
    }
    /* The next one does. */
    CHECK(start() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);

    /* One frame short of that, the box still does not answer. */
    racing(13, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(idle_frames(GDX_NET_RACE_BOX_OPEN_FRAMES - 1) == 0);
    CHECK(back() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(back() == GDX_NET_RACE_BOX_CLOSED);
}

static void test_it_opens_like_the_stock_box(void) {
    int i;

    racing(21, 0);
    CHECK(gdx_net_race_box_opening() == 0);
    /* The stock pause box's scissor falls by 8 a frame, and so does this one's: the box, 64 pixels tall
       with its scissor margin, is shut on the frame it opens and shows on the next. */
    CHECK(GDX_NET_RACE_BOX_OPEN_FRAMES == 4 && GDX_NET_RACE_BOX_OPEN_STEP == 8);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(gdx_net_race_box_opening() == GDX_NET_RACE_BOX_OPEN_FRAMES * GDX_NET_RACE_BOX_OPEN_STEP);
    /* The timer falls by one step a frame, and no press counts while it is above 0. */
    for (i = 1; i <= GDX_NET_RACE_BOX_OPEN_FRAMES; i++) {
        CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
        CHECK(gdx_net_race_box_opening() == (GDX_NET_RACE_BOX_OPEN_FRAMES - i) * GDX_NET_RACE_BOX_OPEN_STEP);
    }
    /* Fully open: it is drawn whole, and the next press counts. */
    CHECK(gdx_net_race_box_opening() == 0);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(choose() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(gdx_net_race_box_opening() == 0);

    /* Closed in the middle of opening (the race ended), it opens from the start the next time. */
    racing(21, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(idle_frames(2) == 0);
    CHECK(gdx_net_race_box_opening() == (GDX_NET_RACE_BOX_OPEN_FRAMES - 2) * GDX_NET_RACE_BOX_OPEN_STEP);
    gdx_net_race_box_reset();
    CHECK(gdx_net_race_box_opening() == 0);
    racing(21, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(gdx_net_race_box_opening() == GDX_NET_RACE_BOX_OPEN_FRAMES * GDX_NET_RACE_BOX_OPEN_STEP);
}

static void test_continue_and_b_close_it(void) {
    racing(14, 0);
    open_ready();
    CHECK(choose() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(sQuitCalls == 0 && sStopCalls == 0 && sFrontCalls == 0);
    /* Closed, A and B do nothing; START opens it again. */
    CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(back() == GDX_NET_RACE_BOX_NOTHING);
    open_ready();
    /* B closes it from any row and chooses nothing. */
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2);
    CHECK(back() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
    /* It opens on the first row again, not where the cursor was. */
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(gdx_net_race_box_cursor() == 0);
}

static void test_return_to_room(void) {
    racing(15, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    CHECK(sQuitCalls == 0);
    CHECK(choose() == GDX_NET_RACE_BOX_LEFT);
    /* The race is left once, and the room is not: the main menu comes up on the room, as after a race. */
    CHECK(sQuitCalls == 1 && sStopCalls == 0);
    CHECK(sFrontCalls == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(idle_frames(30) == 0);
    CHECK(sQuitCalls == 1);

    /* START chooses as A does. */
    racing(15, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(start() == GDX_NET_RACE_BOX_LEFT);
    CHECK(sQuitCalls == 1 && sStopCalls == 0);
}

static void test_leave_the_room(void) {
    racing(16, 0);
    open_ready();
    CHECK(up() == GDX_NET_RACE_BOX_MOVED); /* up from the first row is the last */
    CHECK(gdx_net_race_box_cursor() == 2);
    CHECK(choose() == GDX_NET_RACE_BOX_LEFT);
    /* The room is left first, then the race: both once. The main menu is to show the front page. */
    CHECK(sStopCalls == 1 && sQuitCalls == 1 && sHandOverCalls == 0);
    CHECK(sStopWasCall == 1 && sQuitWasCall == 2);
    CHECK(sFrontCalls == 1);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
}

static void test_the_cursor_goes_round(void) {
    racing(17, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2);
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(up() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2);
    CHECK(up() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_cursor() == 1);
}

/* Holds the stick for that many frames and returns how often the cursor moved. */
static int hold_stick(int y, int frames) {
    int moves = 0;
    int i;

    for (i = 0; i < frames; i++) {
        moves += stick(y) == GDX_NET_RACE_BOX_MOVED;
    }
    return moves;
}

static void test_the_stick_moves_and_repeats(void) {
    racing(18, 0);
    open_ready();
    /* The stick must lean beyond the threshold. */
    CHECK(stick(-GDX_NET_RACE_BOX_STICK) == GDX_NET_RACE_BOX_NOTHING);
    CHECK(stick(GDX_NET_RACE_BOX_STICK) == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_cursor() == 0);
    /* Down is the negative way. One move on the first frame of the push. */
    CHECK(stick(-GDX_NET_RACE_BOX_STICK - 1) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    /* Then nothing until it has been held for REPEAT_AFTER frames. */
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_AFTER - 2) == 0);
    CHECK(gdx_net_race_box_cursor() == 1);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2);
    /* Then once every REPEAT_EVERY frames. */
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_EVERY - 1) == 0);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_EVERY - 1) == 0);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_EVERY * 5) == 5);
    /* Let go and push again: that is a first push. */
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_NOTHING);
}

static void test_the_stick_up_and_a_change_of_way(void) {
    racing(19, 0);
    open_ready();
    CHECK(stick(GDX_NET_RACE_BOX_STICK + 1) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 2); /* up from the first row */
    CHECK(hold_stick(80, 20) == 0);
    /* The other way at once, with no frame at rest: a first push that way. */
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 0);
    /* And its hold starts again: no repeat until REPEAT_AFTER frames of this way. */
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_AFTER - 2) == 0);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);
}

static void test_a_stick_already_leaning_does_not_move_at_once(void) {
    int i;

    /* The player steers with the stick down, and presses START with it there. */
    racing(20, 0);
    CHECK(hold_stick(-80, 30) == 0);
    CHECK(gdx_net_race_box_frame(1, 0, 0, 0, 0, -80) == GDX_NET_RACE_BOX_OPENED);
    for (i = 0; i < GDX_NET_RACE_BOX_OPEN_FRAMES; i++) {
        CHECK(stick(-80) == GDX_NET_RACE_BOX_NOTHING);
    }
    /* 31 frames of hold before the box and the frames while it opened count as a push already made, not
       as a hold: the first frame that takes a press is the third of the hold, and the repeat comes at 60. */
    CHECK(stick(-80) == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_REPEAT_AFTER - 4) == 0);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_cursor() == 1);

    /* A stick pushed while the box opens does not move the cursor either. */
    racing(20, 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(hold_stick(-80, GDX_NET_RACE_BOX_OPEN_FRAMES) == 0);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(stick(-80) == GDX_NET_RACE_BOX_NOTHING);
}

static void test_a_choice_wins_over_the_cursor(void) {
    /* A and the d-pad on one frame: the row under the cursor is chosen, the cursor does not move first. */
    racing(21, 0);
    open_ready();
    CHECK(gdx_net_race_box_frame(0, 1, 0, 0, 1, 0) == GDX_NET_RACE_BOX_CLOSED);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
    /* B and A on one frame: B. */
    racing(21, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    CHECK(gdx_net_race_box_frame(0, 1, 1, 0, 0, 0) == GDX_NET_RACE_BOX_CLOSED);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
}

static void test_watch_once_the_machine_is_done(void) {
    racing(22, 0);
    sOwnState = FINISHED;
    /* A finished machine does not bring the box up by itself. */
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 3) == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    open_ready();
    CHECK(strcmp(gdx_net_race_box_text(0), "WATCH") == 0);
    CHECK(strcmp(gdx_net_race_box_text(1), "RETURN TO ROOM") == 0);
    CHECK(strcmp(gdx_net_race_box_text(2), "LEAVE THE ROOM") == 0);
    CHECK(choose() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(sQuitCalls == 0);

    racing(22, 0);
    sOwnState = OUT;
    open_ready();
    CHECK(strcmp(gdx_net_race_box_text(0), "WATCH") == 0);

    /* The word follows the machine while the box is open. */
    racing(22, 0);
    open_ready();
    CHECK(strcmp(gdx_net_race_box_text(0), "CONTINUE") == 0);
    sOwnState = FINISHED;
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(strcmp(gdx_net_race_box_text(0), "WATCH") == 0);
}

static void test_it_comes_up_by_itself_when_the_machine_is_out(void) {
    racing(23, 0);
    CHECK(idle_frames(100) == 0);
    sOwnState = OUT;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(idle() == GDX_NET_RACE_BOX_OPENED);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(gdx_net_race_box_holds_input() == 1);
    CHECK(strcmp(gdx_net_race_box_text(0), "WATCH") == 0);
    /* It takes no press at once, like the box START opens. */
    CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(idle_frames(GDX_NET_RACE_BOX_OPEN_FRAMES - 1) == 0);
    CHECK(choose() == GDX_NET_RACE_BOX_CLOSED);
    /* Closed to watch, it does not come up again by itself. START still opens it. */
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 4) == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(start() == GDX_NET_RACE_BOX_OPENED);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
}

static void test_it_does_not_come_up_twice(void) {
    /* The player opened and closed the box after the machine went out, before the time was up. */
    racing(24, 0);
    sOwnState = OUT;
    CHECK(idle_frames(50) == 0);
    open_ready();
    CHECK(back() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 4) == 0);
    CHECK(gdx_net_race_box_rows() == 0);

    /* The box was open when the machine went out, and is closed long after. */
    racing(24, 0);
    open_ready();
    sOwnState = OUT;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 2) == 0);
    CHECK(gdx_net_race_box_rows() == 3);
    CHECK(back() == GDX_NET_RACE_BOX_CLOSED);
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 4) == 0);
    CHECK(gdx_net_race_box_rows() == 0);

    /* A box opened and closed while the machine drove does not count: it comes up when the machine is out. */
    racing(24, 0);
    open_ready();
    CHECK(back() == GDX_NET_RACE_BOX_CLOSED);
    sOwnState = OUT;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    CHECK(idle() == GDX_NET_RACE_BOX_OPENED);
}

static void test_only_in_a_race_that_runs(void) {
    static const int phases[] = { GDX_NET_RACE_PHASE_NONE, GDX_NET_RACE_PHASE_LOADING,
                                  GDX_NET_RACE_PHASE_COUNTDOWN };
    int i;

    for (i = 0; i < 3; i++) {
        racing(25, 0);
        sFakeRoom.phase = phases[i];
        CHECK(start() == GDX_NET_RACE_BOX_NOTHING);
        CHECK(gdx_net_race_box_rows() == 0);
        CHECK(gdx_net_race_box_holds_input() == 0);
    }
    racing(25, 0);
    sFakeRoom.inRoom = 0;
    CHECK(start() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 0);
    /* A watcher of the room has no place in the race and no box. */
    racing(25, 0);
    sFakeRoom.onGrid = 0;
    CHECK(start() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 0);
    /* A machine that is out before GO does not run the box's clock. */
    racing(25, 0);
    sFakeRoom.phase = GDX_NET_RACE_PHASE_COUNTDOWN;
    sOwnState = OUT;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES * 2) == 0);
    sFakeRoom.phase = GDX_NET_RACE_PHASE_RACING;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    CHECK(idle() == GDX_NET_RACE_BOX_OPENED);
}

static void test_it_goes_when_the_race_ends(void) {
    racing(26, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    /* The race ends by itself: the box is gone at once and holds no input. */
    sFakeRoom.phase = GDX_NET_RACE_PHASE_NONE;
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(strcmp(gdx_net_race_box_text(0), "") == 0);
    CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);
}

static void test_a_box_of_another_race_is_not_this_ones(void) {
    racing(27, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    /* No frame ran in between and the room is in its next race: the box is closed, the press chooses nothing. */
    sFakeRoom.raceIndex = 28;
    CHECK(choose() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(sQuitCalls == 0 && sStopCalls == 0);

    /* The time a machine was out in one race does not count in the next. */
    racing(27, 0);
    sOwnState = OUT;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    sFakeRoom.raceIndex = 28;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    CHECK(idle() == GDX_NET_RACE_BOX_OPENED);

    /* Nor does a box that came up in one race stop the next one's. */
    sFakeRoom.raceIndex = 29;
    CHECK(idle_frames(GDX_NET_RACE_BOX_AUTO_FRAMES - 1) == 0);
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(idle() == GDX_NET_RACE_BOX_OPENED);
}

static void test_reset_closes_it(void) {
    racing(30, 0);
    open_ready();
    CHECK(down() == GDX_NET_RACE_BOX_MOVED);
    gdx_net_race_box_reset();
    CHECK(gdx_net_race_box_rows() == 0);
    CHECK(gdx_net_race_box_cursor() == 0);
    CHECK(gdx_net_race_box_holds_input() == 0);
    CHECK(strcmp(gdx_net_race_box_text(0), "") == 0);
    CHECK(strcmp(gdx_net_race_box_text(1), "") == 0);
}

static void test_words_fit_the_small_font(void) {
    static const int own[] = { DRIVES, FINISHED };
    int state;
    int row;
    size_t k;

    for (state = 0; state < 2; state++) {
        racing(31, 0);
        sOwnState = own[state];
        open_ready();
        for (row = 0; row < GDX_NET_RACE_BOX_ROWS; row++) {
            const char* text = gdx_net_race_box_text(row);

            /* The small stock font has capitals, digits and a space; the box holds 16 of them. */
            CHECK(strlen(text) > 0 && strlen(text) <= 16);
            for (k = 0; k < strlen(text); k++) {
                const char c = text[k];

                CHECK((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ');
            }
        }
    }
    CHECK(GDX_NET_RACE_BOX_ROWS == 3);
}

/* Each row names the texture it draws, and it agrees with the row's text. */
static void test_each_row_names_its_word(void) {
    int row;

    racing(40, 0);
    CHECK(gdx_net_race_box_word(0) == GDX_NET_RACE_WORD_NONE);
    open_ready();
    CHECK(gdx_net_race_box_word(0) == GDX_NET_RACE_WORD_CONTINUE);
    CHECK(gdx_net_race_box_word(1) == GDX_NET_RACE_WORD_RETURN_TO_ROOM);
    CHECK(gdx_net_race_box_word(2) == GDX_NET_RACE_WORD_LEAVE_THE_ROOM);
    CHECK(gdx_net_race_box_word(3) == GDX_NET_RACE_WORD_NONE);
    CHECK(gdx_net_race_box_word(-1) == GDX_NET_RACE_WORD_NONE);
    sOwnState = FINISHED;
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_word(0) == GDX_NET_RACE_WORD_WATCH);
    sOwnState = OUT;
    CHECK(idle() == GDX_NET_RACE_BOX_NOTHING);
    CHECK(gdx_net_race_box_word(0) == GDX_NET_RACE_WORD_WATCH);

    racing(41, 1);
    open_ready();
    CHECK(gdx_net_race_box_word(0) == GDX_NET_RACE_WORD_CONTINUE);
    CHECK(gdx_net_race_box_word(1) == GDX_NET_RACE_WORD_RETURN_TO_ROOM);
    CHECK(gdx_net_race_box_word(2) == GDX_NET_RACE_WORD_LEAVE_THE_ROOM);
    CHECK(gdx_net_race_box_word(3) == GDX_NET_RACE_WORD_NONE);

    for (row = 0; row < gdx_net_race_box_rows(); row++) {
        const char* text = gdx_net_race_box_text(row);
        const int word = gdx_net_race_box_word(row);

        CHECK((word == GDX_NET_RACE_WORD_CONTINUE) == (strcmp(text, "CONTINUE") == 0));
        CHECK((word == GDX_NET_RACE_WORD_RETURN_TO_ROOM) == (strcmp(text, "RETURN TO ROOM") == 0));
    }
}

/* The art: the sizes, only indices the legend has, the stock blank rows above and below the letters, and
 * one buffer per word, decoded once. */
static void test_the_word_art(void) {
    static const int kWords[3] = { GDX_NET_RACE_WORD_WATCH, GDX_NET_RACE_WORD_RETURN_TO_ROOM,
                                   GDX_NET_RACE_WORD_LEAVE_THE_ROOM };
    static const int kWidths[3] = { 40, 64, 64 };
    int i;
    int w = -1;
    int h = -1;

    CHECK(gdx_net_race_words_faults() == 0);
    CHECK(gdx_net_race_words_texture(GDX_NET_RACE_WORD_CONTINUE, &w, &h) == NULL);
    CHECK(w == -1 && h == -1);
    CHECK(gdx_net_race_words_texture(GDX_NET_RACE_WORD_NONE, &w, &h) == NULL);
    CHECK(gdx_net_race_words_texture(GDX_NET_RACE_WORD_COUNT, NULL, NULL) == NULL);
    for (i = 0; i < 3; i++) {
        const unsigned char* tex = gdx_net_race_words_texture(kWords[i], &w, &h);
        int inked = 0;
        int x;
        int y;

        CHECK(tex != NULL);
        if (tex == NULL) {
            continue;
        }
        CHECK(w == kWidths[i] && h == 16);
        CHECK(((unsigned long) (size_t) tex & 7u) == 0);
        CHECK(gdx_net_race_words_texture(kWords[i], NULL, NULL) == tex);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const unsigned char c = tex[y * w + x];

                CHECK(c == 0 || c == 1 || c == 2 || c == 4 || c == 5 || c == 7 || c == 11 || c == 12 || c == 16);
                if (y < 3 || y > 12) {
                    CHECK(c == 0);
                }
                inked += c != 0;
            }
        }
        /* Every word has white letter cores and dark rims. */
        CHECK(inked > w * 3);
        CHECK(memchr(tex, 5, (size_t) (w * h)) != NULL);
        CHECK(memchr(tex, 1, (size_t) (w * h)) != NULL);
    }
}

/* --dump prints each word's palette indices, one row per line, for a comparison with the art's source. */
static void dump_words(void) {
    static const int kWords[3] = { GDX_NET_RACE_WORD_RETURN_TO_ROOM, GDX_NET_RACE_WORD_LEAVE_THE_ROOM,
                                   GDX_NET_RACE_WORD_WATCH };
    int i;

    for (i = 0; i < 3; i++) {
        int w;
        int h;
        const unsigned char* tex = gdx_net_race_words_texture(kWords[i], &w, &h);
        int x;
        int y;

        printf("word %d %dx%d\n", kWords[i], w, h);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                printf(x == 0 ? "%d" : " %d", tex[y * w + x]);
            }
            printf("\n");
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump_words();
        return 0;
    }
    test_closed_until_start();
    test_start_opens_it();
    test_the_host_has_three_rows();
    test_the_host_hands_the_room_over();
    test_the_press_that_opened_is_not_an_answer();
    test_it_opens_like_the_stock_box();
    test_continue_and_b_close_it();
    test_return_to_room();
    test_leave_the_room();
    test_the_cursor_goes_round();
    test_the_stick_moves_and_repeats();
    test_the_stick_up_and_a_change_of_way();
    test_a_stick_already_leaning_does_not_move_at_once();
    test_a_choice_wins_over_the_cursor();
    test_watch_once_the_machine_is_done();
    test_it_comes_up_by_itself_when_the_machine_is_out();
    test_it_does_not_come_up_twice();
    test_only_in_a_race_that_runs();
    test_it_goes_when_the_race_ends();
    test_a_box_of_another_race_is_not_this_ones();
    test_reset_closes_it();
    test_words_fit_the_small_font();
    test_each_row_names_its_word();
    test_the_word_art();
    if (sFailures != 0) {
        printf("gdx_net_race_box_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_race_box_tests: all %d checks passed\n", sChecks);
    return 0;
}
