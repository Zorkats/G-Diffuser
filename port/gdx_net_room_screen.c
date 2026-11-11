/* port/gdx_net_room_screen.c -- see gdx_net_room_screen.h.
 * Copyright (c) Zorkats
 *
 * The page follows the session: no session shows the front page or the address page, a search shows the
 * rooms, a room shows the room. Within a page the cursor is kept by the row it is on and not by its number,
 * so it stays on "LEAVE THE ROOM" when the rows above it come and go with the room's state.
 */

#include "gdx_net_room_screen.h"

#include "gdx_net_track_choice.h"

#include <stdio.h>
#include <string.h>

/* Layout, in 320 x 240 screen pixels. Text sits between the two margins. */
#define MARGIN_LEFT 20
#define MARGIN_RIGHT 300
#define MIDDLE 160
#define TITLE_Y 44
#define MENU_STEP 12
#define LIST_STEP 10
#define LIST_BOTTOM 206 /* the last row a list of the room page may stand on: the log's lines are below */
#define ROOM_ROWS_TOP 108
#define TABLE_ROWS_TOP 84 /* the room page's rows while it shows the table of a race */
#define TABLE_NAME_X (MARGIN_LEFT + 14)
#define TABLE_TIME_X 152
#define TABLE_POINTS_X 262
#define FOOT_Y 222
#define FOOT_LINES 2
#define LOG_LINES_MAX 6
#define MESSAGES_LOG_Y 60
#define MESSAGES_ROWS_Y 126
#define BAR_RGBA 0xFFFFFF30u
#define SCROLL_TRACK_RGBA 0xFFFFFF28u
#define SCROLL_THUMB_RGBA 0xFFFFFFA0u
#define ROOMS_TOP 78
#define ROOMS_VISIBLE 10
#define DIGIT_STEP 8
#define FIELD_LEFT 104

#define ADDRESS_DIGITS 12
#define PORT_DIGITS 5
#define PORT_MIN 1024u
#define PORT_MAX 65535u

enum {
    ROW_NONE = 0,
    ROW_HOST,
    ROW_FIND,
    ROW_ADDRESS,
    ROW_PORT,
    ROW_BACK,
    ROW_STOP_SEARCH,
    ROW_JOIN_RACE,
    ROW_JOIN_WATCH,
    ROW_ADDR_FIELD,
    ROW_ADDR_PORT,
    ROW_CANCEL,
    ROW_READY,
    ROW_RACE_NEXT,
    ROW_START,
    ROW_SEATS,
    ROW_LEAVE,
    ROW_KICK,
    ROW_DISMISS,
    ROW_TRACK,
    ROW_SKIP,
    ROW_END_SERIES,
    ROW_MESSAGE,
    ROW_MUTE,
    /* A message to send: ROW_MSG plus its place among the presets. */
    ROW_MSG = 60,
    /* A row of a list: ROW_LIST plus the room's place in the search, or plus the player's id. */
    ROW_LIST = 100
};

typedef struct Row {
    int id;
    int enabled;
} Row;

#define ROWS_MAX (8 + GDX_NET_ROOM_FOUND)

static GdxNetRoomView sView;
static int sProfileOk;
static int sPage = GDX_NET_ROOM_PAGE_FRONT;
static Row sRows[ROWS_MAX];
static int sRowCount;
static int sCursor;   /* index into sRows */
static int sCursorId; /* the id of that row */
static int sCursorFresh; /* the room page just opened and the player has given no input yet */
static int sTop;      /* first list row on screen */
static int sEdit;     /* 0, or 1 plus the digit being changed */
static int sEditBackup[ADDRESS_DIGITS];
static int sAddress[ADDRESS_DIGITS] = { 1, 9, 2, 1, 6, 8, 0, 0, 1, 0, 0, 1 };
static int sAddressPort[PORT_DIGITS] = { 2, 4, 3, 0, 2 };
static int sPortDigits[PORT_DIGITS];
static unsigned sFoundIp; /* the room of the found page */
static unsigned sFoundPort;
static int sPlayerId; /* the player of the player page */
static int sArmed;    /* the row that was pressed once and asks again, or ROW_NONE */
static int sWantFront; /* the main menu is to come up on the front page, with no session (want_front) */
static char sHostError[GDX_NET_ROOM_STATUS_BYTES];
static int sSound;
static GdxNetRoomDrawItem sItems[GDX_NET_ROOM_DRAW_MAX];
static int sItemCount;

/* ---- the pieces ---- */

static unsigned digits_value(const int* digits, int count) {
    unsigned value = 0;
    int i;

    for (i = 0; i < count; i++) {
        value = value * 10u + (unsigned) digits[i];
    }
    return value;
}

static void set_digits(int* digits, int count, unsigned value) {
    int i;

    for (i = count - 1; i >= 0; i--) {
        digits[i] = (int) (value % 10u);
        value /= 10u;
    }
}

static unsigned clamp_port(unsigned port) {
    return port < PORT_MIN ? PORT_MIN : port > PORT_MAX ? PORT_MAX : port;
}

/* The address being typed, a.b.c.d with a in the top byte. Each part is at most 255 outside of an edit. */
static unsigned address_ip(void) {
    unsigned ip = 0;
    int part;

    for (part = 0; part < 4; part++) {
        ip = (ip << 8) | (digits_value(&sAddress[part * 3], 3) & 0xFFu);
    }
    return ip;
}

/* ---- the room ---- */

static const GdxNetRoomPlayer* player_by_id(int id) {
    int i;

    for (i = 0; i < sView.playerCount; i++) {
        if (sView.players[i].id == id) {
            return &sView.players[i];
        }
    }
    return NULL;
}

static const GdxNetRoomPlayer* local_player(void) {
    return player_by_id(sView.localId);
}

static const GdxNetRoomFound* found_room(void) {
    int i;

    for (i = 0; i < sView.foundCount; i++) {
        if (sView.found[i].ip == sFoundIp && sView.found[i].port == sFoundPort) {
            return &sView.found[i];
        }
    }
    return NULL;
}

/* The row of the rooms page that holds the room of the found page. */
static int found_row(void) {
    const GdxNetRoomFound* room = found_room();

    return room != NULL ? ROW_LIST + (int) (room - sView.found) : ROW_NONE;
}

static int count_racers(void) {
    int count = 0;
    int i;

    for (i = 0; i < sView.playerCount; i++) {
        count += sView.players[i].watcher ? 0 : 1;
    }
    return count;
}

/* 1 while the room page shows the table of the race being run or just over in place of the plain roster. */
static int table_shown(void) {
    return (sView.phase == GDX_NET_ROOM_PHASE_RACING || sView.phase == GDX_NET_ROOM_PHASE_RESULTS) &&
           sView.haveResults && sView.resultsRace == sView.raceIndex && sView.resultCount > 0;
}

/* A player's line of that table and its place in it counted from 1, or NULL. */
static const GdxNetRoomResult* result_of(int id, int* place) {
    int i;

    for (i = 0; i < sView.resultCount; i++) {
        if (sView.results[i].id == id) {
            *place = i + 1;
            return &sView.results[i];
        }
    }
    return NULL;
}

/* ---- rows and the cursor ---- */

static void add_row(int id, int enabled) {
    if (sRowCount < ROWS_MAX) {
        sRows[sRowCount].id = id;
        sRows[sRowCount].enabled = enabled;
        sRowCount++;
    }
}

static void build_rows(void) {
    const GdxNetRoomPlayer* me;
    const GdxNetRoomFound* found;
    int place;
    int i;

    sRowCount = 0;
    switch (sPage) {
        case GDX_NET_ROOM_PAGE_FRONT:
            if (sProfileOk) {
                add_row(ROW_HOST, 1);
                add_row(ROW_FIND, 1);
                add_row(ROW_ADDRESS, 1);
                add_row(ROW_PORT, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_ADDRESS:
            add_row(ROW_ADDR_FIELD, 1);
            add_row(ROW_ADDR_PORT, 1);
            add_row(ROW_JOIN_RACE, address_ip() != 0);
            add_row(ROW_JOIN_WATCH, address_ip() != 0);
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_ROOMS:
            for (i = 0; i < sView.foundCount; i++) {
                add_row(ROW_LIST + i, 1);
            }
            add_row(ROW_STOP_SEARCH, 1);
            break;
        case GDX_NET_ROOM_PAGE_FOUND:
            found = found_room();
            add_row(ROW_JOIN_RACE, found != NULL && found->sameBuild && found->racers < found->racerCap);
            add_row(ROW_JOIN_WATCH, found != NULL && found->sameBuild && found->watchers < GDX_NET_ROOM_WATCH_SEATS);
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_JOINING:
            add_row(ROW_CANCEL, 1);
            break;
        case GDX_NET_ROOM_PAGE_ROOM:
            me = local_player();
            /* The vote or the pick the player has not made yet: the course select opens for it by itself,
               and this row opens it again for a player who came back from there. */
            if (gdx_net_track_choice_available(&sView) != GDX_NET_TRACK_CHOICE_NONE) {
                add_row(ROW_TRACK, 1);
            }
            if (me != NULL && me->watcher) {
                add_row(ROW_RACE_NEXT, 1);
            } else if (me != NULL && sView.phase == GDX_NET_ROOM_PHASE_LOBBY) {
                add_row(ROW_READY, 1);
            }
            if (sView.mode == GDX_NET_ROOM_MODE_HOSTING) {
                switch (sView.phase) {
                    case GDX_NET_ROOM_PHASE_LOBBY:
                        add_row(ROW_START, 1);
                        add_row(ROW_SEATS, 1);
                        break;
                    case GDX_NET_ROOM_PHASE_LOTTERY:
                        add_row(ROW_SKIP, 1);
                        break;
                    case GDX_NET_ROOM_PHASE_RESULTS:
                        add_row(ROW_END_SERIES, 1);
                        break;
                    default:
                        break;
                }
            }
            add_row(ROW_MESSAGE, 1);
            add_row(ROW_LEAVE, 1);
            /* The table's order first, then whoever is not in it. A line of a player who left stays, and the
               cursor passes it by. */
            if (table_shown()) {
                for (i = 0; i < sView.resultCount; i++) {
                    add_row(ROW_LIST + sView.results[i].id, player_by_id(sView.results[i].id) != NULL);
                }
            }
            for (i = 0; i < sView.playerCount; i++) {
                if (!table_shown() || result_of(sView.players[i].id, &place) == NULL) {
                    add_row(ROW_LIST + sView.players[i].id, 1);
                }
            }
            break;
        case GDX_NET_ROOM_PAGE_PLAYER:
            add_row(ROW_MUTE, 1);
            if (sView.mode == GDX_NET_ROOM_MODE_HOSTING) {
                add_row(ROW_KICK, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_MESSAGES:
            for (i = 0; i < GDX_NET_ROOM_MESSAGES; i++) {
                add_row(ROW_MSG + i, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        default:
            add_row(ROW_DISMISS, 1);
            break;
    }
}

/* Where the page's list sits: the index of its first row in sRows, how many rows it has, the row its first
 * line stands on and how many fit. A page without a list gives a count of 0. */
static void list_geometry(int* first, int* count, int* y, int* visible) {
    int i;

    *first = 0;
    *count = 0;
    *y = 0;
    *visible = 0;
    if (sPage == GDX_NET_ROOM_PAGE_ROOMS) {
        *count = sRowCount - 1;
        *y = ROOMS_TOP;
        *visible = ROOMS_VISIBLE;
    } else if (sPage == GDX_NET_ROOM_PAGE_ROOM) {
        for (i = 0; i < sRowCount && sRows[i].id < ROW_LIST; i++) {
        }
        *first = i;
        *count = sRowCount - i;
        /* A table has a row of column heads above its first line. */
        *y = table_shown() ? TABLE_ROWS_TOP + i * LIST_STEP + 8 + LIST_STEP : ROOM_ROWS_TOP + i * LIST_STEP + 8;
        *visible = (LIST_BOTTOM - *y) / LIST_STEP + 1;
    }
}

static void keep_visible(void) {
    int first;
    int count;
    int y;
    int visible;
    int line;

    list_geometry(&first, &count, &y, &visible);
    line = sCursor - first;
    if (line >= 0 && line < count) {
        if (line < sTop) {
            sTop = line;
        } else if (line >= sTop + visible) {
            sTop = line - visible + 1;
        }
    }
    if (sTop > count - visible) {
        sTop = count - visible;
    }
    if (sTop < 0) {
        sTop = 0;
    }
}

/* Puts the cursor back on its row after the rows were rebuilt, or on the nearest row that can be chosen. */
static void settle_cursor(void) {
    int found = -1;
    int i;

    for (i = 0; i < sRowCount; i++) {
        if (sRows[i].id == sCursorId) {
            found = i;
            break;
        }
    }
    if (found < 0 || !sRows[found].enabled) {
        int start = found >= 0 ? found : sCursor >= sRowCount ? sRowCount - 1 : sCursor < 0 ? 0 : sCursor;

        found = -1;
        for (i = start; i < sRowCount && found < 0; i++) {
            found = sRows[i].enabled ? i : -1;
        }
        for (i = start - 1; i >= 0 && found < 0; i--) {
            found = sRows[i].enabled ? i : -1;
        }
        if (found < 0) {
            found = 0;
        }
        sEdit = 0;
        sArmed = ROW_NONE;
    }
    sCursor = found;
    sCursorId = sRows[found].id;
    keep_visible();
}

static void go(int page, int cursorId) {
    sPage = page;
    sCursor = 0;
    sCursorId = cursorId;
    sCursorFresh = page == GDX_NET_ROOM_PAGE_ROOM && cursorId == ROW_NONE;
    sTop = 0;
    sEdit = 0;
    sArmed = ROW_NONE;
    build_rows();
    settle_cursor();
}

/* Reads the room and makes the page, the rows and the cursor agree with it. */
static void sync(void) {
    int page = sPage;

    gdx_lan_room_view(&sView);
    sProfileOk = gdx_lan_room_profile_ok();
    switch (sView.mode) {
        case GDX_NET_ROOM_MODE_IDLE:
            if (page != GDX_NET_ROOM_PAGE_ADDRESS) {
                page = GDX_NET_ROOM_PAGE_FRONT;
            }
            break;
        case GDX_NET_ROOM_MODE_BROWSING:
            if (page != GDX_NET_ROOM_PAGE_FOUND || found_room() == NULL) {
                page = GDX_NET_ROOM_PAGE_ROOMS;
            }
            break;
        case GDX_NET_ROOM_MODE_JOINING:
            page = GDX_NET_ROOM_PAGE_JOINING;
            break;
        case GDX_NET_ROOM_MODE_HOSTING:
        case GDX_NET_ROOM_MODE_JOINED:
            if (page == GDX_NET_ROOM_PAGE_PLAYER) {
                /* The page of a player lasts as long as their stay. */
                if (player_by_id(sPlayerId) == NULL || sPlayerId == sView.localId) {
                    page = GDX_NET_ROOM_PAGE_ROOM;
                }
            } else if (page != GDX_NET_ROOM_PAGE_MESSAGES) {
                page = GDX_NET_ROOM_PAGE_ROOM;
            }
            break;
        default:
            page = GDX_NET_ROOM_PAGE_CLOSED;
            break;
    }
    if (page != sPage) {
        int cursorId = ROW_NONE;

        /* The cursor returns to the row the player came through: a player's row, or "FIND ROOMS". */
        if (sPage == GDX_NET_ROOM_PAGE_PLAYER && page == GDX_NET_ROOM_PAGE_ROOM) {
            cursorId = ROW_LIST + sPlayerId;
        } else if (sPage == GDX_NET_ROOM_PAGE_ROOMS && page == GDX_NET_ROOM_PAGE_FRONT) {
            cursorId = ROW_FIND;
        }
        go(page, cursorId);
        return;
    }
    /* A row that asks twice stops asking once the cursor is off it: the player moved, or pressed another row. */
    if (sArmed != ROW_NONE && sArmed != sCursorId) {
        sArmed = ROW_NONE;
    }
    /* The first room to answer takes the cursor from "STOP SEARCHING", which was the only row: the next
     * press of A is then about that room and does not end the search. */
    if (sPage == GDX_NET_ROOM_PAGE_ROOMS && sRowCount == 1 && sView.foundCount > 0 &&
        sCursorId == ROW_STOP_SEARCH) {
        sCursorId = ROW_LIST;
    }
    /* A room that just opened has no roster for a moment, so its top row, "READY UP", is not there yet and a
     * cursor that followed its row would be on "START THE RACES" or on the leave row when it comes. The
     * cursor waits at the top until the player gives an input. */
    if (sCursorFresh) {
        sCursor = 0;
        sCursorId = ROW_NONE;
    }
    build_rows();
    settle_cursor();
}

static void move_cursor(int direction) {
    int i = sCursor + direction;

    while (i >= 0 && i < sRowCount && !sRows[i].enabled) {
        i += direction;
    }
    if (i < 0 || i >= sRowCount) {
        return;
    }
    sCursor = i;
    sCursorId = sRows[i].id;
    sSound = GDX_NET_ROOM_SOUND_MOVE;
    keep_visible();
}

/* ---- the number fields ---- */

/* The digits the cursor's row edits, or NULL when it is not a number field. */
static int* edit_digits(int* count) {
    switch (sCursorId) {
        case ROW_PORT:
            *count = PORT_DIGITS;
            return sPortDigits;
        case ROW_ADDR_FIELD:
            *count = ADDRESS_DIGITS;
            return sAddress;
        case ROW_ADDR_PORT:
            *count = PORT_DIGITS;
            return sAddressPort;
        default:
            *count = 0;
            return NULL;
    }
}

static void start_edit(void) {
    int count;
    int* digits;

    if (sCursorId == ROW_PORT) {
        set_digits(sPortDigits, PORT_DIGITS, clamp_port(gdx_lan_room_port()));
    }
    digits = edit_digits(&count);
    if (digits == NULL) {
        return;
    }
    memcpy(sEditBackup, digits, (size_t) count * sizeof(int));
    sEdit = 1;
    sSound = GDX_NET_ROOM_SOUND_CONFIRM;
}

static void edit_input(unsigned input) {
    int count;
    int* digits = edit_digits(&count);
    int part;

    if (digits == NULL) {
        sEdit = 0;
        return;
    }
    if ((input & GDX_NET_ROOM_IN_LEFT) && sEdit > 1) {
        sEdit--;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if ((input & GDX_NET_ROOM_IN_RIGHT) && sEdit < count) {
        sEdit++;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if (input & GDX_NET_ROOM_IN_UP) {
        digits[sEdit - 1] = (digits[sEdit - 1] + 1) % 10;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if (input & GDX_NET_ROOM_IN_DOWN) {
        digits[sEdit - 1] = (digits[sEdit - 1] + 9) % 10;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if (input & GDX_NET_ROOM_IN_BACK) {
        memcpy(digits, sEditBackup, (size_t) count * sizeof(int));
        sEdit = 0;
        sSound = GDX_NET_ROOM_SOUND_BACK;
        return;
    }
    if (!(input & GDX_NET_ROOM_IN_CONFIRM)) {
        return;
    }
    /* Done: a number outside what the field can hold becomes the nearest one it can. */
    if (sCursorId == ROW_ADDR_FIELD) {
        for (part = 0; part < 4; part++) {
            if (digits_value(&sAddress[part * 3], 3) > 255u) {
                set_digits(&sAddress[part * 3], 3, 255u);
            }
        }
    } else {
        set_digits(digits, PORT_DIGITS, clamp_port(digits_value(digits, PORT_DIGITS)));
        if (sCursorId == ROW_PORT) {
            gdx_lan_room_set_port(digits_value(digits, PORT_DIGITS));
        }
    }
    sEdit = 0;
    sSound = GDX_NET_ROOM_SOUND_CONFIRM;
}

/* ---- the buttons ---- */

static void join(int watch) {
    unsigned ip = sFoundIp;
    unsigned port = sFoundPort;

    if (sPage == GDX_NET_ROOM_PAGE_ADDRESS) {
        ip = address_ip();
        port = digits_value(sAddressPort, PORT_DIGITS);
    }
    sSound = gdx_lan_room_join(ip, port, watch) ? GDX_NET_ROOM_SOUND_CONFIRM : GDX_NET_ROOM_SOUND_REFUSE;
}

/* A press of a row that asks twice: 1 when it is the second one in a row. The first shows in the row's label. */
static int second_press(int id) {
    if (sArmed != id) {
        sArmed = id;
        return 0;
    }
    sArmed = ROW_NONE;
    return 1;
}

static int confirm(void) {
    const GdxNetRoomPlayer* me = local_player();
    const GdxNetRoomPlayer* other;

    sSound = GDX_NET_ROOM_SOUND_CONFIRM;
    if (sCursorId >= ROW_LIST) {
        const int key = sCursorId - ROW_LIST;

        if (sPage == GDX_NET_ROOM_PAGE_ROOMS && key < sView.foundCount) {
            sFoundIp = sView.found[key].ip;
            sFoundPort = sView.found[key].port;
            go(GDX_NET_ROOM_PAGE_FOUND, ROW_NONE);
        } else if (sPage == GDX_NET_ROOM_PAGE_ROOM && key != sView.localId && player_by_id(key) != NULL) {
            sPlayerId = key;
            go(GDX_NET_ROOM_PAGE_PLAYER, ROW_BACK);
        } else {
            sSound = GDX_NET_ROOM_SOUND_NONE; /* nothing to do with this row */
        }
        return GDX_NET_ROOM_SCREEN_STAY;
    }
    if (sCursorId >= ROW_MSG) {
        gdx_lan_room_message(sCursorId - ROW_MSG);
        go(GDX_NET_ROOM_PAGE_ROOM, ROW_MESSAGE);
        return GDX_NET_ROOM_SCREEN_STAY;
    }
    switch (sCursorId) {
        case ROW_HOST:
            sHostError[0] = '\0';
            if (!gdx_lan_room_host(sHostError, (int) sizeof(sHostError))) {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
            }
            break;
        case ROW_FIND:
            sHostError[0] = '\0';
            gdx_lan_room_browse();
            break;
        case ROW_ADDRESS:
            sHostError[0] = '\0';
            go(GDX_NET_ROOM_PAGE_ADDRESS, ROW_NONE);
            break;
        case ROW_PORT:
        case ROW_ADDR_FIELD:
        case ROW_ADDR_PORT:
            start_edit();
            break;
        case ROW_BACK:
            sSound = GDX_NET_ROOM_SOUND_BACK;
            if (sPage == GDX_NET_ROOM_PAGE_FRONT) {
                return GDX_NET_ROOM_SCREEN_LEAVE;
            }
            if (sPage == GDX_NET_ROOM_PAGE_ADDRESS) {
                go(GDX_NET_ROOM_PAGE_FRONT, ROW_ADDRESS);
            } else if (sPage == GDX_NET_ROOM_PAGE_FOUND) {
                go(GDX_NET_ROOM_PAGE_ROOMS, found_row());
            } else if (sPage == GDX_NET_ROOM_PAGE_MESSAGES) {
                go(GDX_NET_ROOM_PAGE_ROOM, ROW_MESSAGE);
            } else {
                go(GDX_NET_ROOM_PAGE_ROOM, ROW_LIST + sPlayerId);
            }
            break;
        case ROW_STOP_SEARCH:
        case ROW_CANCEL:
            sSound = GDX_NET_ROOM_SOUND_BACK;
            gdx_lan_room_stop();
            break;
        case ROW_JOIN_RACE:
            join(0);
            break;
        case ROW_JOIN_WATCH:
            join(1);
            break;
        case ROW_READY:
            gdx_lan_room_ready(me != NULL && !me->ready);
            break;
        case ROW_RACE_NEXT:
            gdx_lan_room_race_next(me != NULL && !me->raceNext);
            break;
        case ROW_START:
            gdx_lan_room_start();
            break;
        case ROW_TRACK:
            gdx_net_track_choice_request();
            break;
        case ROW_SEATS:
            sSound = GDX_NET_ROOM_SOUND_NONE; /* left and right change it */
            break;
        case ROW_LEAVE:
            if (second_press(ROW_LEAVE)) {
                sSound = GDX_NET_ROOM_SOUND_BACK;
                gdx_lan_room_stop();
            }
            break;
        case ROW_SKIP:
            gdx_lan_room_skip_lottery();
            break;
        case ROW_END_SERIES:
            /* Asked twice like the one above: it ends the series for everyone. */
            if (second_press(ROW_END_SERIES)) {
                gdx_lan_room_end_series();
            }
            break;
        case ROW_MESSAGE:
            go(GDX_NET_ROOM_PAGE_MESSAGES, ROW_NONE);
            break;
        case ROW_MUTE:
            other = player_by_id(sPlayerId);
            gdx_lan_room_set_muted(sPlayerId, other != NULL && !other->muted);
            break;
        case ROW_KICK:
            gdx_lan_room_kick(sPlayerId);
            go(GDX_NET_ROOM_PAGE_ROOM, ROW_NONE);
            break;
        case ROW_DISMISS:
            sSound = GDX_NET_ROOM_SOUND_BACK;
            gdx_lan_room_dismiss();
            break;
        default:
            sSound = GDX_NET_ROOM_SOUND_NONE;
            break;
    }
    return GDX_NET_ROOM_SCREEN_STAY;
}

static int back(void) {
    int i;

    sSound = GDX_NET_ROOM_SOUND_BACK;
    switch (sPage) {
        case GDX_NET_ROOM_PAGE_FRONT:
            return GDX_NET_ROOM_SCREEN_LEAVE;
        case GDX_NET_ROOM_PAGE_ADDRESS:
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_ADDRESS);
            break;
        case GDX_NET_ROOM_PAGE_ROOMS:
        case GDX_NET_ROOM_PAGE_JOINING:
            gdx_lan_room_stop();
            break;
        case GDX_NET_ROOM_PAGE_FOUND:
            go(GDX_NET_ROOM_PAGE_ROOMS, found_row());
            break;
        case GDX_NET_ROOM_PAGE_ROOM:
            /* B never leaves a room by itself: it takes the cursor to the row that does. A row that asks
               twice takes B as the answer no. */
            if (sArmed != ROW_NONE) {
                sArmed = ROW_NONE;
                break;
            }
            for (i = 0; i < sRowCount; i++) {
                if (sRows[i].id == ROW_LEAVE) {
                    sCursor = i;
                    sCursorId = ROW_LEAVE;
                }
            }
            break;
        case GDX_NET_ROOM_PAGE_PLAYER:
            go(GDX_NET_ROOM_PAGE_ROOM, ROW_LIST + sPlayerId);
            break;
        case GDX_NET_ROOM_PAGE_MESSAGES:
            go(GDX_NET_ROOM_PAGE_ROOM, ROW_MESSAGE);
            break;
        default:
            gdx_lan_room_dismiss();
            break;
    }
    return GDX_NET_ROOM_SCREEN_STAY;
}

int gdx_net_room_screen_wanted(void) {
    GdxNetRoomView view;

    gdx_lan_room_view(&view);
    return view.mode != GDX_NET_ROOM_MODE_IDLE || sWantFront;
}

void gdx_net_room_screen_want_front(void) {
    sWantFront = 1;
}

void gdx_net_room_screen_open(void) {
    sWantFront = 0;
    sHostError[0] = '\0';
    sSound = GDX_NET_ROOM_SOUND_NONE;
    sPage = -1; /* no page: sync puts the cursor at the top of the one the session gives */
    sync();
}

int gdx_net_room_screen_update(unsigned input) {
    int result = GDX_NET_ROOM_SCREEN_STAY;

    sSound = GDX_NET_ROOM_SOUND_NONE;
    sync();
    if (input != 0) {
        sCursorFresh = 0; /* the cursor is the player's from here on */
    }
    if (sEdit != 0) {
        edit_input(input);
        return GDX_NET_ROOM_SCREEN_STAY;
    }
    if (input & GDX_NET_ROOM_IN_UP) {
        move_cursor(-1);
    }
    if (input & GDX_NET_ROOM_IN_DOWN) {
        move_cursor(1);
    }
    if ((input & (GDX_NET_ROOM_IN_LEFT | GDX_NET_ROOM_IN_RIGHT)) && sCursorId == ROW_SEATS) {
        const int cap = sView.racerCap + ((input & GDX_NET_ROOM_IN_RIGHT) ? 1 : -1);

        if (cap >= GDX_NET_ROOM_SEATS_MIN && cap <= GDX_NET_ROOM_SEATS_MAX) {
            gdx_lan_room_racer_cap(cap);
            sSound = GDX_NET_ROOM_SOUND_MOVE;
        }
    }
    if (input & GDX_NET_ROOM_IN_CONFIRM) {
        result = confirm();
    } else if (input & GDX_NET_ROOM_IN_BACK) {
        result = back();
    }
    /* An action may have changed the session at once: the page follows in the same tick. */
    sync();
    return result;
}

int gdx_net_room_screen_take_sound(void) {
    const int sound = sSound;

    sSound = GDX_NET_ROOM_SOUND_NONE;
    return sound;
}

int gdx_net_room_screen_page(void) {
    return sPage;
}

int gdx_net_room_screen_row(void) {
    return sCursor;
}

int gdx_net_room_screen_editing(void) {
    return sEdit;
}

/* ---- drawing ---- */

static GdxNetRoomDrawItem* new_item(void) {
    GdxNetRoomDrawItem* item;

    if (sItemCount >= GDX_NET_ROOM_DRAW_MAX) {
        return NULL;
    }
    item = &sItems[sItemCount++];
    memset(item, 0, sizeof(*item));
    return item;
}

static void put_text(int x, int y, int font, int align, int colour, const char* text) {
    GdxNetRoomDrawItem* item = new_item();

    if (item == NULL) {
        return;
    }
    item->kind = GDX_NET_ROOM_DRAW_TEXT;
    item->font = (unsigned char) font;
    item->align = (unsigned char) align;
    item->colour = (unsigned char) colour;
    item->x = (short) x;
    item->y = (short) y;
    gdx_net_text_filter(font, text, item->text, (int) sizeof(item->text));
}

static void put_rect(int x, int y, int w, int h, unsigned rgba) {
    GdxNetRoomDrawItem* item = new_item();

    if (item == NULL) {
        return;
    }
    item->kind = GDX_NET_ROOM_DRAW_RECT;
    item->x = (short) x;
    item->y = (short) y;
    item->w = (short) w;
    item->h = (short) h;
    item->rgba = rgba;
}

static void put_title(const char* text) {
    put_text(MIDDLE, TITLE_Y, GDX_NET_ROOM_FONT_TITLE, GDX_NET_ROOM_ALIGN_CENTRE, GDX_NET_ROOM_COLOUR_GOLD, text);
}

static void put_line(int y, int colour, const char* text) {
    put_text(MARGIN_LEFT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, text);
}

/* A paragraph from y down, in lines of LIST_STEP. Returns the row after its last line. */
static int put_paragraph(int y, int colour, const char* text, int maxLines) {
    char lines[8][GDX_NET_ROOM_TEXT_BYTES];
    int count;
    int i;

    if (maxLines > 8) {
        maxLines = 8;
    }
    count = gdx_net_text_wrap(text, GDX_NET_ROOM_WRAP_COLUMNS, lines[0], GDX_NET_ROOM_TEXT_BYTES, maxLines);
    for (i = 0; i < count; i++) {
        put_line(y + i * LIST_STEP, colour, lines[i]);
    }
    return y + count * LIST_STEP;
}

/* The bar behind the row the cursor is on. Glyphs of the small font are 8 pixels high and stand on y. */
static void put_bar(int y) {
    put_rect(MARGIN_LEFT - 4, y - 9, MARGIN_RIGHT - MARGIN_LEFT + 8, 10, BAR_RGBA);
}

static const char* row_label(int id) {
    static char message[GDX_NET_ROOM_TEXT_BYTES];
    const GdxNetRoomPlayer* me = local_player();
    const GdxNetRoomPlayer* other = player_by_id(sPlayerId);
    const int host = sView.mode == GDX_NET_ROOM_MODE_HOSTING;

    if (id >= ROW_MSG && id < ROW_MSG + GDX_NET_ROOM_MESSAGES) {
        gdx_lan_room_message_text(id - ROW_MSG, message, (int) sizeof(message));
        return message;
    }
    switch (id) {
        case ROW_HOST:
            return "HOST A ROOM";
        case ROW_FIND:
            return "FIND ROOMS";
        case ROW_ADDRESS:
            return "JOIN BY ADDRESS";
        case ROW_PORT:
        case ROW_ADDR_PORT:
            return "PORT";
        case ROW_ADDR_FIELD:
            return "ADDRESS";
        case ROW_STOP_SEARCH:
            return "STOP SEARCHING";
        case ROW_JOIN_RACE:
            return "JOIN TO RACE";
        case ROW_JOIN_WATCH:
            return "JOIN TO WATCH";
        case ROW_CANCEL:
            return "CANCEL";
        case ROW_READY:
            return (me != NULL && me->ready) ? "CANCEL READY" : "READY UP";
        case ROW_RACE_NEXT:
            return (me != NULL && me->raceNext) ? "KEEP WATCHING" : "RACE NEXT";
        case ROW_START:
            return "START THE RACES";
        case ROW_TRACK:
            return gdx_net_track_choice_available(&sView) == GDX_NET_TRACK_CHOICE_PICK ? "PICK THE TRACK"
                                                                                       : "VOTE FOR A TRACK";
        case ROW_SEATS:
            return "RACE SEATS";
        case ROW_LEAVE:
            if (sArmed == ROW_LEAVE) {
                return host ? "PRESS A AGAIN TO CLOSE THE ROOM" : "PRESS A AGAIN TO LEAVE THE ROOM";
            }
            return host ? "CLOSE THE ROOM" : "LEAVE THE ROOM";
        case ROW_SKIP:
            return "SKIP THE LOTTERY";
        case ROW_END_SERIES:
            return sArmed == ROW_END_SERIES ? "PRESS A AGAIN TO GO BACK TO THE LOBBY" : "BACK TO THE LOBBY";
        case ROW_MESSAGE:
            return "SEND A MESSAGE";
        case ROW_MUTE:
            return (other != NULL && other->muted) ? "UNMUTE THIS PLAYER" : "MUTE THIS PLAYER";
        case ROW_KICK:
            return "REMOVE FROM THE ROOM";
        default:
            return "BACK";
    }
}

static int row_colour(int index) {
    return index == sCursor ? GDX_NET_ROOM_COLOUR_CURSOR
                            : sRows[index].enabled ? GDX_NET_ROOM_COLOUR_WHITE : GDX_NET_ROOM_COLOUR_DIM;
}

/* A row of the page's menu: its label, and the bar when the cursor is on it. */
static void put_row(int index, int y) {
    if (index == sCursor) {
        put_bar(y);
    }
    put_line(y, row_colour(index), row_label(sRows[index].id));
}

/* The digits of a number field from x, grouped in threes with a period between groups when grouped. The
 * digit being changed takes the cursor's colour. */
static void put_digits(int x, int y, const int* digits, int count, int grouped, int editing, int colour) {
    char one[2] = { '0', '\0' };
    int i;

    for (i = 0; i < count; i++) {
        if (grouped && i > 0 && (i % 3) == 0) {
            put_text(x, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, ".");
            x += DIGIT_STEP;
        }
        one[0] = (char) ('0' + digits[i]);
        put_text(x, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 (editing && sEdit == i + 1) ? GDX_NET_ROOM_COLOUR_CURSOR : colour, one);
        x += DIGIT_STEP;
    }
}

/* A number field's row: the label, and the digits in white while they are being changed. */
static void put_field_row(int index, int y, const int* digits, int count, int grouped) {
    const int editing = index == sCursor && sEdit != 0;

    put_row(index, y);
    put_digits(FIELD_LEFT, y, digits, count, grouped, editing, editing ? GDX_NET_ROOM_COLOUR_WHITE : row_colour(index));
}

/* A bar at the right margin that shows which part of a list is on screen. Nothing when all of it is. */
static void put_scroll(int y, int count, int visible) {
    const int height = visible * LIST_STEP;
    int thumb;

    if (count <= visible) {
        return;
    }
    thumb = height * visible / count;
    put_rect(MARGIN_RIGHT + 6, y - 9, 2, height, SCROLL_TRACK_RGBA);
    put_rect(MARGIN_RIGHT + 6, y - 9 + (height - thumb) * sTop / (count - visible), 2, thumb, SCROLL_THUMB_RGBA);
}

static const char* phase_title(int phase) {
    switch (phase) {
        case GDX_NET_ROOM_PHASE_LOBBY:
            return "LOBBY";
        case GDX_NET_ROOM_PHASE_VOTING:
            return "TRACK VOTE";
        case GDX_NET_ROOM_PHASE_LOTTERY:
            return "VOTE RESULT";
        case GDX_NET_ROOM_PHASE_PICK:
            return "TRACK PICK";
        case GDX_NET_ROOM_PHASE_MACHINE:
            return "MACHINE SELECT";
        case GDX_NET_ROOM_PHASE_GRID_SYNC:
            return "LOADING";
        case GDX_NET_ROOM_PHASE_COUNTDOWN:
            return "RACE START";
        case GDX_NET_ROOM_PHASE_RACING:
            return "RACE";
        default:
            return "RESULTS";
    }
}

/* What a player is doing, in a word or two, and whether that is something the room waits for. */
static const char* seat_text(const GdxNetRoomPlayer* player, int* done) {
    *done = 0;
    if (player->watcher) {
        return player->raceNext ? "NEXT RACE" : "WATCHING";
    }
    switch (sView.phase) {
        case GDX_NET_ROOM_PHASE_LOBBY:
            *done = player->ready;
            return player->ready ? "READY" : "NOT READY";
        case GDX_NET_ROOM_PHASE_VOTING:
            *done = player->voted;
            return player->voted ? "VOTED" : "VOTING";
        case GDX_NET_ROOM_PHASE_PICK:
            return player->id == sView.picker ? "PICKS" : "WAITING";
        case GDX_NET_ROOM_PHASE_MACHINE:
            *done = player->inRace && player->locked;
            return !player->inRace ? "SITS OUT" : player->locked ? "LOCKED IN" : "CHOOSING";
        case GDX_NET_ROOM_PHASE_GRID_SYNC:
        case GDX_NET_ROOM_PHASE_COUNTDOWN:
        case GDX_NET_ROOM_PHASE_RACING:
            return player->inRace ? "RACING" : "SITS OUT";
        default:
            return "";
    }
}

static const char* player_name(int id) {
    const GdxNetRoomPlayer* player = player_by_id(id);

    return player != NULL ? player->name : "A PLAYER WHO LEFT";
}

/* What the room is doing, as a sentence or two. */
static void room_info(char* out, int size) {
    char track[GDX_NET_ROOM_TEXT_BYTES];
    int voted = 0;
    int i;

    gdx_lan_room_track_name(sView.phase == GDX_NET_ROOM_PHASE_LOTTERY ? sView.lotteryTrack : sView.track, track,
                            (int) sizeof(track));
    switch (sView.phase) {
        case GDX_NET_ROOM_PHASE_LOBBY:
            snprintf(out, (size_t) size, "THE RACES START WHEN EVERYONE IS READY OR WHEN %s.",
                     sView.mode == GDX_NET_ROOM_MODE_HOSTING ? "YOU START THEM" : "THE HOST STARTS THEM");
            break;
        case GDX_NET_ROOM_PHASE_VOTING:
            for (i = 0; i < sView.playerCount; i++) {
                voted += sView.players[i].voted ? 1 : 0;
            }
            snprintf(out, (size_t) size, "THE RACERS VOTE FOR THE NEXT TRACK.\n%d OF %d VOTED.", voted, count_racers());
            break;
        case GDX_NET_ROOM_PHASE_LOTTERY:
            if (sView.lotteryWinner < 0 || sView.lotteryWinner >= sView.lotteryCount) {
                snprintf(out, (size_t) size, "NOBODY VOTED.\nTHE ROOM DREW %s.", track);
            } else {
                snprintf(out, (size_t) size, "%s WON THE VOTE.\nNEXT TRACK %s.",
                         player_name(sView.lottery[sView.lotteryWinner].id), track);
            }
            break;
        case GDX_NET_ROOM_PHASE_PICK:
            if (sView.picker == sView.localId) {
                snprintf(out, (size_t) size, "YOU FINISHED LAST AND PICK THE NEXT TRACK.");
            } else {
                snprintf(out, (size_t) size, "%s FINISHED LAST AND PICKS THE NEXT TRACK.", player_name(sView.picker));
            }
            break;
        case GDX_NET_ROOM_PHASE_MACHINE:
            snprintf(out, (size_t) size, "NEXT TRACK %s.\n%d OF %d LOCKED IN.", track, sView.lockedCount, sView.racers);
            break;
        case GDX_NET_ROOM_PHASE_GRID_SYNC:
            snprintf(out, (size_t) size, "LOADING %s.", track);
            break;
        case GDX_NET_ROOM_PHASE_COUNTDOWN:
            snprintf(out, (size_t) size, "%s IS ABOUT TO START.", track);
            break;
        case GDX_NET_ROOM_PHASE_RACING:
            snprintf(out, (size_t) size, "THE RACE IS ON AT %s.%s", track,
                     table_shown() ? "" : "\nNOBODY HAS FINISHED YET.");
            break;
        default:
            snprintf(out, (size_t) size, "THE RACE AT %s IS OVER.", track);
            break;
    }
}

/* "192.168.1.20:24302" as the view gives it becomes "192.168.1.20 PORT 24302": the small font has no colon. */
static void address_words(const char* address, char* out, int size) {
    const char* colon = strchr(address, ':');

    if (colon == NULL) {
        snprintf(out, (size_t) size, "%s", address);
    } else {
        snprintf(out, (size_t) size, "%.*s PORT %s", (int) (colon - address), address, colon + 1);
    }
}

static void format_address(unsigned ip, unsigned port, char* out, int size) {
    snprintf(out, (size_t) size, "%u.%u.%u.%u PORT %u", (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu,
             ip & 0xFFu, port);
}

static void draw_front(void) {
    static const char kNotice[] =
        "HOSTING OR SEARCHING OR JOINING SHARES YOUR PROFILE NAME AND COLOUR AND FAVOURITE MACHINE AND THE "
        "MESSAGES YOU SEND WITH THE ROOM. PLAYERS ON YOUR NETWORK CAN SEE YOUR IP ADDRESS. NOTHING IS SENT "
        "BEFORE THAT.";
    int i;

    put_title("G-NET");
    if (!sProfileOk) {
        put_paragraph(70, GDX_NET_ROOM_COLOUR_WHITE,
                      "MAKE A PROFILE FIRST. OPEN THE MENU AND GO TO ONLINE AND THEN PROFILE. A ROOM SHOWS "
                      "ITS NAME AND COLOUR.",
                      4);
        put_row(0, 122);
        return;
    }
    for (i = 0; i < sRowCount; i++) {
        const int y = 62 + i * MENU_STEP;

        if (sRows[i].id == ROW_PORT) {
            if (i == sCursor && sEdit != 0) {
                put_field_row(i, y, sPortDigits, PORT_DIGITS, 0);
            } else {
                int digits[PORT_DIGITS];

                set_digits(digits, PORT_DIGITS, clamp_port(gdx_lan_room_port()));
                put_field_row(i, y, digits, PORT_DIGITS, 0);
            }
        } else {
            put_row(i, y);
        }
    }
    if (sEdit != 0) {
        put_paragraph(126, GDX_NET_ROOM_COLOUR_DIM,
                      "LEFT AND RIGHT PICK A DIGIT. UP AND DOWN CHANGE IT. A KEEPS THE PORT. B DOES NOT.", 3);
    } else if (sHostError[0] != '\0') {
        put_paragraph(126, GDX_NET_ROOM_COLOUR_RED, sHostError, 3);
    }
    put_paragraph(162, GDX_NET_ROOM_COLOUR_DIM, kNotice, 6);
}

static void draw_address(void) {
    put_title("JOIN BY ADDRESS");
    put_field_row(0, 66, sAddress, ADDRESS_DIGITS, 1);
    put_field_row(1, 66 + MENU_STEP, sAddressPort, PORT_DIGITS, 0);
    put_row(2, 98);
    put_row(3, 98 + MENU_STEP);
    put_row(4, 98 + 2 * MENU_STEP);
    if (sEdit != 0) {
        put_paragraph(146, GDX_NET_ROOM_COLOUR_DIM,
                      "LEFT AND RIGHT PICK A DIGIT. UP AND DOWN CHANGE IT. A KEEPS THE NUMBER. B DOES NOT.", 3);
    } else if (address_ip() == 0) {
        /* No apostrophe: the small font draws one far from its letter. */
        put_paragraph(146, GDX_NET_ROOM_COLOUR_DIM, "SET THE ADDRESS OF THE HOST FIRST.", 2);
    } else {
        put_paragraph(146, GDX_NET_ROOM_COLOUR_DIM,
                      "ASK THE HOST FOR THE ADDRESS OF THEIR PC. THE PORT IS 24302 UNLESS THEY CHANGED IT.",
                      3);
    }
}

static void draw_rooms(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int i;

    put_title("FIND ROOMS");
    if (sView.foundCount == 0) {
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, "SEARCHING YOUR NETWORK. NO ROOM YET.");
    } else {
        snprintf(text, sizeof(text), "%d %s ON YOUR NETWORK", sView.foundCount,
                 sView.foundCount == 1 ? "ROOM" : "ROOMS");
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, text);
    }
    for (i = sTop; i < sView.foundCount && i < sTop + ROOMS_VISIBLE; i++) {
        const GdxNetRoomFound* room = &sView.found[i];
        const int y = ROOMS_TOP + (i - sTop) * LIST_STEP;
        const int colour = i == sCursor ? GDX_NET_ROOM_COLOUR_CURSOR
                                        : room->sameBuild ? GDX_NET_ROOM_COLOUR_WHITE : GDX_NET_ROOM_COLOUR_DIM;

        if (i == sCursor) {
            put_bar(y);
        }
        put_line(y, colour, room->hostName);
        snprintf(text, sizeof(text), "%u OF %u", (unsigned) room->racers, (unsigned) room->racerCap);
        put_text(164, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, text);
        put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, colour,
                 !room->sameBuild ? "OTHER BUILD" : room->phase == GDX_NET_ROOM_PHASE_LOBBY ? "LOBBY" : "RACING");
    }
    put_scroll(ROOMS_TOP, sView.foundCount, ROOMS_VISIBLE);
    put_row(sRowCount - 1, ROOMS_TOP + ROOMS_VISIBLE * LIST_STEP + 10);
}

static void draw_found(void) {
    const GdxNetRoomFound* room = found_room();
    char text[GDX_NET_ROOM_TEXT_BYTES];
    char address[GDX_NET_ROOM_ADDRESS_BYTES + 8];
    int y = 142;

    if (room == NULL) {
        return; /* sync left this page when the room went */
    }
    put_title(room->hostName);
    format_address(room->ip, room->port, address, (int) sizeof(address));
    snprintf(text, sizeof(text), "ADDRESS %s", address);
    put_line(60, GDX_NET_ROOM_COLOUR_DIM, text);
    snprintf(text, sizeof(text), "%u OF %u RACERS - %u OF %u WATCHING", (unsigned) room->racers,
             (unsigned) room->racerCap, (unsigned) room->watchers, (unsigned) GDX_NET_ROOM_WATCH_SEATS);
    put_line(70, GDX_NET_ROOM_COLOUR_DIM, text);
    put_line(80, GDX_NET_ROOM_COLOUR_DIM,
             room->phase == GDX_NET_ROOM_PHASE_LOBBY ? "IN THE LOBBY." : "RACING. YOU JOIN FOR THE NEXT RACE.");
    put_row(0, 100);
    put_row(1, 100 + MENU_STEP);
    put_row(2, 100 + 2 * MENU_STEP);
    if (!room->sameBuild) {
        put_paragraph(y, GDX_NET_ROOM_COLOUR_RED, "THIS ROOM RUNS ANOTHER BUILD OF THE GAME.", 2);
        return;
    }
    if (room->racers >= room->racerCap) {
        y = put_paragraph(y, GDX_NET_ROOM_COLOUR_DIM, "EVERY RACE SEAT IS TAKEN.", 1);
    }
    if (room->watchers >= GDX_NET_ROOM_WATCH_SEATS) {
        put_paragraph(y, GDX_NET_ROOM_COLOUR_DIM, "EVERY WATCH SEAT IS TAKEN.", 1);
    }
}

static void draw_joining(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    char address[GDX_NET_ROOM_ADDRESS_BYTES + 8];

    put_title("JOINING");
    address_words(sView.hostText, address, (int) sizeof(address));
    snprintf(text, sizeof(text), "JOINING %s", address);
    put_line(70, GDX_NET_ROOM_COLOUR_WHITE, text);
    put_row(0, 100);
}

static void draw_closed(void) {
    put_title("G-NET");
    put_paragraph(70, GDX_NET_ROOM_COLOUR_WHITE, sView.status[0] != '\0' ? sView.status : "THE ROOM CLOSED.", 4);
    put_row(0, 122);
}

static void draw_player(void) {
    const GdxNetRoomPlayer* player = player_by_id(sPlayerId);
    int i;

    put_title(player_name(sPlayerId));
    for (i = 0; i < sRowCount; i++) {
        put_row(i, 80 + i * MENU_STEP);
    }
    /* No apostrophe: the small font draws one far from its letter. */
    put_paragraph(80 + sRowCount * MENU_STEP + 12, GDX_NET_ROOM_COLOUR_DIM,
                  (player != NULL && player->muted) ? "MESSAGES FROM THIS PLAYER ARE HIDDEN ON YOUR SCREEN."
                                                    : "MUTING HIDES MESSAGES FROM THIS PLAYER ON YOUR SCREEN ONLY.",
                  2);
}

/* A line of the log as it stands on screen. */
typedef struct LogLine {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int player;   /* a player's message, and not a notice of the room */
    int head;     /* the first line of its entry */
    unsigned rgb; /* the player's colour */
} LogLine;

/* The newest entries of the log that fit in maxLines lines of the screen, oldest first. An entry takes one
 * line or two and is never shown in part. A player's entry reads "NAME - MESSAGE": the small font has no
 * colon. Returns the number of lines. */
static int log_lines(LogLine* out, int maxLines) {
    char wrapped[GDX_NET_ROOM_LINES][2][GDX_NET_ROOM_TEXT_BYTES];
    char text[GDX_NET_ROOM_NAME_BYTES + GDX_NET_ROOM_LINE_BYTES + 4];
    int counts[GDX_NET_ROOM_LINES];
    int firstEntry = sView.lineCount;
    int used = 0;
    int count = 0;
    int i;
    int k;

    for (i = sView.lineCount - 1; i >= 0; i--) {
        const GdxNetRoomLine* line = &sView.lines[i];
        const int player = line->from != GDX_NET_ROOM_NOBODY;

        if (player) {
            snprintf(text, sizeof(text), "%s - %s", line->name, line->text);
        } else {
            snprintf(text, sizeof(text), "%s", line->text);
        }
        /* A player's entry starts after the square of their colour, two columns in. */
        counts[i] = gdx_net_text_wrap(text, GDX_NET_ROOM_WRAP_COLUMNS - (player ? 2 : 0), wrapped[i][0],
                                      GDX_NET_ROOM_TEXT_BYTES, 2);
        if (used + counts[i] > maxLines) {
            break;
        }
        used += counts[i];
        firstEntry = i;
    }
    for (i = firstEntry; i < sView.lineCount; i++) {
        for (k = 0; k < counts[i]; k++) {
            snprintf(out[count].text, sizeof(out[count].text), "%s", wrapped[i][k]);
            out[count].player = sView.lines[i].from != GDX_NET_ROOM_NOBODY;
            out[count].head = k == 0;
            out[count].rgb = sView.lines[i].colour & 0xFFFFFFu;
            count++;
        }
    }
    return count;
}

/* Those lines from y down. A message has the square of its player's colour before it and is white; a notice
 * is dim. Returns the number of lines. */
static int put_log(int y, int maxLines) {
    LogLine lines[LOG_LINES_MAX];
    const int count = log_lines(lines, maxLines > LOG_LINES_MAX ? LOG_LINES_MAX : maxLines);
    int i;

    for (i = 0; i < count; i++) {
        const int row = y + i * LIST_STEP;

        if (!lines[i].player) {
            put_line(row, GDX_NET_ROOM_COLOUR_DIM, lines[i].text);
            continue;
        }
        if (lines[i].head) {
            put_rect(MARGIN_LEFT + 2, row - 7, 6, 6, (lines[i].rgb << 8) | 0xFFu);
        }
        put_text(MARGIN_LEFT + 12, row, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_WHITE,
                 lines[i].text);
    }
    return count;
}

static void draw_messages(void) {
    int i;

    put_title("MESSAGES");
    if (put_log(MESSAGES_LOG_Y, LOG_LINES_MAX) == 0) {
        put_line(MESSAGES_LOG_Y, GDX_NET_ROOM_COLOUR_DIM, "NO MESSAGE YET.");
    }
    /* The messages to send, and "BACK" a little apart from them. */
    for (i = 0; i < sRowCount; i++) {
        put_row(i, MESSAGES_ROWS_Y + i * LIST_STEP + (sRows[i].id == ROW_BACK ? 4 : 0));
    }
}

/* A line of the race's table: the place, the name, the time or what became of the racer, what the race is
 * worth and the points of the series. A player who is not in the table has their seat in the time's column. */
static void put_table_row(int y, int selected, int id) {
    const GdxNetRoomPlayer* player = player_by_id(id);
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int place = 0;
    const GdxNetRoomResult* result = result_of(id, &place);
    int done;

    if (result != NULL && (sView.resultsFinal || result->kind == GDX_NET_ROOM_RESULT_FINISHED)) {
        snprintf(text, sizeof(text), "%d", place);
        put_text(MARGIN_LEFT + 10, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE,
                 text);
    }
    put_text(TABLE_NAME_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
             selected ? GDX_NET_ROOM_COLOUR_CURSOR
                      : player == NULL ? GDX_NET_ROOM_COLOUR_DIM
                                       : id == sView.localId ? GDX_NET_ROOM_COLOUR_GOLD : GDX_NET_ROOM_COLOUR_WHITE,
             player != NULL ? player->name : "LEFT THE ROOM");
    if (result == NULL) {
        if (player != NULL) {
            put_text(TABLE_TIME_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_DIM,
                     seat_text(player, &done));
        }
    } else if (result->kind == GDX_NET_ROOM_RESULT_FINISHED) {
        snprintf(text, sizeof(text), "%u'%02u\"%03u", result->timeMs / 60000u, (result->timeMs / 1000u) % 60u,
                 result->timeMs % 1000u);
        put_text(TABLE_TIME_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_WHITE, text);
    } else if (result->kind == GDX_NET_ROOM_RESULT_RETIRED) {
        put_text(TABLE_TIME_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_RED, "RETIRED");
    } else {
        put_text(TABLE_TIME_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_DIM,
                 sView.resultsFinal ? "DID NOT FINISH" : "RACING");
    }
    if (result != NULL && sView.resultsFinal) {
        snprintf(text, sizeof(text), "+%u", (unsigned) result->points);
        put_text(TABLE_POINTS_X, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE, text);
    }
    if (player != NULL) {
        snprintf(text, sizeof(text), "%u", player->points);
        put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE, text);
    }
}

static void draw_room(void) {
    char text[GDX_NET_ROOM_STATUS_BYTES];
    const int racers = count_racers();
    const int table = table_shown();
    const int rowsTop = table ? TABLE_ROWS_TOP : ROOM_ROWS_TOP;
    int anyPoints = 0;
    int first;
    int count;
    int listY;
    int visible;
    int i;

    put_text(MARGIN_LEFT, TITLE_Y, GDX_NET_ROOM_FONT_TITLE, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_GOLD,
             phase_title(sView.phase));
    if (sView.phaseTotalMs != 0) {
        gdx_net_text_format_timer(sView.phaseLeftMs, text, (int) sizeof(text));
        /* In the title's font, the one of the two with a colon. */
        put_text(MARGIN_RIGHT, TITLE_Y, GDX_NET_ROOM_FONT_TITLE, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE,
                 text);
    }
    /* A table needs the rows the room's address and its seat counts stand on. */
    if (!table) {
        if (sView.mode == GDX_NET_ROOM_MODE_HOSTING) {
            snprintf(text, sizeof(text), "YOUR ROOM ON PORT %u", sView.port);
        } else {
            char address[GDX_NET_ROOM_ADDRESS_BYTES + 8];

            address_words(sView.hostText, address, (int) sizeof(address));
            snprintf(text, sizeof(text), "ROOM AT %s", address);
        }
        put_line(58, GDX_NET_ROOM_COLOUR_DIM, text);
        snprintf(text, sizeof(text), "%d OF %d RACERS - %d OF %d WATCHING", racers, sView.racerCap,
                 sView.playerCount - racers, GDX_NET_ROOM_WATCH_SEATS);
        put_line(68, GDX_NET_ROOM_COLOUR_DIM, text);
    }
    room_info(text, (int) sizeof(text));
    put_paragraph(table ? 58 : 82, GDX_NET_ROOM_COLOUR_WHITE, text, 2);

    list_geometry(&first, &count, &listY, &visible);
    for (i = 0; i < first; i++) {
        const int y = rowsTop + i * LIST_STEP;

        put_row(i, y);
        if (sRows[i].id == ROW_SEATS) {
            snprintf(text, sizeof(text), "%d", sView.racerCap);
            put_text(FIELD_LEFT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, row_colour(i), text);
        }
    }

    for (i = 0; i < sView.playerCount; i++) {
        anyPoints |= sView.players[i].points != 0;
    }
    if (table) {
        put_text(TABLE_TIME_X, listY - LIST_STEP, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 GDX_NET_ROOM_COLOUR_DIM, "TIME");
        if (sView.resultsFinal) {
            put_text(TABLE_POINTS_X, listY - LIST_STEP, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT,
                     GDX_NET_ROOM_COLOUR_DIM, "PTS");
        }
        put_text(MARGIN_RIGHT, listY - LIST_STEP, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT,
                 GDX_NET_ROOM_COLOUR_DIM, "TOTAL");
    }
    for (i = sTop; i < count && i < sTop + visible; i++) {
        const GdxNetRoomPlayer* player = player_by_id(sRows[first + i].id - ROW_LIST);
        const int y = listY + (i - sTop) * LIST_STEP;
        const int selected = first + i == sCursor;
        const char* seat;
        int done;

        if (selected) {
            put_bar(y);
        }
        if (table) {
            put_table_row(y, selected, sRows[first + i].id - ROW_LIST);
            continue;
        }
        if (player == NULL) {
            continue;
        }
        put_rect(MARGIN_LEFT + 2, y - 7, 6, 6, ((player->colour & 0xFFFFFFu) << 8) | 0xFFu);
        put_text(MARGIN_LEFT + 12, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 selected ? GDX_NET_ROOM_COLOUR_CURSOR
                          : player->id == sView.localId ? GDX_NET_ROOM_COLOUR_GOLD : GDX_NET_ROOM_COLOUR_WHITE,
                 player->name);
        seat = seat_text(player, &done);
        put_text(168, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 done ? GDX_NET_ROOM_COLOUR_GREEN : GDX_NET_ROOM_COLOUR_DIM, seat);
        if (anyPoints) {
            snprintf(text, sizeof(text), "%u", player->points);
            put_text(258, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_WHITE, text);
        }
        if (player->muted) {
            snprintf(text, sizeof(text), "MUTED");
        } else if (player->host) {
            snprintf(text, sizeof(text), "HOST");
        } else {
            snprintf(text, sizeof(text), "%u MS", player->pingMs);
        }
        put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_DIM, text);
    }
    put_scroll(listY, count, visible);
    put_log(FOOT_Y, FOOT_LINES);
}

const GdxNetRoomDrawItem* gdx_net_room_screen_draw(int* count) {
    sync();
    sItemCount = 0;
    switch (sPage) {
        case GDX_NET_ROOM_PAGE_FRONT:
            draw_front();
            break;
        case GDX_NET_ROOM_PAGE_ADDRESS:
            draw_address();
            break;
        case GDX_NET_ROOM_PAGE_ROOMS:
            draw_rooms();
            break;
        case GDX_NET_ROOM_PAGE_FOUND:
            draw_found();
            break;
        case GDX_NET_ROOM_PAGE_JOINING:
            draw_joining();
            break;
        case GDX_NET_ROOM_PAGE_ROOM:
            draw_room();
            break;
        case GDX_NET_ROOM_PAGE_PLAYER:
            draw_player();
            break;
        case GDX_NET_ROOM_PAGE_MESSAGES:
            draw_messages();
            break;
        default:
            draw_closed();
            break;
    }
    *count = sItemCount;
    return sItems;
}
