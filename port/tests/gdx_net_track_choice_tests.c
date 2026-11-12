/* port/tests/gdx_net_track_choice_tests.c -- unit tests for the G-NET vote and pick on the course select
 * (gdx_net_track_choice.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_track_choice.c and gdx_net_text.c. The room session is the functions of
 * gdx_net_room_view.h the module calls, the Edit Cup is the two of gdx_net_own_tracks.h and the port menu is one
 * more, all written here, so no socket, thread, save or game code is involved. Expected texts, colours and positions are written out by hand.
 *
 * The module keeps its state between calls, as it does between screens of the game, so every test starts by
 * closing the screen and polling an empty session.
 */

#include "../gdx_net_track_choice.h"
#include "../gdx_net_own_tracks.h"

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

/* ---- the session and the menu, as the module sees them ---- */

static GdxNetRoomView sFake;
static int sVoteCalls;
static int sVoteTrack;
static int sPickCalls;
static int sPickTrack;
static int sMenuCloseCalls;

void gdx_lan_room_view(GdxNetRoomView* out) {
    *out = sFake;
}

void gdx_lan_room_vote(int track) {
    sVoteCalls++;
    sVoteTrack = track;
}

void gdx_lan_room_pick(int track) {
    sPickCalls++;
    sPickTrack = track;
}

void gdx_menu_request_close(void) {
    sMenuCloseCalls++;
}

/* The Edit Cup: which slots hold a track a room can race, and their identities and titles. */
static int sOwnOk[GDX_NET_OWN_TRACK_SLOTS];
static unsigned char sOwnId[GDX_NET_OWN_TRACK_SLOTS][GDX_NET_ROOM_TRACK_ID_BYTES];
static char sOwnTitle[GDX_NET_OWN_TRACK_SLOTS][GDX_NET_ROOM_TITLE_BYTES];
static int sForgetCalls;
static int sVoteCustomCalls;
static int sPickCustomCalls;
static unsigned char sSentId[GDX_NET_ROOM_TRACK_ID_BYTES];
static char sSentTitle[GDX_NET_ROOM_TITLE_BYTES];

int gdx_net_own_track(int slot, unsigned char* id, char* title, int titleSize) {
    if (slot < 0 || slot >= GDX_NET_OWN_TRACK_SLOTS || !sOwnOk[slot]) {
        return 0;
    }
    if (id != NULL) {
        memcpy(id, sOwnId[slot], GDX_NET_ROOM_TRACK_ID_BYTES);
    }
    if (title != NULL && titleSize > 0) {
        snprintf(title, (size_t) titleSize, "%s", sOwnTitle[slot]);
    }
    return 1;
}

void gdx_net_own_tracks_forget(void) {
    sForgetCalls++;
}

void gdx_lan_room_vote_custom(const unsigned char* id, const char* title) {
    sVoteCustomCalls++;
    memcpy(sSentId, id, sizeof(sSentId));
    snprintf(sSentTitle, sizeof(sSentTitle), "%s", title);
}

void gdx_lan_room_pick_custom(const unsigned char* id, const char* title) {
    sPickCustomCalls++;
    memcpy(sSentId, id, sizeof(sSentId));
    snprintf(sSentTitle, sizeof(sSentTitle), "%s", title);
}

/* Slot holds a track whose identity is 32 bytes of fill, titled title. */
static void own_track(int slot, unsigned char fill, const char* title) {
    sOwnOk[slot] = 1;
    memset(sOwnId[slot], fill, GDX_NET_ROOM_TRACK_ID_BYTES);
    snprintf(sOwnTitle[slot], sizeof(sOwnTitle[slot]), "%s", title);
}

/* A room of count players with ids 0 to count - 1 named "P00" on; id 0 hosts and the last watchers of them
 * hold watch seats. Every stock track is in the pool and the room is setting up its race 3. */
static void fake_room(int mode, int phase, int localId, int count, int watchers) {
    int i;

    memset(&sFake, 0, sizeof(sFake));
    sFake.mode = mode;
    sFake.phase = phase;
    sFake.localId = localId;
    sFake.raceIndex = 3u;
    sFake.racerCap = 8;
    sFake.pool = 0xFFFFFFu;
    sFake.track = GDX_NET_ROOM_TRACK_NONE;
    sFake.lastTrack = GDX_NET_ROOM_TRACK_NONE;
    sFake.picker = GDX_NET_ROOM_NOBODY;
    sFake.lotteryWinner = -1;
    sFake.lotteryTrack = GDX_NET_ROOM_TRACK_NONE;
    sFake.playerCount = count;
    for (i = 0; i < count; i++) {
        GdxNetRoomPlayer* p = &sFake.players[i];

        p->id = (unsigned char) i;
        p->host = i == 0;
        p->watcher = i >= count - watchers;
        p->machine = 0xFF;
        snprintf(p->name, sizeof(p->name), "P%02d", i);
    }
}

/* No session, the screen not the room's, nothing sent or declined, and every counter at zero. */
static void reset(void) {
    memset(&sFake, 0, sizeof(sFake));
    sFake.localId = GDX_NET_ROOM_NOBODY;
    gdx_net_track_choice_close();
    gdx_net_track_choice_poll_open();
    sVoteCalls = 0;
    sVoteTrack = -1;
    sPickCalls = 0;
    sPickTrack = -1;
    sMenuCloseCalls = 0;
    memset(sOwnOk, 0, sizeof(sOwnOk));
    sForgetCalls = 0;
    sVoteCustomCalls = 0;
    sPickCustomCalls = 0;
    memset(sSentId, 0, sizeof(sSentId));
    sSentTitle[0] = '\0';
}

/* A vote of four racers and a watcher, seen by racer 2, with the course select open for it. */
static void open_vote(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    gdx_net_track_choice_poll_open();
}

/* A pick that is racer 2's, among three racers, with the course select open for it. */
static void open_pick(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 3, 0);
    sFake.picker = 2;
    gdx_net_track_choice_poll_open();
}

static void set_ballot(int index, int id, int track) {
    sFake.ballots[index].id = (unsigned char) id;
    sFake.ballots[index].track = (unsigned char) track;
}

static void set_lottery(int index, int id, int track) {
    sFake.lottery[index].id = (unsigned char) id;
    sFake.lottery[index].track = (unsigned char) track;
}

/* A custom ballot of player id for the track whose identity is 32 bytes of fill. */
static void set_custom(GdxNetRoomBallot* ballot, int id, unsigned char fill, const char* title) {
    memset(ballot, 0, sizeof(*ballot));
    ballot->id = (unsigned char) id;
    ballot->track = GDX_NET_ROOM_TRACK_CUSTOM;
    memset(ballot->custom, fill, sizeof(ballot->custom));
    snprintf(ballot->title, sizeof(ballot->title), "%s", title);
}

/* ---- the lines ---- */

static int sCup;
static int sTrackNo;

/* The place of the cursor the draw calls below are for: a cup, 0 to 4, and a track of it or -1. */
static void at(int cup, int trackNo) {
    sCup = cup;
    sTrackNo = trackNo;
}

static int draw_count(void) {
    int count = -1;

    gdx_net_track_choice_draw(sCup, sTrackNo, &count);
    return count;
}

static const GdxNetRoomDrawItem* find_text(const char* text) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_track_choice_draw(sCup, sTrackNo, &count);
    int i;

    for (i = 0; i < count; i++) {
        if (items[i].kind == GDX_NET_ROOM_DRAW_TEXT && strcmp(items[i].text, text) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

static int has_text(const char* text) {
    return find_text(text) != NULL;
}

/* The text at a place, or NULL. Two texts never share one. */
static const GdxNetRoomDrawItem* text_at(int x, int y) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_track_choice_draw(sCup, sTrackNo, &count);
    int i;

    for (i = 0; i < count; i++) {
        if (items[i].kind == GDX_NET_ROOM_DRAW_TEXT && items[i].x == x && items[i].y == y) {
            return &items[i];
        }
    }
    return NULL;
}

/* 1 when the text at a place is this one, in this colour, in the small font. */
static int small_text_is(int x, int y, const char* text, int colour, int align) {
    const GdxNetRoomDrawItem* item = text_at(x, y);

    return item != NULL && strcmp(item->text, text) == 0 && item->colour == colour && item->align == align &&
           item->font == GDX_NET_ROOM_FONT_SMALL;
}

/* 1 when the six counts of a cup read as given, "0" dim, the marked one gold and the others white.
 * marked is 0 to 5, or -1. */
static int counts_are(const char* const counts[6], int marked) {
    int ok = 1;
    int i;

    for (i = 0; i < 6; i++) {
        int colour = GDX_NET_ROOM_COLOUR_WHITE;

        if (i == marked) {
            colour = GDX_NET_ROOM_COLOUR_GOLD;
        } else if (strcmp(counts[i], "0") == 0) {
            colour = GDX_NET_ROOM_COLOUR_DIM;
        }
        ok = ok && small_text_is(166 + 16 * i, 221, counts[i], colour, GDX_NET_ROOM_ALIGN_CENTRE);
    }
    return ok;
}

/* ---- what the local player may choose ---- */

static void test_available(void) {
    int phase;
    int mode;

    reset();
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);

    /* A racer in a vote, until their ballot is in. */
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_VOTE);
    sFake.players[1].voted = 1;
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_VOTE);
    sFake.players[2].voted = 1;
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);

    /* The host votes as any racer does. */
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_VOTING, 0, 5, 1);
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_VOTE);

    /* A watcher has no ballot, and neither has a player the roster does not list yet. */
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 4, 5, 1);
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 9, 5, 1);
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);

    /* The pick is one racer's. */
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 3, 0);
    sFake.picker = 2;
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_PICK);
    sFake.picker = 1;
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);
    sFake.picker = GDX_NET_ROOM_NOBODY;
    CHECK(gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_NONE);

    /* No other phase asks for a track. */
    for (phase = GDX_NET_ROOM_PHASE_LOBBY; phase <= GDX_NET_ROOM_PHASE_RESULTS; phase++) {
        fake_room(GDX_NET_ROOM_MODE_JOINED, phase, 2, 5, 1);
        sFake.picker = 1;
        CHECK((gdx_net_track_choice_available(&sFake) != GDX_NET_TRACK_CHOICE_NONE) ==
              (phase == GDX_NET_ROOM_PHASE_VOTING));
    }

    /* Only a room the player is in. */
    for (mode = GDX_NET_ROOM_MODE_IDLE; mode <= GDX_NET_ROOM_MODE_CLOSED; mode++) {
        fake_room(mode, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
        CHECK((gdx_net_track_choice_available(&sFake) == GDX_NET_TRACK_CHOICE_VOTE) ==
              (mode == GDX_NET_ROOM_MODE_HOSTING || mode == GDX_NET_ROOM_MODE_JOINED));
    }
}

/* ---- when the course select is the room's ---- */

static void test_poll(void) {
    /* Nothing to choose: the screen does not open and the menu is left alone. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 5, 1);
    CHECK(gdx_net_track_choice_poll_open() == 0);
    CHECK(gdx_net_track_choice_active() == 0);
    CHECK(sMenuCloseCalls == 0);

    /* A vote opens it, and the port menu steps aside. */
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_active() == 1);
    CHECK(sMenuCloseCalls == 1);
    /* The poll answers the same on the next tick when the screen could not change on this one. */
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_active() == 1);

    /* B on the cups: the main menu runs again and the player is not taken back. */
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_active() == 1); /* until the main menu polls */
    CHECK(gdx_net_track_choice_poll_open() == 0);
    CHECK(gdx_net_track_choice_active() == 0);
    CHECK(gdx_net_track_choice_poll_open() == 0);
    /* The room screen's row asks for it again. */
    gdx_net_track_choice_request();
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_active() == 1);
    /* Open again, the choice is no longer a declined one: the poll of the next tick answers the same. */
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_active() == 1);
    /* Once, not for good: going back again keeps the player on the room screen. */
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 0);

    /* Going back is about one choice. The next vote opens the screen by itself. */
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_poll_open() == 0);
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    sFake.raceIndex = 4u;
    CHECK(gdx_net_track_choice_poll_open() == 1);

    /* A request made while nothing was open is forgotten with the choice. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    gdx_net_track_choice_poll_open();
    gdx_net_track_choice_back();
    gdx_net_track_choice_request();
    sFake.phase = GDX_NET_ROOM_PHASE_LOBBY;
    CHECK(gdx_net_track_choice_poll_open() == 0);
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 0);

    /* Closing is not going back. */
    open_vote();
    gdx_net_track_choice_close();
    CHECK(gdx_net_track_choice_active() == 0);
    CHECK(gdx_net_track_choice_poll_open() == 1);
    /* Neither is a B that comes while the screen is not the room's, */
    gdx_net_track_choice_close();
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 1);
    /* or after the choice was sent. */
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_confirm(3) == 1);
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 1);

    /* The pick opens it for the racer who picks. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_PICK, 0, 3, 0);
    sFake.picker = 1;
    CHECK(gdx_net_track_choice_poll_open() == 0);
    sFake.picker = 0;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(sMenuCloseCalls == 1);
}

/* ---- what the room makes of the screen ---- */

static void test_update_vote(void) {
    /* Not the room's: nothing to do here. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);

    open_vote();
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    /* The room has the ballot. */
    sFake.players[2].voted = 1;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    sFake.players[2].voted = 0;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    /* The ballot is sent: no input from then on, before the room answers. */
    CHECK(gdx_net_track_choice_confirm(3) == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);

    /* The lottery: the screen waits for the track, then shows it. */
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    sFake.track = GDX_NET_ROOM_TRACK_RANDOM; /* never a track to turn to */
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    sFake.track = 24;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    sFake.track = 23;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    CHECK(gdx_net_track_choice_shown_track() == 23);
    sFake.track = 0;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    CHECK(gdx_net_track_choice_shown_track() == 0);
    /* A racer whose ballot never came sees the lottery too. */
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 7;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    CHECK(gdx_net_track_choice_shown_track() == 7);

    /* Then the room locks machines in, and the screen is over. */
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);

    /* A pick is not what this screen was opened for. */
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_PICK;
    sFake.picker = 2;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* Another race's vote is not either. */
    open_vote();
    sFake.raceIndex = 4u;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* The room closed, */
    open_vote();
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* the player was moved to a watch seat, */
    open_vote();
    sFake.players[2].watcher = 1;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* or is not in the roster any more. */
    open_vote();
    sFake.localId = 9;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* The room went back to its lobby. */
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_LOBBY;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
}

static void test_update_pick(void) {
    open_pick();
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_confirm(5) == 1);
    CHECK(sPickCalls == 1 && sPickTrack == 5 && sVoteCalls == 0);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    /* The room took the pick: on to the machines. */
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    sFake.track = 5;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);

    /* A lottery belongs to a vote, so it is not this screen's. */
    open_pick();
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 5;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* The pick went to someone else. */
    open_pick();
    sFake.picker = 1;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* A fourth racer joined and the room votes. */
    open_pick();
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    open_pick();
    sFake.raceIndex = 2u;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
}

/* The screen leaves for the room's machine select: it stays the room's, asks for nothing more and draws what
   it drew last, until the main menu polls or the screen is closed. */
static void test_hold(void) {
    /* Not the room's: nothing to hold. */
    reset();
    gdx_net_track_choice_hold();
    CHECK(!gdx_net_track_choice_active());
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);

    /* A vote, its lottery, then the machines. */
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 7;
    sFake.lotteryTrack = 7;
    sFake.lotteryCount = 0;
    sFake.lotteryWinner = -1;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    at(1, 1);
    CHECK(draw_count() == 1 && has_text("NOBODY VOTED. THE TRACK WAS DRAWN."));
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    sFake.track = 9;
    sFake.lotteryTrack = 9;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    /* A room that moved on is not drawn, and the track shown is still the lottery's. */
    CHECK(draw_count() == 0);
    CHECK(gdx_net_track_choice_shown_track() == 7);
    /* Held: the lottery's line is back, whatever the room does next. */
    gdx_net_track_choice_hold();
    CHECK(gdx_net_track_choice_active());
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_HOLD);
    CHECK(draw_count() == 1 && has_text("NOBODY VOTED. THE TRACK WAS DRAWN."));
    sFake.phase = GDX_NET_ROOM_PHASE_GRID_SYNC;
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_HOLD);
    CHECK(draw_count() == 1 && has_text("NOBODY VOTED. THE TRACK WAS DRAWN."));
    /* It takes no choice and no B. */
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    CHECK(sVoteCalls == 0 && sPickCalls == 0);
    /* Closed: over. */
    gdx_net_track_choice_close();
    CHECK(!gdx_net_track_choice_active());
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    CHECK(draw_count() == 0);

    /* A pick that was sent: the wait line stays while the screen leaves. */
    open_pick();
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_confirm(5) == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    at(0, 5);
    CHECK(has_text("PLEASE WAIT"));
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    gdx_net_track_choice_hold();
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_HOLD);
    CHECK(has_text("PLEASE WAIT"));

    /* The host's own pick: the room has moved on by the next update, and the wait line is still what the
       screen leaves with. */
    gdx_net_track_choice_close();
    sFake.phase = GDX_NET_ROOM_PHASE_PICK;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_confirm(5) == 1);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    gdx_net_track_choice_hold();
    at(0, 5);
    CHECK(has_text("PLEASE WAIT") && !has_text("YOU PICK THE NEXT TRACK"));

    /* The main menu's poll ends a hold: the next screen of the room starts as a new one. */
    sFake.phase = GDX_NET_ROOM_PHASE_PICK;
    sFake.raceIndex = 1u;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    /* And with nothing to choose the poll leaves the screen not the room's. */
    gdx_net_track_choice_hold();
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_poll_open() == 0);
    CHECK(!gdx_net_track_choice_active());
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
}

/* ---- the choice ---- */

static void test_allowed_and_confirm(void) {
    int track;

    /* The whole pool. */
    open_vote();
    for (track = 0; track < 24; track++) {
        CHECK(gdx_net_track_choice_allowed(track) == 1);
    }
    CHECK(gdx_net_track_choice_allowed(GDX_NET_ROOM_TRACK_RANDOM) == 1);
    CHECK(gdx_net_track_choice_allowed(24) == 0); /* the first Edit Cup track */
    CHECK(gdx_net_track_choice_allowed(48) == 0); /* the X Cup's own course */
    CHECK(gdx_net_track_choice_allowed(-1) == 0);
    CHECK(gdx_net_track_choice_allowed(GDX_NET_ROOM_TRACK_NONE) == 0);

    /* A pool without track 7, as the room has it at the last update. */
    sFake.pool = 0xFFFFFFu & ~(1u << 7);
    CHECK(gdx_net_track_choice_allowed(7) == 1);
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_allowed(7) == 0);
    CHECK(gdx_net_track_choice_allowed(6) == 1 && gdx_net_track_choice_allowed(8) == 1);
    CHECK(gdx_net_track_choice_allowed(GDX_NET_ROOM_TRACK_RANDOM) == 1); /* a room always takes Random */
    sFake.pool = 1u << 23;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_allowed(23) == 1 && gdx_net_track_choice_allowed(22) == 0);
    CHECK(gdx_net_track_choice_allowed(0) == 0);
    /* The pool's bits past the last stock track name no track. */
    sFake.pool = 0xFFFFFFFFu;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_allowed(23) == 1);
    CHECK(gdx_net_track_choice_allowed(24) == 0 && gdx_net_track_choice_allowed(31) == 0);
    sFake.pool = 1u << 23;
    gdx_net_track_choice_update();

    /* A track outside the pool is not sent. */
    CHECK(gdx_net_track_choice_confirm(7) == 0);
    CHECK(gdx_net_track_choice_confirm(24) == 0);
    CHECK(sVoteCalls == 0 && sPickCalls == 0);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    /* One in it is, once. */
    CHECK(gdx_net_track_choice_confirm(23) == 1);
    CHECK(sVoteCalls == 1 && sVoteTrack == 23 && sPickCalls == 0);
    CHECK(gdx_net_track_choice_confirm(23) == 0);
    CHECK(gdx_net_track_choice_confirm(GDX_NET_ROOM_TRACK_RANDOM) == 0);
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(23) == 0);
    CHECK(sVoteCalls == 1);

    /* The ballot that lets the room draw. */
    open_vote();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(GDX_NET_ROOM_TRACK_RANDOM) == 1);
    CHECK(sVoteCalls == 1 && sVoteTrack == GDX_NET_ROOM_TRACK_RANDOM);

    /* Nothing is sent from a screen that is not the room's, */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    open_vote();
    gdx_net_track_choice_close();
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    /* when the room already has the player's ballot, */
    open_vote();
    sFake.players[2].voted = 1;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    /* or when the vote is over. */
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 7;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    open_vote();
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 0);
    CHECK(sVoteCalls == 0 && sPickCalls == 0);

    /* A new vote can be answered after an old one was. */
    open_vote();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 1);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_poll_open() == 0);
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    sFake.raceIndex = 4u;
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_confirm(4) == 1);
    CHECK(sVoteCalls == 2 && sVoteTrack == 4);

    /* The pick goes out as a pick. */
    open_pick();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(GDX_NET_ROOM_TRACK_RANDOM) == 1);
    CHECK(sPickCalls == 1 && sPickTrack == GDX_NET_ROOM_TRACK_RANDOM && sVoteCalls == 0);
    CHECK(gdx_net_track_choice_confirm(2) == 0);
    CHECK(sPickCalls == 1);

    CHECK(strcmp(gdx_net_track_choice_random_title(), "RANDOM TRACK") == 0);
}

/* ---- the lines while the player chooses ---- */

static void test_draw_choose(void) {
    const GdxNetRoomDrawItem* item;

    /* Not the room's: no line. */
    reset();
    at(0, -1);
    CHECK(draw_count() == 0);

    open_vote();
    sFake.phaseTotalMs = 30000u;
    sFake.phaseLeftMs = 12001u;
    gdx_net_track_choice_update();
    at(0, -1);
    CHECK(draw_count() == 2);
    /* The timer, top right, in the font with a colon. */
    item = find_text("0:13");
    CHECK(item != NULL && item->x == 300 && item->y == 44);
    CHECK(item != NULL && item->align == GDX_NET_ROOM_ALIGN_RIGHT && item->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_WHITE && item->kind == GDX_NET_ROOM_DRAW_TEXT);
    /* What is asked, on the last row. */
    CHECK(small_text_is(160, 232, "VOTE FOR THE NEXT TRACK", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    /* No votes show before the player's own ballot is in. */
    sFake.ballotCount = 1;
    set_ballot(0, 1, 3);
    gdx_net_track_choice_update();
    CHECK(draw_count() == 2);
    CHECK(!has_text("VOTES IN THIS CUP") && !has_text("1 OF 4 VOTED"));
    at(0, 3);
    CHECK(draw_count() == 2);

    /* The X Cup's place. */
    at(4, -1);
    CHECK(draw_count() == 3);
    CHECK(small_text_is(160, 221, "THIS PLACE IS A RANDOM TRACK", GDX_NET_ROOM_COLOUR_DIM, GDX_NET_ROOM_ALIGN_CENTRE));
    at(4, 0);
    CHECK(has_text("THIS PLACE IS A RANDOM TRACK"));

    /* A track the room does not race, when the cursor is on it. */
    sFake.pool = 0xFFFFFFu & ~(1u << 7);
    gdx_net_track_choice_update();
    at(1, 1);
    CHECK(draw_count() == 3);
    CHECK(small_text_is(160, 221, "THE ROOM DOES NOT RACE THIS TRACK", GDX_NET_ROOM_COLOUR_RED,
                        GDX_NET_ROOM_ALIGN_CENTRE));
    at(1, 0);
    CHECK(draw_count() == 2);
    at(1, 2);
    CHECK(draw_count() == 2);
    at(1, -1); /* the cup screen */
    CHECK(draw_count() == 2);
    at(0, 1);
    CHECK(draw_count() == 2);

    /* A phase without a timer shows none. */
    sFake.phaseTotalMs = 0u;
    gdx_net_track_choice_update();
    at(0, -1);
    CHECK(draw_count() == 1 && !has_text("0:13"));

    /* The last second, and the minute. */
    sFake.phaseTotalMs = 90000u;
    sFake.phaseLeftMs = 1u;
    gdx_net_track_choice_update();
    CHECK(has_text("0:01"));
    sFake.phaseLeftMs = 60001u;
    gdx_net_track_choice_update();
    CHECK(has_text("1:01"));

    /* The pick. */
    open_pick();
    sFake.phaseTotalMs = 15000u;
    sFake.phaseLeftMs = 15000u;
    gdx_net_track_choice_update();
    at(2, 5);
    CHECK(draw_count() == 2);
    CHECK(small_text_is(160, 232, "YOU PICK THE NEXT TRACK", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(find_text("0:15") != NULL && find_text("0:15")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(!has_text("VOTE FOR THE NEXT TRACK"));

    /* Closed: no line. */
    gdx_net_track_choice_close();
    CHECK(draw_count() == 0);
}

/* ---- the lines once the choice is sent ---- */

static void test_draw_wait(void) {
    static const char* const kNone[6] = { "0", "0", "0", "0", "0", "0" };
    static const char* const kOwnAlone[6] = { "0", "0", "0", "1", "0", "0" };
    static const char* const kOwnAndOther[6] = { "0", "0", "0", "2", "1", "0" };
    static const char* const kOthers[6] = { "0", "0", "0", "1", "1", "0" };
    static const char* const kMoved[6] = { "0", "0", "0", "1", "1", "1" };

    open_vote();
    sFake.phaseTotalMs = 30000u;
    sFake.phaseLeftMs = 9000u;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 1);

    /* Sent, and the room has not answered: the ballot counts nowhere yet, but its track is marked. */
    at(0, 3);
    CHECK(draw_count() == 9);
    CHECK(find_text("0:09") != NULL && find_text("0:09")->x == 300 && find_text("0:09")->y == 44);
    CHECK(small_text_is(160, 232, "0 OF 4 VOTED", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(small_text_is(150, 221, "VOTES IN THIS CUP", GDX_NET_ROOM_COLOUR_DIM, GDX_NET_ROOM_ALIGN_RIGHT));
    {
        static const char* const kSent[6] = { "0", "0", "0", "0", "0", "0" };

        CHECK(small_text_is(166 + 16 * 3, 221, "0", GDX_NET_ROOM_COLOUR_GOLD, GDX_NET_ROOM_ALIGN_CENTRE));
        CHECK(counts_are(kSent, 3));
    }
    CHECK(!has_text("VOTE FOR THE NEXT TRACK") && !has_text("PLEASE WAIT"));

    /* The room has it. */
    sFake.players[2].voted = 1;
    sFake.ballotCount = 1;
    set_ballot(0, 2, 3);
    gdx_net_track_choice_update();
    CHECK(has_text("1 OF 4 VOTED"));
    CHECK(counts_are(kOwnAlone, 3));

    /* Other ballots: the same track, the next one, and one that lets the room draw. */
    sFake.ballotCount = 4;
    set_ballot(1, 1, 3);
    set_ballot(2, 0, 4);
    set_ballot(3, 3, GDX_NET_ROOM_TRACK_RANDOM);
    gdx_net_track_choice_update();
    CHECK(draw_count() == 9);
    CHECK(has_text("4 OF 4 VOTED"));
    CHECK(counts_are(kOwnAndOther, 3));
    CHECK(small_text_is(166 + 16 * 4, 221, "1", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));

    /* The counts are of the cup on screen, whichever track of it. */
    at(0, 0);
    CHECK(counts_are(kOwnAndOther, 3));
    at(0, -1);
    CHECK(counts_are(kOwnAndOther, 3));
    at(1, 3);
    CHECK(draw_count() == 9);
    CHECK(counts_are(kNone, -1));
    at(3, 0);
    CHECK(counts_are(kNone, -1));
    /* A place that is no cup of the room has no counts: the timer and the voters alone. */
    at(-1, -1);
    CHECK(draw_count() == 2 && has_text("4 OF 4 VOTED") && !has_text("VOTES IN THIS CUP"));
    at(5, 0);
    CHECK(draw_count() == 2 && !has_text("VOTES IN THIS CUP"));

    /* The X Cup's place counts the ballots that let the room draw. */
    at(4, -1);
    CHECK(draw_count() == 3);
    CHECK(small_text_is(160, 221, "1 VOTE FOR A RANDOM TRACK", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(!has_text("VOTES IN THIS CUP"));
    set_ballot(2, 0, GDX_NET_ROOM_TRACK_RANDOM);
    gdx_net_track_choice_update();
    CHECK(small_text_is(160, 221, "2 VOTES FOR A RANDOM TRACK", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    sFake.ballotCount = 2;
    gdx_net_track_choice_update();
    CHECK(has_text("0 VOTES FOR A RANDOM TRACK"));

    /* The room's word on the player's ballot is the one marked, not what was sent. */
    sFake.ballotCount = 3;
    set_ballot(0, 2, 5);
    set_ballot(1, 1, 3);
    set_ballot(2, 0, 4);
    gdx_net_track_choice_update();
    at(0, 3);
    CHECK(counts_are(kMoved, 5));

    /* The player's own ballot when it lets the room draw. */
    open_vote();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(GDX_NET_ROOM_TRACK_RANDOM) == 1);
    at(4, -1);
    CHECK(small_text_is(160, 221, "0 VOTES FOR A RANDOM TRACK", GDX_NET_ROOM_COLOUR_GOLD, GDX_NET_ROOM_ALIGN_CENTRE));
    sFake.ballotCount = 3;
    set_ballot(0, 2, GDX_NET_ROOM_TRACK_RANDOM);
    set_ballot(1, 1, 3);
    set_ballot(2, 0, 4);
    gdx_net_track_choice_update();
    CHECK(small_text_is(160, 221, "1 VOTE FOR A RANDOM TRACK", GDX_NET_ROOM_COLOUR_GOLD, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(draw_count() == 2); /* no timer: this vote has none */
    at(0, 0);
    CHECK(counts_are(kOthers, -1)); /* and no track of a cup is the player's */
    CHECK(has_text("3 OF 4 VOTED"));

    /* A room of 12 racers and 2 watchers. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 14, 2);
    gdx_net_track_choice_poll_open();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(0) == 1);
    {
        static const char* const kFull[6] = { "12", "0", "0", "0", "0", "0" };
        int i;

        sFake.ballotCount = 12;
        for (i = 0; i < 12; i++) {
            set_ballot(i, i, 0);
        }
        gdx_net_track_choice_update();
        at(0, 0);
        CHECK(has_text("12 OF 12 VOTED"));
        CHECK(counts_are(kFull, 0));
    }

    /* The vote closed and the track has not come: nothing to count, no timer. */
    open_vote();
    sFake.phaseTotalMs = 3000u;
    sFake.phaseLeftMs = 3000u;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(3) == 1);
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    gdx_net_track_choice_update();
    at(0, 3);
    CHECK(draw_count() == 1);
    CHECK(small_text_is(160, 232, "PLEASE WAIT", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));

    /* The pick, sent. */
    open_pick();
    sFake.phaseTotalMs = 15000u;
    sFake.phaseLeftMs = 4000u;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(8) == 1);
    at(1, 2);
    CHECK(draw_count() == 2);
    CHECK(small_text_is(160, 232, "PLEASE WAIT", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(has_text("0:04"));
    CHECK(!has_text("VOTES IN THIS CUP") && !has_text("YOU PICK THE NEXT TRACK"));
}

/* ---- the lines of the lottery ---- */

static void test_draw_show(void) {
    static const char* const kCupOfSeven[6] = { "0", "2", "0", "0", "1", "0" };
    static const char* const kOneVote[6] = { "0", "1", "0", "0", "0", "0" };

    open_vote();
    sFake.phaseTotalMs = 3000u;
    sFake.phaseLeftMs = 2500u;
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 7;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);

    /* The track came before the lottery's ballots, or those are an earlier race's: the track alone shows. */
    at(1, 1);
    CHECK(draw_count() == 0);
    sFake.lotteryTrack = 3;
    sFake.lotteryCount = 1;
    set_lottery(0, 1, 3);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    CHECK(draw_count() == 0);

    /* Nobody voted. No timer: the lottery is a short show, not a wait. */
    sFake.lotteryTrack = 7;
    sFake.lotteryCount = 0;
    sFake.lotteryWinner = -1;
    gdx_net_track_choice_update();
    CHECK(draw_count() == 1);
    CHECK(small_text_is(160, 232, "NOBODY VOTED. THE TRACK WAS DRAWN.", GDX_NET_ROOM_COLOUR_WHITE,
                        GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(!has_text("0:03"));
    /* A winner the list does not hold reads the same. */
    sFake.lotteryCount = 2;
    set_lottery(0, 1, 7);
    set_lottery(1, 0, 10);
    sFake.lotteryWinner = 2;
    gdx_net_track_choice_update();
    CHECK(draw_count() == 1 && has_text("NOBODY VOTED. THE TRACK WAS DRAWN."));

    /* Another racer's ballot won: their name, and the final votes of the track's cup with the track marked. */
    sFake.lotteryCount = 4;
    set_lottery(0, 1, 7);
    set_lottery(1, 0, 10);
    set_lottery(2, 2, 3);
    set_lottery(3, 3, 7);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    CHECK(draw_count() == 8);
    CHECK(small_text_is(160, 232, "THE BALLOT OF P01 WON", GDX_NET_ROOM_COLOUR_WHITE, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(small_text_is(150, 221, "VOTES IN THIS CUP", GDX_NET_ROOM_COLOUR_DIM, GDX_NET_ROOM_ALIGN_RIGHT));
    CHECK(counts_are(kCupOfSeven, 1));
    /* Whatever place the cursor was on: the screen has turned to the track. */
    at(0, 3);
    CHECK(counts_are(kCupOfSeven, 1));
    at(4, -1);
    CHECK(counts_are(kCupOfSeven, 1));
    /* The ballots of the vote that closed are not the lottery's. */
    sFake.ballotCount = 1;
    set_ballot(0, 1, 8);
    gdx_net_track_choice_update();
    CHECK(counts_are(kCupOfSeven, 1));

    /* A name in small letters and with a character the font lacks. */
    snprintf(sFake.players[1].name, sizeof(sFake.players[1].name), "Po_1");
    gdx_net_track_choice_update();
    CHECK(has_text("THE BALLOT OF PO 1 WON"));
    /* The longest name fits its line. */
    snprintf(sFake.players[1].name, sizeof(sFake.players[1].name), "ABCDEFGHIJKLMNOP");
    gdx_net_track_choice_update();
    CHECK(has_text("THE BALLOT OF ABCDEFGHIJKLMNOP WON"));
    /* The winner left the room. */
    set_lottery(0, 9, 7);
    gdx_net_track_choice_update();
    CHECK(has_text("THE BALLOT OF A PLAYER WHO LEFT WON"));

    /* The player's own ballot won. */
    sFake.lotteryCount = 1;
    set_lottery(0, 2, 7);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    CHECK(small_text_is(160, 232, "YOUR BALLOT WON", GDX_NET_ROOM_COLOUR_GOLD, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(counts_are(kOneVote, 1));

    /* The winning ballot let the room draw: no count would explain the track. */
    sFake.lotteryCount = 2;
    set_lottery(0, 1, GDX_NET_ROOM_TRACK_RANDOM);
    set_lottery(1, 2, 3);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    CHECK(draw_count() == 2);
    CHECK(has_text("THE BALLOT OF A PLAYER WHO LEFT WON") == 0);
    CHECK(small_text_is(160, 221, "A RANDOM TRACK WAS DRAWN", GDX_NET_ROOM_COLOUR_DIM, GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(!has_text("VOTES IN THIS CUP"));
    set_lottery(0, 2, GDX_NET_ROOM_TRACK_RANDOM);
    set_lottery(1, 1, 3);
    gdx_net_track_choice_update();
    CHECK(has_text("YOUR BALLOT WON") && has_text("A RANDOM TRACK WAS DRAWN"));

    /* The last track of the last cup. */
    sFake.track = 23;
    sFake.lotteryTrack = 23;
    sFake.lotteryCount = 1;
    set_lottery(0, 0, 23);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    {
        static const char* const kLast[6] = { "0", "0", "0", "0", "0", "1" };

        CHECK(has_text("THE BALLOT OF P00 WON"));
        CHECK(counts_are(kLast, 5));
    }

    /* Over: the room locks machines in, and the screen has no line left. */
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_LEAVE);
    CHECK(draw_count() == 0);
}

/* ---- custom tracks: the Edit Cup in a room that takes them ---- */

static void test_custom_tracks(void) {
    static const char* const kEditCup[6] = { "2", "0", "0", "0", "0", "0" };
    static const char* const kStockCup[6] = { "0", "1", "0", "0", "0", "0" };
    static const char* const kWonCup[6] = { "1", "0", "0", "0", "0", "0" };
    unsigned char want[GDX_NET_ROOM_TRACK_ID_BYTES];

    /* A room without custom tracks takes no Edit Cup track, even one that could be sent. */
    open_vote();
    own_track(0, 0xA1, "My Loop");
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_custom_tracks() == 0);
    CHECK(gdx_net_track_choice_allowed(24) == 0);
    CHECK(gdx_net_track_choice_confirm(24) == 0);
    CHECK(sVoteCustomCalls == 0 && sVoteCalls == 0);
    /* Every choice reads the Edit Cup afresh. */
    CHECK(sForgetCalls == 1);

    /* A room with them: each slot that holds a track a room can race, and no other. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    sFake.customTracks = 1;
    own_track(0, 0xA1, "My Loop");
    own_track(5, 0xA6, "Last Slot");
    CHECK(gdx_net_track_choice_custom_tracks() == 0); /* the screen is not the room's yet */
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_CHOOSE);
    CHECK(gdx_net_track_choice_custom_tracks() == 1);
    CHECK(gdx_net_track_choice_allowed(24) == 1 && gdx_net_track_choice_allowed(29) == 1);
    CHECK(gdx_net_track_choice_allowed(25) == 0 && gdx_net_track_choice_allowed(28) == 0);
    CHECK(gdx_net_track_choice_allowed(23) == 1 && gdx_net_track_choice_allowed(30) == 0);
    CHECK(gdx_net_track_choice_allowed(GDX_NET_ROOM_TRACK_CUSTOM) == 0);

    /* The lines on the Edit Cup: a slot with nothing to send says so, one with a track says nothing. */
    at(GDX_NET_TRACK_CHOICE_EDIT_CUP, 1);
    CHECK(small_text_is(160, 221, "THIS SLOT HAS NO TRACK TO SEND", GDX_NET_ROOM_COLOUR_RED,
                        GDX_NET_ROOM_ALIGN_CENTRE));
    at(GDX_NET_TRACK_CHOICE_EDIT_CUP, 0);
    CHECK(text_at(160, 221) == NULL);
    at(GDX_NET_TRACK_CHOICE_EDIT_CUP, -1);
    CHECK(text_at(160, 221) == NULL);

    /* An empty slot is not sent; a track is, by its identity and title, once. */
    CHECK(gdx_net_track_choice_confirm(25) == 0);
    CHECK(gdx_net_track_choice_confirm(24) == 1);
    memset(want, 0xA1, sizeof(want));
    CHECK(sVoteCustomCalls == 1 && sVoteCalls == 0 && sPickCustomCalls == 0);
    CHECK(memcmp(sSentId, want, sizeof(want)) == 0 && strcmp(sSentTitle, "My Loop") == 0);
    CHECK(gdx_net_track_choice_confirm(29) == 0);
    CHECK(sVoteCustomCalls == 1);

    /* Until the room has the ballot, the one sent is marked on the Edit Cup. */
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_WAIT);
    {
        static const char* const kNone[6] = { "0", "0", "0", "0", "0", "0" };

        CHECK(counts_are(kNone, 0));
    }

    /* The votes so far: two for this copy's first slot, one for another copy's track and one stock. The
       Edit Cup counts this copy's own tracks; another copy's track counts in no cup. */
    sFake.ballotCount = 4;
    set_custom(&sFake.ballots[0], 1, 0xA1, "My Loop");
    set_custom(&sFake.ballots[1], 2, 0xA1, "My Loop");
    set_custom(&sFake.ballots[2], 3, 0xB0, "Their Track");
    set_ballot(3, 0, 1);
    sFake.players[2].voted = 1;
    gdx_net_track_choice_update();
    CHECK(small_text_is(150, 221, "VOTES IN THIS CUP", GDX_NET_ROOM_COLOUR_DIM, GDX_NET_ROOM_ALIGN_RIGHT));
    CHECK(counts_are(kEditCup, 0));
    at(0, -1);
    CHECK(counts_are(kStockCup, -1));

    /* The lottery drew this copy's track: the screen turns to its Edit Cup slot and marks it. */
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
    memset(sFake.trackId, 0xA1, sizeof(sFake.trackId));
    snprintf(sFake.trackTitle, sizeof(sFake.trackTitle), "My Loop");
    sFake.lotteryTrack = GDX_NET_ROOM_TRACK_CUSTOM;
    sFake.lotteryCount = 2;
    set_custom(&sFake.lottery[0], 2, 0xA1, "My Loop");
    set_custom(&sFake.lottery[1], 3, 0xB0, "Their Track");
    sFake.lotteryWinner = 0;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    CHECK(gdx_net_track_choice_shown_track() == 24);
    CHECK(gdx_net_track_choice_foreign_title() == NULL);
    CHECK(has_text("YOUR BALLOT WON"));
    CHECK(counts_are(kWonCup, 0));

    /* Another copy's track won: this copy lacks it, so its title stands for it. */
    memset(sFake.trackId, 0xB0, sizeof(sFake.trackId));
    snprintf(sFake.trackTitle, sizeof(sFake.trackTitle), "Their Track");
    sFake.lotteryWinner = 1;
    CHECK(gdx_net_track_choice_update() == GDX_NET_TRACK_CHOICE_SHOW);
    CHECK(gdx_net_track_choice_shown_track() == -1);
    CHECK(gdx_net_track_choice_foreign_title() != NULL &&
          strcmp(gdx_net_track_choice_foreign_title(), "THEIR TRACK") == 0);
    CHECK(has_text("THE BALLOT OF P03 WON"));
    CHECK(small_text_is(160, 221, "A CUSTOM TRACK. THE ROOM SENDS IT.", GDX_NET_ROOM_COLOUR_DIM,
                        GDX_NET_ROOM_ALIGN_CENTRE));
    CHECK(!has_text("VOTES IN THIS CUP"));
    /* A stock track has no foreign title. */
    sFake.track = 7;
    sFake.lotteryTrack = 7;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_foreign_title() == NULL);
    /* Nor has a screen that is not the room's. */
    sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
    sFake.lotteryTrack = GDX_NET_ROOM_TRACK_CUSTOM;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_foreign_title() != NULL);
    gdx_net_track_choice_close();
    CHECK(gdx_net_track_choice_foreign_title() == NULL);

    /* The host turns custom tracks off while the player chooses: the Edit Cup is no longer taken. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    sFake.customTracks = 1;
    own_track(0, 0xA1, "My Loop");
    gdx_net_track_choice_poll_open();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_allowed(24) == 1);
    sFake.customTracks = 0;
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_allowed(24) == 0);
    CHECK(gdx_net_track_choice_confirm(24) == 0);
    CHECK(sVoteCustomCalls == 0);

    /* The pick goes out as a pick. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 3, 0);
    sFake.picker = 2;
    sFake.customTracks = 1;
    own_track(3, 0xC3, "Pick Me");
    gdx_net_track_choice_poll_open();
    gdx_net_track_choice_update();
    CHECK(gdx_net_track_choice_confirm(27) == 1);
    memset(want, 0xC3, sizeof(want));
    CHECK(sPickCustomCalls == 1 && sVoteCustomCalls == 0 && sPickCalls == 0);
    CHECK(memcmp(sSentId, want, sizeof(want)) == 0 && strcmp(sSentTitle, "Pick Me") == 0);
}

/* Every text fits the 48 bytes of a draw item and uses only what its font has. */
static void test_texts_fit(void) {
    int count;
    const GdxNetRoomDrawItem* items;
    int i;

    open_vote();
    snprintf(sFake.players[1].name, sizeof(sFake.players[1].name), "ABCDEFGHIJKLMNOP");
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.track = 7;
    sFake.lotteryTrack = 7;
    sFake.lotteryCount = 1;
    set_lottery(0, 1, 7);
    sFake.lotteryWinner = 0;
    gdx_net_track_choice_update();
    items = gdx_net_track_choice_draw(1, 1, &count);
    CHECK(count == 8);
    for (i = 0; i < count; i++) {
        size_t length = strlen(items[i].text);
        size_t c;

        CHECK(length > 0 && length < GDX_NET_ROOM_TEXT_BYTES);
        for (c = 0; c < length; c++) {
            const char ch = items[i].text[c];

            CHECK((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.');
        }
    }
}

int main(void) {
    test_available();
    test_poll();
    test_update_vote();
    test_update_pick();
    test_hold();
    test_allowed_and_confirm();
    test_draw_choose();
    test_draw_wait();
    test_draw_show();
    test_custom_tracks();
    test_texts_fit();

    if (sFailures != 0) {
        printf("gdx_net_track_choice_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_track_choice_tests: %d checks passed\n", sChecks);
    return 0;
}
