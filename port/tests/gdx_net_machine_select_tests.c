/* port/tests/gdx_net_machine_select_tests.c -- unit tests for the G-NET lock-in screen (gdx_net_machine_select.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_machine_select.c only. The room and the menu are the four functions below, so no
 * socket, thread or game code is involved. Expected values are written out by hand from the header.
 *
 * The module keeps the race it last sent a lock for, so every test uses race numbers of its own.
 */

#include "../gdx_net_machine_select.h"

#include <math.h>
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

/* The room and the menu, as the module sees them. */
static GdxNetLockInRoom sFakeRoom;
static int sRoomReads;
static int sSends;
static int sSentMachine;
static int sSentSkin;
static int sSentEngine;
static int sCloseRequests;
static int sRuleCaptures;
/* The race bridge's answer when the screen asks for the room's race: 0 there is none for this copy, 1 its
 * grid is on the way, 2 it is set up. */
static int sArmAnswer;
static int sArmCalls;
static unsigned sArmRace;

void gdx_lan_lock_in_room(GdxNetLockInRoom* out) {
    sRoomReads++;
    *out = sFakeRoom;
}

void gdx_lan_lock_in_send(int machine, int skin, int enginePercent) {
    sSends++;
    sSentMachine = machine;
    sSentSkin = skin;
    sSentEngine = enginePercent;
}

void gdx_menu_request_close(void) {
    sCloseRequests++;
}

void gdx_lan_race_capture_rules(void) {
    sRuleCaptures++;
}

int gdx_net_race_arm(unsigned raceIndex) {
    sArmCalls++;
    sArmRace = raceIndex;
    return sArmAnswer;
}

static void reset_counts(void) {
    sRoomReads = 0;
    sSends = 0;
    sSentMachine = -1;
    sSentSkin = -1;
    sSentEngine = -1;
    sCloseRequests = 0;
    sRuleCaptures = 0;
    sArmAnswer = 0;
    sArmCalls = 0;
    sArmRace = 0u;
}

/* A room in its lock-in state with the local player on the grid: four racers, nobody locked, a minute left,
 * course 3. */
static void room_lock_in(unsigned raceIndex) {
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    sFakeRoom.inRoom = 1;
    sFakeRoom.phaseMachine = 1;
    sFakeRoom.raceIndex = raceIndex;
    sFakeRoom.entrant = 1;
    sFakeRoom.leftMs = 60000u;
    sFakeRoom.racers = 4;
    sFakeRoom.track = 3;
}

/* Opens the screen for a race, as the main menu does, with fresh counters afterwards. */
static void open_screen(unsigned raceIndex) {
    room_lock_in(raceIndex);
    CHECK(gdx_net_machine_select_poll_open() == 1);
    reset_counts();
}

static void test_does_not_open(void) {
    reset_counts();

    /* No room at all. */
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    CHECK(gdx_net_machine_select_poll_open() == 0);
    CHECK(gdx_net_machine_select_active() == 0);

    /* In a room that is not in its lock-in state. */
    room_lock_in(100u);
    sFakeRoom.phaseMachine = 0;
    CHECK(gdx_net_machine_select_poll_open() == 0);

    /* Watching this race. */
    room_lock_in(100u);
    sFakeRoom.entrant = 0;
    CHECK(gdx_net_machine_select_poll_open() == 0);

    /* The room already has this player's lock, for instance from the menu page. */
    room_lock_in(100u);
    sFakeRoom.locked = 1;
    CHECK(gdx_net_machine_select_poll_open() == 0);

    /* A course the stock game does not have. */
    room_lock_in(100u);
    sFakeRoom.track = -1;
    CHECK(gdx_net_machine_select_poll_open() == 0);
    sFakeRoom.track = 24;
    CHECK(gdx_net_machine_select_poll_open() == 0);

    CHECK(gdx_net_machine_select_active() == 0);
    CHECK(gdx_net_machine_select_waiting() == 0);
    CHECK(sCloseRequests == 0);
    /* One look at the room per call. */
    CHECK(sRoomReads == 6);
}

static void test_opens(void) {
    reset_counts();
    room_lock_in(110u);
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_active() == 1);
    CHECK(gdx_net_machine_select_waiting() == 0);
    CHECK(gdx_net_machine_select_track() == 3);
    /* The menu is put away so the screen can be seen. */
    CHECK(sCloseRequests == 1);
    CHECK(sRoomReads == 1);

    /* The first and the last stock course. */
    room_lock_in(111u);
    sFakeRoom.track = 0;
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_track() == 0);
    room_lock_in(112u);
    sFakeRoom.track = 23;
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_track() == 23);
}

/* A room's custom track: it races in the first Edit Cup slot, under its own title as the title font draws it. */
static void test_custom_track(void) {
    reset_counts();
    room_lock_in(115u);
    sFakeRoom.track = GDX_NET_LOCK_IN_TRACK_CUSTOM;
    /* No title: the room's view is not one a custom race can start from. */
    CHECK(gdx_net_machine_select_poll_open() == 0);
    CHECK(gdx_net_machine_select_active() == 0);

    snprintf(sFakeRoom.title, sizeof(sFakeRoom.title), "%s", "Loop de loop");
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_track() == GDX_NET_LOCK_IN_COURSE_CUSTOM);
    CHECK(gdx_net_machine_select_track() == 24);
    CHECK(gdx_net_machine_select_track_title() != NULL);
    CHECK(strcmp(gdx_net_machine_select_track_title(), "LOOP DE LOOP") == 0);

    /* A stock track keeps its own name. */
    room_lock_in(116u);
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_track() == 3);
    CHECK(gdx_net_machine_select_track_title() == NULL);
}

/* The main menu only runs when the screen is not up, so its poll records that. */
static void test_main_menu_poll_clears(void) {
    open_screen(120u);
    CHECK(gdx_net_machine_select_active() == 1);
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    CHECK(gdx_net_machine_select_poll_open() == 0);
    CHECK(gdx_net_machine_select_active() == 0);
    /* Nothing to leave any more. */
    CHECK(gdx_net_machine_select_should_leave() == 0);
}

static void test_stays_then_leaves_when_the_state_ends(void) {
    int tick;

    open_screen(130u);
    for (tick = 0; tick < 5; tick++) {
        sFakeRoom.leftMs -= 1000u;
        CHECK(gdx_net_machine_select_should_leave() == 0);
    }
    CHECK(gdx_net_machine_select_active() == 1);
    CHECK(sRoomReads == 5);

    /* Everyone locked in or the timer ran out: the room is on its next state. */
    sFakeRoom.phaseMachine = 0;
    CHECK(gdx_net_machine_select_should_leave() == 1);
    CHECK(gdx_net_machine_select_active() == 0);
    CHECK(gdx_net_machine_select_waiting() == 0);
    /* Once. */
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(sCloseRequests == 0);
}

static void test_leaves_when_the_room_closes(void) {
    open_screen(140u);
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    CHECK(gdx_net_machine_select_should_leave() == 1);
    CHECK(gdx_net_machine_select_active() == 0);
}

static void test_leaves_when_taken_off_the_grid(void) {
    open_screen(150u);
    sFakeRoom.entrant = 0;
    CHECK(gdx_net_machine_select_should_leave() == 1);
}

/* A lock-in for another race is not this screen's, even though the state reads the same. */
static void test_leaves_on_another_race(void) {
    open_screen(160u);
    sFakeRoom.raceIndex = 161u;
    CHECK(gdx_net_machine_select_should_leave() == 1);
    CHECK(gdx_net_machine_select_active() == 0);
    /* Back on the main menu, the new race opens its own screen. */
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_active() == 1);
    CHECK(sCloseRequests == 1);
}

static void test_confirm_sends_once_and_waits(void) {
    open_screen(170u);
    gdx_net_machine_select_confirm(7, 2, 0.75f);
    CHECK(sSends == 1);
    CHECK(sSentMachine == 7);
    CHECK(sSentSkin == 2);
    CHECK(sSentEngine == 75);
    /* Waiting from this moment, before the room has answered. */
    CHECK(gdx_net_machine_select_waiting() == 1);
    CHECK(gdx_net_machine_select_active() == 1);

    /* A lock is final. */
    gdx_net_machine_select_confirm(9, 1, 0.25f);
    CHECK(sSends == 1);
    CHECK(sSentMachine == 7);

    /* The screen stays up while the others choose, also once the room shows the lock. */
    CHECK(gdx_net_machine_select_should_leave() == 0);
    sFakeRoom.locked = 1;
    sFakeRoom.lockedCount = 1;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(gdx_net_machine_select_waiting() == 1);

    sFakeRoom.phaseMachine = 0;
    CHECK(gdx_net_machine_select_should_leave() == 1);
    CHECK(gdx_net_machine_select_waiting() == 0);
    CHECK(sSends == 1);
}

/* The lock is on its way and the room has not shown it yet: the main menu must not open the screen again. */
static void test_sent_lock_does_not_reopen(void) {
    open_screen(180u);
    gdx_net_machine_select_confirm(1, 0, 0.5f);
    CHECK(sSends == 1);
    CHECK(sFakeRoom.locked == 0);
    CHECK(gdx_net_machine_select_poll_open() == 0);
    CHECK(gdx_net_machine_select_active() == 0);
    CHECK(sCloseRequests == 0);

    /* The next race of the room is a new lock-in. */
    room_lock_in(181u);
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(gdx_net_machine_select_waiting() == 0);
    gdx_net_machine_select_confirm(2, 3, 1.0f);
    CHECK(sSends == 2);
    CHECK(sSentMachine == 2);
    CHECK(sSentSkin == 3);
    CHECK(sSentEngine == 100);
}

/* Locked from the menu page while the screen is up: the screen waits and sends nothing of its own. */
static void test_room_lock_makes_the_screen_wait(void) {
    open_screen(190u);
    CHECK(gdx_net_machine_select_waiting() == 0);
    sFakeRoom.locked = 1;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(gdx_net_machine_select_waiting() == 1);
    gdx_net_machine_select_confirm(4, 1, 0.5f);
    CHECK(sSends == 0);
}

static void test_confirm_needs_the_screen(void) {
    reset_counts();
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    CHECK(gdx_net_machine_select_poll_open() == 0);
    gdx_net_machine_select_confirm(4, 1, 0.5f);
    CHECK(sSends == 0);
    CHECK(gdx_net_machine_select_waiting() == 0);
}

/* What reaches the room is always a machine and a colour it has. */
static void test_confirm_clamps(void) {
    open_screen(200u);
    gdx_net_machine_select_confirm(29, 3, 0.0f);
    CHECK(sSends == 1 && sSentMachine == 29 && sSentSkin == 3 && sSentEngine == 0);

    open_screen(201u);
    gdx_net_machine_select_confirm(30, 4, 0.5f);
    CHECK(sSends == 1 && sSentMachine == 0 && sSentSkin == 0 && sSentEngine == 50);

    open_screen(202u);
    gdx_net_machine_select_confirm(-1, -1, 2.0f);
    CHECK(sSends == 1 && sSentMachine == 0 && sSentSkin == 0 && sSentEngine == 100);
}

static void test_engine_percent(void) {
    CHECK(gdx_net_lock_in_engine_percent(0.0f) == 0);
    CHECK(gdx_net_lock_in_engine_percent(0.5f) == 50);
    CHECK(gdx_net_lock_in_engine_percent(1.0f) == 100);
    /* Rounded to the nearest percent. */
    CHECK(gdx_net_lock_in_engine_percent(0.004f) == 0);
    CHECK(gdx_net_lock_in_engine_percent(0.006f) == 1);
    CHECK(gdx_net_lock_in_engine_percent(0.994f) == 99);
    CHECK(gdx_net_lock_in_engine_percent(0.996f) == 100);
    /* Outside the slider. */
    CHECK(gdx_net_lock_in_engine_percent(-0.2f) == 0);
    CHECK(gdx_net_lock_in_engine_percent(1.5f) == 100);
    CHECK(gdx_net_lock_in_engine_percent((float) INFINITY) == 100);
    CHECK(gdx_net_lock_in_engine_percent((float) -INFINITY) == 0);
    /* Not a number: the middle of the slider. */
    CHECK(gdx_net_lock_in_engine_percent((float) NAN) == 50);
}

static int timer_is(unsigned leftMs, const char* expected) {
    char text[8];

    memset(text, '#', sizeof(text));
    gdx_net_lock_in_format_timer(leftMs, text, (int) sizeof(text));
    return strcmp(text, expected) == 0;
}

static void test_timer_text(void) {
    char small[3];

    CHECK(timer_is(0u, "0:00"));
    /* Rounded up: 0:00 only when no time is left. */
    CHECK(timer_is(1u, "0:01"));
    CHECK(timer_is(1000u, "0:01"));
    CHECK(timer_is(1001u, "0:02"));
    CHECK(timer_is(9000u, "0:09"));
    CHECK(timer_is(59000u, "0:59"));
    CHECK(timer_is(59001u, "1:00"));
    CHECK(timer_is(60000u, "1:00"));
    CHECK(timer_is(240000u, "4:00"));
    CHECK(timer_is(599000u, "9:59"));
    /* Four glyphs at most. */
    CHECK(timer_is(599001u, "9:59"));
    CHECK(timer_is(3600000u, "9:59"));
    CHECK(timer_is(4294967295u, "9:59"));

    /* A short buffer is cut, never overrun. */
    memset(small, '#', sizeof(small));
    gdx_net_lock_in_format_timer(60000u, small, (int) sizeof(small));
    CHECK(strcmp(small, "1:") == 0);
}

static int count_is(int locked, int racers, const char* expected) {
    char text[12];

    memset(text, '#', sizeof(text));
    gdx_net_lock_in_format_count(locked, racers, text, (int) sizeof(text));
    return strcmp(text, expected) == 0;
}

static void test_count_text(void) {
    CHECK(count_is(0, 4, "0 OF 4"));
    CHECK(count_is(3, 8, "3 OF 8"));
    CHECK(count_is(12, 12, "12 OF 12"));
    /* Never more locked than racing, never negative. */
    CHECK(count_is(9, 4, "4 OF 4"));
    CHECK(count_is(-1, 4, "0 OF 4"));
    CHECK(count_is(2, -3, "0 OF 0"));
    /* Two digits at most. */
    CHECK(count_is(150, 150, "99 OF 99"));
}

/* The menu fonts draw capitals, digits, the space and the colon among others, and have no slash. */
static int only_font_glyphs(const char* text) {
    for (; *text != '\0'; text++) {
        if (!((*text >= 'A' && *text <= 'Z') || (*text >= '0' && *text <= '9') || *text == ' ' || *text == ':')) {
            return 0;
        }
    }
    return 1;
}

static void test_screen_text_follows_the_room(void) {
    room_lock_in(210u);
    sFakeRoom.leftMs = 41500u;
    sFakeRoom.lockedCount = 3;
    sFakeRoom.racers = 8;
    CHECK(gdx_net_machine_select_poll_open() == 1);
    reset_counts();

    CHECK(strcmp(gdx_net_machine_select_timer_text(), "0:42") == 0);
    /* While the player chooses, the timer is all the screen adds: how many have locked in is not shown. */
    CHECK(gdx_net_machine_select_waiting() == 0);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:42") == 0);
    CHECK(strcmp(gdx_net_machine_select_wait_text(), "PLEASE WAIT") == 0);
    /* Drawing reads nothing: the room is looked at once a tick, by the update. */
    CHECK(sRoomReads == 0);

    sFakeRoom.leftMs = 9000u;
    sFakeRoom.lockedCount = 7;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:09") == 0);

    /* Locked in: the count joins the timer, also before the room has answered the lock. */
    gdx_net_machine_select_confirm(7, 2, 0.75f);
    CHECK(gdx_net_machine_select_waiting() == 1);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:09  7 OF 8") == 0);
    sFakeRoom.locked = 1;
    sFakeRoom.lockedCount = 8;
    sFakeRoom.leftMs = 8001u;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(strcmp(gdx_net_machine_select_timer_text(), "0:09") == 0);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:09  8 OF 8") == 0);

    /* The longest line the room can produce. */
    sFakeRoom.leftMs = 600000u;
    sFakeRoom.lockedCount = 12;
    sFakeRoom.racers = 12;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "9:59  12 OF 12") == 0);

    CHECK(only_font_glyphs(gdx_net_machine_select_timer_text()));
    CHECK(only_font_glyphs(gdx_net_machine_select_status_text()));
    CHECK(only_font_glyphs(gdx_net_machine_select_wait_text()));
}

/* A racer whose lock the room holds reads the count as well, whether or not this copy remembers sending it:
 * the room's word counts, not only this copy's own press. */
static void test_status_text_of_a_lock_the_room_holds(void) {
    room_lock_in(211u);
    sFakeRoom.leftMs = 30000u;
    sFakeRoom.lockedCount = 2;
    sFakeRoom.racers = 5;
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:30") == 0);
    sFakeRoom.locked = 1;
    sFakeRoom.lockedCount = 3;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(gdx_net_machine_select_waiting() == 1);
    CHECK(strcmp(gdx_net_machine_select_status_text(), "0:30  3 OF 5") == 0);
}

/* The host's driving options become the rules of the race when its lock-in screen opens, and only then. */
static void test_rules_are_read_when_the_screen_opens(void) {
    reset_counts();
    memset(&sFakeRoom, 0, sizeof(sFakeRoom));
    CHECK(gdx_net_machine_select_poll_open() == 0);
    CHECK(sRuleCaptures == 0);
    room_lock_in(300u);
    CHECK(gdx_net_machine_select_poll_open() == 1);
    CHECK(sRuleCaptures == 1);
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(sRuleCaptures == 1);
    /* While the lock-in is on, the race is not asked for. */
    CHECK(sArmCalls == 0);
}

/* The lock-in is over and the room's race is next: the screen goes on to the race, not to the main menu. */
static void test_goes_on_to_the_race(void) {
    open_screen(310u);
    sFakeRoom.phaseMachine = 0;
    sArmAnswer = 2;
    CHECK(gdx_net_machine_select_should_leave() == 2);
    CHECK(sArmCalls == 1);
    CHECK(sArmRace == 310u);
    CHECK(gdx_net_machine_select_active() == 0);
    /* Once. */
    CHECK(gdx_net_machine_select_should_leave() == 0);
    CHECK(sArmCalls == 1);
}

/* The room is loading the race and its grid has not arrived yet: the screen stays up for it. */
static void test_waits_for_the_grid(void) {
    int tick;

    open_screen(320u);
    sFakeRoom.phaseMachine = 0;
    sArmAnswer = 1;
    for (tick = 0; tick < 3; tick++) {
        CHECK(gdx_net_machine_select_should_leave() == 0);
        CHECK(gdx_net_machine_select_active() == 1);
    }
    CHECK(sArmCalls == 3);
    sArmAnswer = 2;
    CHECK(gdx_net_machine_select_should_leave() == 2);

    /* Or the race goes on without this copy: back to the main menu and its room screen. */
    open_screen(321u);
    sFakeRoom.phaseMachine = 0;
    sArmAnswer = 1;
    CHECK(gdx_net_machine_select_should_leave() == 0);
    sArmAnswer = 0;
    CHECK(gdx_net_machine_select_should_leave() == 1);
    CHECK(gdx_net_machine_select_active() == 0);
}

int main(void) {
    test_does_not_open();
    test_opens();
    test_custom_track();
    test_main_menu_poll_clears();
    test_stays_then_leaves_when_the_state_ends();
    test_leaves_when_the_room_closes();
    test_leaves_when_taken_off_the_grid();
    test_leaves_on_another_race();
    test_confirm_sends_once_and_waits();
    test_sent_lock_does_not_reopen();
    test_room_lock_makes_the_screen_wait();
    test_confirm_needs_the_screen();
    test_confirm_clamps();
    test_engine_percent();
    test_timer_text();
    test_count_text();
    test_screen_text_follows_the_room();
    test_status_text_of_a_lock_the_room_holds();
    test_rules_are_read_when_the_screen_opens();
    test_goes_on_to_the_race();
    test_waits_for_the_grid();
    if (sFailures != 0) {
        printf("gdx_net_machine_select_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_machine_select_tests: all %d checks passed\n", sChecks);
    return 0;
}
