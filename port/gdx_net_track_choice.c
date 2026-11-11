/* port/gdx_net_track_choice.c -- see gdx_net_track_choice.h.
 * Copyright (c) Zorkats
 */

#include "gdx_net_track_choice.h"

#include <stdio.h>
#include <string.h>

#define STOCK_TRACKS 24 /* net::lobby::kStockTracks */
#define CUP_TRACKS 6
#define STOCK_CUPS 4

/* Layout, in 320 x 240 screen pixels. The course select keeps its header at the top, the track's name on row
 * 200 and its subtitle on row 210; the two rows under them are free in a room, which shows no Edit Cup. */
#define MIDDLE 160
#define MARGIN_RIGHT 300
#define TIMER_Y 44 /* where the room screen has it */
#define INFO_Y 221
#define STATUS_Y 232
#define COUNTS_LABEL_RIGHT 150
#define COUNTS_LEFT 166 /* the middle of the first of a cup's six counts */
#define COUNTS_STEP 16

#define ITEMS_MAX 12

static int sActive;         /* the course select is the room's */
static int sKind;           /* GDX_NET_TRACK_CHOICE_VOTE or _PICK: what it was opened for */
static int sState;          /* what the last update said, or what is drawn while held */
static int sShownState;     /* the last state that was not LEAVE */
static int sHeld;           /* on its way to the machine select: it draws what it drew last */
static int sSent;           /* the choice is on its way to the host */
static int sSentTrack;
static int sDeclined;       /* the player went back to the room screen without choosing */
static int sRequested;      /* and then asked for the course select again */
static unsigned sRaceIndex; /* the race the choice is for */
static GdxNetRoomView sView; /* the room as of the last update that was not LEAVE */
static GdxNetRoomView sNext; /* the room as of the last update */
static GdxNetRoomDrawItem sItems[ITEMS_MAX];
static int sItemCount;

static int in_room(const GdxNetRoomView* view) {
    return view->mode == GDX_NET_ROOM_MODE_HOSTING || view->mode == GDX_NET_ROOM_MODE_JOINED;
}

static const GdxNetRoomPlayer* player_by_id(const GdxNetRoomView* view, int id) {
    int i;

    for (i = 0; i < view->playerCount; i++) {
        if (view->players[i].id == id) {
            return &view->players[i];
        }
    }
    return NULL;
}

/* The local player when they hold a race seat. */
static const GdxNetRoomPlayer* local_racer(const GdxNetRoomView* view) {
    const GdxNetRoomPlayer* me = player_by_id(view, view->localId);

    return (me != NULL && !me->watcher) ? me : NULL;
}

int gdx_net_track_choice_available(const GdxNetRoomView* view) {
    const GdxNetRoomPlayer* me;

    if (!in_room(view)) {
        return GDX_NET_TRACK_CHOICE_NONE;
    }
    me = local_racer(view);
    if (me == NULL) {
        return GDX_NET_TRACK_CHOICE_NONE;
    }
    if (view->phase == GDX_NET_ROOM_PHASE_VOTING && !me->voted) {
        return GDX_NET_TRACK_CHOICE_VOTE;
    }
    if (view->phase == GDX_NET_ROOM_PHASE_PICK && view->picker == view->localId) {
        return GDX_NET_TRACK_CHOICE_PICK;
    }
    return GDX_NET_TRACK_CHOICE_NONE;
}

int gdx_net_track_choice_poll_open(void) {
    int kind;

    sActive = 0;
    sHeld = 0;
    gdx_lan_room_view(&sView);
    kind = gdx_net_track_choice_available(&sView);
    if (kind == GDX_NET_TRACK_CHOICE_NONE) {
        /* Nothing to choose: whatever was sent or declined belonged to a choice that is over. */
        sSent = 0;
        sDeclined = 0;
        sRequested = 0;
        return 0;
    }
    if (sDeclined && !sRequested) {
        return 0;
    }
    sActive = 1;
    sKind = kind;
    sState = GDX_NET_TRACK_CHOICE_CHOOSE;
    sShownState = GDX_NET_TRACK_CHOICE_CHOOSE;
    sSent = 0;
    sDeclined = 0;
    sRequested = 0;
    sRaceIndex = sView.raceIndex;
    gdx_menu_request_close();
    return 1;
}

void gdx_net_track_choice_request(void) {
    sRequested = 1;
}

int gdx_net_track_choice_active(void) {
    return sActive;
}

void gdx_net_track_choice_close(void) {
    sActive = 0;
    sHeld = 0;
}

void gdx_net_track_choice_back(void) {
    if (sActive && !sSent) {
        sDeclined = 1;
        sRequested = 0;
    }
}

void gdx_net_track_choice_hold(void) {
    if (sActive) {
        sHeld = 1;
        sState = sShownState;
    }
}

static int state_of(const GdxNetRoomView* view) {
    const GdxNetRoomPlayer* me;

    if (!in_room(view) || view->raceIndex != sRaceIndex) {
        return GDX_NET_TRACK_CHOICE_LEAVE;
    }
    me = local_racer(view);
    if (me == NULL) {
        return GDX_NET_TRACK_CHOICE_LEAVE;
    }
    if (sKind == GDX_NET_TRACK_CHOICE_VOTE) {
        if (view->phase == GDX_NET_ROOM_PHASE_VOTING) {
            return (sSent || me->voted) ? GDX_NET_TRACK_CHOICE_WAIT : GDX_NET_TRACK_CHOICE_CHOOSE;
        }
        if (view->phase == GDX_NET_ROOM_PHASE_LOTTERY) {
            /* The track travels with the room's state; the ballots follow in a message of their own. */
            return (view->track >= 0 && view->track < STOCK_TRACKS) ? GDX_NET_TRACK_CHOICE_SHOW
                                                                     : GDX_NET_TRACK_CHOICE_WAIT;
        }
        return GDX_NET_TRACK_CHOICE_LEAVE;
    }
    if (view->phase == GDX_NET_ROOM_PHASE_PICK && view->picker == view->localId) {
        return sSent ? GDX_NET_TRACK_CHOICE_WAIT : GDX_NET_TRACK_CHOICE_CHOOSE;
    }
    return GDX_NET_TRACK_CHOICE_LEAVE;
}

int gdx_net_track_choice_update(void) {
    if (!sActive) {
        return GDX_NET_TRACK_CHOICE_LEAVE;
    }
    if (sHeld) {
        return GDX_NET_TRACK_CHOICE_HOLD;
    }
    gdx_lan_room_view(&sNext);
    sState = state_of(&sNext);
    if (sState != GDX_NET_TRACK_CHOICE_LEAVE) {
        /* The room as it is drawn. A room that has moved on is not drawn, and the screen that leaves for
           the machine select draws the one before it (gdx_net_track_choice_hold). */
        sView = sNext;
        sShownState = sState;
    }
    return sState;
}

int gdx_net_track_choice_shown_track(void) {
    return sView.track;
}

int gdx_net_track_choice_allowed(int track) {
    if (track == GDX_NET_ROOM_TRACK_RANDOM) {
        return 1;
    }
    return track >= 0 && track < STOCK_TRACKS && ((sView.pool >> track) & 1u) != 0;
}

int gdx_net_track_choice_confirm(int track) {
    if (!sActive || sSent || sState != GDX_NET_TRACK_CHOICE_CHOOSE || !gdx_net_track_choice_allowed(track)) {
        return 0;
    }
    if (sKind == GDX_NET_TRACK_CHOICE_VOTE) {
        gdx_lan_room_vote(track);
    } else {
        gdx_lan_room_pick(track);
    }
    sSent = 1;
    sSentTrack = track;
    /* Shown as waiting from now on, also by a screen that is held before the next update: the host's own
       choice can move the room on at once. */
    sState = GDX_NET_TRACK_CHOICE_WAIT;
    sShownState = GDX_NET_TRACK_CHOICE_WAIT;
    return 1;
}

const char* gdx_net_track_choice_random_title(void) {
    return "RANDOM TRACK";
}

/* ---- drawing ---- */

static void put_text(int x, int y, int font, int align, int colour, const char* text) {
    GdxNetRoomDrawItem* item;

    if (sItemCount >= ITEMS_MAX) {
        return;
    }
    item = &sItems[sItemCount++];
    memset(item, 0, sizeof(*item));
    item->kind = GDX_NET_ROOM_DRAW_TEXT;
    item->font = (unsigned char) font;
    item->align = (unsigned char) align;
    item->colour = (unsigned char) colour;
    item->x = (short) x;
    item->y = (short) y;
    gdx_net_text_filter(font, text, item->text, (int) sizeof(item->text));
}

static void put_status(int colour, const char* text) {
    put_text(MIDDLE, STATUS_Y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_CENTRE, colour, text);
}

static void put_info(int colour, const char* text) {
    put_text(MIDDLE, INFO_Y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_CENTRE, colour, text);
}

static void put_timer(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];

    if (sView.phaseTotalMs == 0) {
        return;
    }
    gdx_net_text_format_timer(sView.phaseLeftMs, text, (int) sizeof(text));
    /* In the title's font, the one of the two with a colon. */
    put_text(MARGIN_RIGHT, TIMER_Y, GDX_NET_ROOM_FONT_TITLE, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE, text);
}

static int count_votes(const GdxNetRoomBallot* ballots, int count, int track) {
    int votes = 0;
    int i;

    for (i = 0; i < count; i++) {
        votes += ballots[i].track == track ? 1 : 0;
    }
    return votes;
}

/* The ballot of a player in a list, or GDX_NET_ROOM_TRACK_NONE. */
static int ballot_of(const GdxNetRoomBallot* ballots, int count, int id) {
    int i;

    for (i = 0; i < count; i++) {
        if (ballots[i].id == id) {
            return ballots[i].track;
        }
    }
    return GDX_NET_ROOM_TRACK_NONE;
}

/* The votes of the place the cursor is on: one count for each of a cup's six tracks, in their order, or the
 * count of the ballots that let the room draw. The track given as marked stands out. */
static void put_counts(int cup, const GdxNetRoomBallot* ballots, int count, int marked) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int i;

    if (cup == GDX_NET_TRACK_CHOICE_RANDOM_CUP) {
        const int votes = count_votes(ballots, count, GDX_NET_ROOM_TRACK_RANDOM);

        snprintf(text, sizeof(text), "%d %s FOR A RANDOM TRACK", votes, votes == 1 ? "VOTE" : "VOTES");
        put_info(marked == GDX_NET_ROOM_TRACK_RANDOM ? GDX_NET_ROOM_COLOUR_GOLD : GDX_NET_ROOM_COLOUR_WHITE, text);
        return;
    }
    if (cup < 0 || cup >= STOCK_CUPS) {
        return;
    }
    put_text(COUNTS_LABEL_RIGHT, INFO_Y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_DIM,
             "VOTES IN THIS CUP");
    for (i = 0; i < CUP_TRACKS; i++) {
        const int track = cup * CUP_TRACKS + i;
        const int votes = count_votes(ballots, count, track);

        snprintf(text, sizeof(text), "%d", votes);
        put_text(COUNTS_LEFT + i * COUNTS_STEP, INFO_Y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_CENTRE,
                 track == marked ? GDX_NET_ROOM_COLOUR_GOLD
                                 : votes == 0 ? GDX_NET_ROOM_COLOUR_DIM : GDX_NET_ROOM_COLOUR_WHITE,
                 text);
    }
}

static void draw_choose(int cup, int trackNo) {
    put_timer();
    put_status(GDX_NET_ROOM_COLOUR_WHITE,
               sKind == GDX_NET_TRACK_CHOICE_VOTE ? "VOTE FOR THE NEXT TRACK" : "YOU PICK THE NEXT TRACK");
    if (cup == GDX_NET_TRACK_CHOICE_RANDOM_CUP) {
        put_info(GDX_NET_ROOM_COLOUR_DIM, "THIS PLACE IS A RANDOM TRACK");
    } else if (cup >= 0 && cup < STOCK_CUPS && trackNo >= 0 && trackNo < CUP_TRACKS &&
               !gdx_net_track_choice_allowed(cup * CUP_TRACKS + trackNo)) {
        put_info(GDX_NET_ROOM_COLOUR_RED, "THE ROOM DOES NOT RACE THIS TRACK");
    }
}

static void draw_wait(int cup) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int racers = 0;
    int own;
    int i;

    if (sKind != GDX_NET_TRACK_CHOICE_VOTE || sView.phase != GDX_NET_ROOM_PHASE_VOTING) {
        if (sView.phase != GDX_NET_ROOM_PHASE_LOTTERY) {
            put_timer();
        }
        put_status(GDX_NET_ROOM_COLOUR_WHITE, "PLEASE WAIT");
        return;
    }
    put_timer();
    for (i = 0; i < sView.playerCount; i++) {
        racers += sView.players[i].watcher ? 0 : 1;
    }
    snprintf(text, sizeof(text), "%d OF %d VOTED", sView.ballotCount, racers);
    put_status(GDX_NET_ROOM_COLOUR_WHITE, text);
    /* The player's own ballot, as the room has it or, until the room answers, as it was sent. */
    own = ballot_of(sView.ballots, sView.ballotCount, sView.localId);
    if (own == GDX_NET_ROOM_TRACK_NONE && sSent) {
        own = sSentTrack;
    }
    put_counts(cup, sView.ballots, sView.ballotCount, own);
}

static void draw_show(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    const GdxNetRoomBallot* winner;
    const GdxNetRoomPlayer* player;

    /* The lottery's message has not arrived, or is the one of an earlier race: the track alone is shown. */
    if (sView.lotteryTrack != sView.track) {
        return;
    }
    if (sView.lotteryWinner < 0 || sView.lotteryWinner >= sView.lotteryCount) {
        put_status(GDX_NET_ROOM_COLOUR_WHITE, "NOBODY VOTED. THE TRACK WAS DRAWN.");
        return;
    }
    winner = &sView.lottery[sView.lotteryWinner];
    if (winner->id == sView.localId) {
        put_status(GDX_NET_ROOM_COLOUR_GOLD, "YOUR BALLOT WON");
    } else {
        player = player_by_id(&sView, winner->id);
        snprintf(text, sizeof(text), "THE BALLOT OF %s WON", player != NULL ? player->name : "A PLAYER WHO LEFT");
        put_status(GDX_NET_ROOM_COLOUR_WHITE, text);
    }
    if (winner->track == GDX_NET_ROOM_TRACK_RANDOM) {
        put_info(GDX_NET_ROOM_COLOUR_DIM, "A RANDOM TRACK WAS DRAWN");
    } else {
        put_counts(sView.track / CUP_TRACKS, sView.lottery, sView.lotteryCount, sView.track);
    }
}

const GdxNetRoomDrawItem* gdx_net_track_choice_draw(int cup, int trackNo, int* count) {
    sItemCount = 0;
    if (sActive) {
        switch (sState) {
            case GDX_NET_TRACK_CHOICE_CHOOSE:
                draw_choose(cup, trackNo);
                break;
            case GDX_NET_TRACK_CHOICE_WAIT:
                draw_wait(cup);
                break;
            case GDX_NET_TRACK_CHOICE_SHOW:
                draw_show();
                break;
            default:
                break;
        }
    }
    *count = sItemCount;
    return sItems;
}
