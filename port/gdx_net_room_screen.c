/* port/gdx_net_room_screen.c -- see gdx_net_room_screen.h.
 * Copyright (c) Zorkats
 *
 * The page follows the session: no session shows the front page or the address page, a search shows the
 * rooms, a room shows the room. Within a page the cursor is kept by the row it is on and not by its number,
 * so it stays on "LEAVE THE ROOM" when the rows above it come and go with the room's state.
 */

#include "gdx_net_room_screen.h"

#include "gdx_net_track_choice.h"
#include "gdx_net_track_save.h"

#include <ctype.h>
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
#define FRONT_ROWS_Y 58
#define FRONT_TEXT_Y 182 /* under the tenth row of the front page */
#define NOTE_Y 212       /* what the last press on a friend page did, two lines */
#define FRIENDS_ROWS_Y 96
#define FRIENDS_LIST_Y 156
#define FRIEND_ROWS_Y 90
#define FRIEND_STATUS_Y 132
#define FRIEND_LIST_Y 146
#define FRIEND_FIELD_Y 78
#define MET_LIST_Y 72
#define MET_VISIBLE 12
#define CODE_GROUP 4         /* a friend code is shown in groups of four */
#define CODE_GROUPS_PER_LINE 7

/* Crockford base32, the alphabet of a room code (port/net/gdx_net_codes.h). */
static const char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
#define CODE_RADIX 32

/* The host's upload per seat it sends to, in kbit/s: measured in L2 (ODD 6.2), UDP and IP headers included. */
#define UPLOAD_KBIT_PER_SEAT 493u

#define ADDRESS_DIGITS 12
#define EDIT_MAX GDX_NET_ROOM_FRIEND_CODE_CHARS /* the longest field: a friend code */
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
    ROW_CUSTOM,
    ROW_CPU_FILL,
    ROW_LEAVE,
    ROW_KICK,
    ROW_DISMISS,
    ROW_TRACK,
    ROW_SKIP,
    ROW_END_SERIES,
    ROW_MESSAGE,
    ROW_MUTE,
    ROW_SAVE_TRACK,
    ROW_QUICK,
    ROW_OPEN,
    ROW_CODE,
    ROW_CODE_FIELD,
    ROW_REFRESH,
    ROW_HOST_OPEN,
    ROW_ABOUT,
    ROW_NOTICE_OK,
    ROW_FRIENDS,
    ROW_COPY_CODE,
    ROW_ADD_CODE,
    ROW_ADD_MET,
    ROW_FRIEND_FIELD,
    ROW_PASTE,
    ROW_ADD_FRIEND,
    ROW_FIND_FRIEND,
    ROW_REMOVE_FRIEND,
    ROW_BAN,
    /* A message to send: ROW_MSG plus its place among the presets. */
    ROW_MSG = 60,
    /* A row of a list: ROW_LIST plus the room's place in the search, the player's id or the friend's place. */
    ROW_LIST = 100
};

typedef struct Row {
    int id;
    int enabled;
} Row;

/* The longest page is the friend list: its four rows and every friend. */
#define ROWS_MAX (8 + GDX_NET_ROOM_FRIENDS_MAX)

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
static int sEditBackup[EDIT_MAX];
static int sAddress[ADDRESS_DIGITS] = { 1, 9, 2, 1, 6, 8, 0, 0, 1, 0, 0, 1 };
static int sCode[GDX_NET_ROOM_CODE_CHARS]; /* places in kCodeAlphabet */
static int sFriendCode[GDX_NET_ROOM_FRIEND_CODE_CHARS]; /* the friend code being typed, places as above */
static char sMyCode[GDX_NET_ROOM_FRIEND_CODE_BYTES];    /* the player's own, read when the friend list opens */
static int sFriendIndex;       /* the friend of the friend page */
static int sFriendRoomsOf = -1; /* the friend the directory was last asked about, or -1 */
static char sNote[GDX_NET_ROOM_STATUS_BYTES]; /* what the last press on a friend page did */
static int sNoteColour;
static int sHostType = GDX_NET_ROOM_TYPE_LAN; /* the type HOST A ROOM hosts, left and right change it */
static int sWaitSeconds;  /* a refused directory press: what was left of the cooldown, or 0 */
static unsigned sAskSerial; /* the directory's answer count when the page last asked */
static int sAsked;        /* the page asked, and its answer has not come yet or was not acted on */
static int sQuickNone;    /* Quick race's last answer: no room to join */
static int sNoticeFromAbout; /* the notice page was opened from its row, after it was read */
static int sDirectoryOk;
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

/* ---- friends ---- */

/* 1 when the directory's last answer is the rooms of the friend on the friend page. */
static int friend_rooms_shown(void) {
    return sFriendRoomsOf >= 0 && sFriendRoomsOf == sFriendIndex && sView.dirAsk == GDX_NET_ROOM_ASK_FRIEND &&
           sView.dirStatus == GDX_NET_ROOM_DIR_READY;
}

static int friend_page(int page) {
    return page == GDX_NET_ROOM_PAGE_FRIENDS || page == GDX_NET_ROOM_PAGE_FRIEND_ADD ||
           page == GDX_NET_ROOM_PAGE_FRIEND || page == GDX_NET_ROOM_PAGE_MET;
}

static void set_note(int colour, const char* text) {
    snprintf(sNote, sizeof(sNote), "%s", text);
    sNoteColour = colour;
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
    int save;
    int i;

    sRowCount = 0;
    switch (sPage) {
        case GDX_NET_ROOM_PAGE_FRONT:
            if (sProfileOk) {
                add_row(ROW_HOST, 1);
                /* Rooms on the internet need a directory: with none set they are not offered, and the page
                   says so under its rows. */
                if (sDirectoryOk) {
                    add_row(ROW_QUICK, 1);
                    add_row(ROW_OPEN, 1);
                    add_row(ROW_CODE, 1);
                }
                add_row(ROW_FIND, 1);
                add_row(ROW_ADDRESS, 1);
                add_row(ROW_PORT, 1);
                add_row(ROW_FRIENDS, 1);
                add_row(ROW_ABOUT, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_NOTICE:
            if (!gdx_lan_room_notice_seen()) {
                add_row(ROW_NOTICE_OK, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_CODE:
            add_row(ROW_CODE_FIELD, 1);
            add_row(ROW_JOIN_RACE, 1);
            add_row(ROW_JOIN_WATCH, 1);
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_OPEN:
            if (sView.dirAsk == GDX_NET_ROOM_ASK_OPEN && sView.dirStatus == GDX_NET_ROOM_DIR_READY) {
                for (i = 0; i < sView.listedCount; i++) {
                    add_row(ROW_LIST + i, 1);
                }
            }
            add_row(ROW_REFRESH, sView.dirStatus != GDX_NET_ROOM_DIR_ASKING);
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_QUICK:
            add_row(ROW_HOST_OPEN, sQuickNone);
            add_row(ROW_REFRESH, !sAsked && sView.dirStatus != GDX_NET_ROOM_DIR_ASKING);
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
            add_row(ROW_JOIN_RACE, found != NULL && found->racers < found->racerCap);
            add_row(ROW_JOIN_WATCH, found != NULL && found->watchers < GDX_NET_ROOM_WATCH_SEATS);
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
            /* The results of a race on a custom track: the one way it reaches this player's Edit Cup. */
            save = gdx_net_track_save_state(&sView);
            if (save != GDX_NET_TRACK_SAVE_NONE) {
                add_row(ROW_SAVE_TRACK, save == GDX_NET_TRACK_SAVE_READY || save == GDX_NET_TRACK_SAVE_FAILED);
            }
            if (sView.mode == GDX_NET_ROOM_MODE_HOSTING) {
                switch (sView.phase) {
                    case GDX_NET_ROOM_PHASE_LOBBY:
                        add_row(ROW_START, 1);
                        add_row(ROW_SEATS, 1);
                        add_row(ROW_CUSTOM, 1);
                        add_row(ROW_CPU_FILL, 1);
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
                add_row(ROW_BAN, 1);
            }
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_FRIENDS:
            add_row(ROW_COPY_CODE, sMyCode[0] != '\0');
            add_row(ROW_ADD_CODE, 1);
            add_row(ROW_ADD_MET, 1);
            add_row(ROW_BACK, 1);
            place = gdx_lan_room_friend_count();
            for (i = 0; i < place && i < GDX_NET_ROOM_FRIENDS_MAX; i++) {
                add_row(ROW_LIST + i, 1);
            }
            break;
        case GDX_NET_ROOM_PAGE_FRIEND_ADD:
            add_row(ROW_FRIEND_FIELD, 1);
            add_row(ROW_PASTE, 1);
            add_row(ROW_ADD_FRIEND, 1);
            add_row(ROW_BACK, 1);
            break;
        case GDX_NET_ROOM_PAGE_FRIEND:
            /* None without a directory. A press while a request is out is refused, and the row stays where it
               is so the cursor does not fall onto REMOVE. */
            add_row(ROW_FIND_FRIEND, sDirectoryOk);
            add_row(ROW_REMOVE_FRIEND, 1);
            add_row(ROW_BACK, 1);
            if (friend_rooms_shown()) {
                for (i = 0; i < sView.listedCount; i++) {
                    add_row(ROW_LIST + i, 1);
                }
            }
            break;
        case GDX_NET_ROOM_PAGE_MET:
            /* A player already on the list under the name the room showed has nothing to add. */
            place = gdx_lan_room_met_count();
            for (i = 0; i < place && i < GDX_NET_ROOM_MET_MAX; i++) {
                add_row(ROW_LIST + i, !gdx_lan_room_met_listed(i));
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
    } else if (sPage == GDX_NET_ROOM_PAGE_OPEN) {
        *count = sRowCount - 2; /* "REFRESH" and "BACK" follow the list */
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
    } else if (sPage == GDX_NET_ROOM_PAGE_FRIENDS || sPage == GDX_NET_ROOM_PAGE_FRIEND) {
        /* The page's rows, then the list under them. */
        *first = sPage == GDX_NET_ROOM_PAGE_FRIENDS ? 4 : 3;
        *count = sRowCount - *first;
        *y = sPage == GDX_NET_ROOM_PAGE_FRIENDS ? FRIENDS_LIST_Y : FRIEND_LIST_Y;
        *visible = (LIST_BOTTOM - *y) / LIST_STEP + 1;
    } else if (sPage == GDX_NET_ROOM_PAGE_MET) {
        *count = sRowCount - 1; /* "BACK" follows the list */
        *y = MET_LIST_Y;
        *visible = MET_VISIBLE;
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
    if (page == GDX_NET_ROOM_PAGE_FRIENDS && sPage != GDX_NET_ROOM_PAGE_FRIENDS) {
        gdx_lan_room_my_friend_code(sMyCode, (int) sizeof(sMyCode));
    }
    sNote[0] = '\0';
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
    sDirectoryOk = gdx_lan_room_directory_ok();
    if (!sDirectoryOk) {
        sHostType = GDX_NET_ROOM_TYPE_LAN;
    }
    switch (sView.mode) {
        case GDX_NET_ROOM_MODE_IDLE:
            if (page != GDX_NET_ROOM_PAGE_ADDRESS && page != GDX_NET_ROOM_PAGE_CODE &&
                page != GDX_NET_ROOM_PAGE_OPEN && page != GDX_NET_ROOM_PAGE_QUICK &&
                page != GDX_NET_ROOM_PAGE_NOTICE && !friend_page(page)) {
                page = GDX_NET_ROOM_PAGE_FRONT;
            }
            /* The friend list is the active profile's key: with no profile there is none to show. */
            if (friend_page(page) && !sProfileOk) {
                page = GDX_NET_ROOM_PAGE_FRONT;
            }
            if (page == GDX_NET_ROOM_PAGE_FRIEND && sFriendIndex >= gdx_lan_room_friend_count()) {
                page = GDX_NET_ROOM_PAGE_FRIENDS;
            }
            /* Nothing leaves this copy before the player has read what will (ODD 10). */
            if (page == GDX_NET_ROOM_PAGE_FRONT && sProfileOk && !gdx_lan_room_notice_seen()) {
                page = GDX_NET_ROOM_PAGE_NOTICE;
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
    /* The same for a friend's rooms. */
    if (sPage == GDX_NET_ROOM_PAGE_FRIEND && sAsked && sView.dirSerial != sAskSerial) {
        sAsked = 0;
        if (friend_rooms_shown() && sView.listedCount > 0) {
            sCursorId = ROW_LIST;
        }
    }
    /* The same for the Open rooms: a fresh answer puts the cursor on its first room, not on BACK. */
    if (sPage == GDX_NET_ROOM_PAGE_OPEN && sAsked && sView.dirSerial != sAskSerial) {
        sAsked = 0;
        if (sView.dirAsk == GDX_NET_ROOM_ASK_OPEN && sView.dirStatus == GDX_NET_ROOM_DIR_READY &&
            sView.listedCount > 0) {
            sCursorId = ROW_LIST;
        }
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

/* The digits the cursor's row edits, or NULL when it is not a number field. A room code is a field of places
 * in kCodeAlphabet. */
static int* edit_digits(int* count) {
    switch (sCursorId) {
        case ROW_CODE_FIELD:
            *count = GDX_NET_ROOM_CODE_CHARS;
            return sCode;
        case ROW_FRIEND_FIELD:
            *count = GDX_NET_ROOM_FRIEND_CODE_CHARS;
            return sFriendCode;
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
    const int radix = (sCursorId == ROW_CODE_FIELD || sCursorId == ROW_FRIEND_FIELD) ? CODE_RADIX : 10;
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
        digits[sEdit - 1] = (digits[sEdit - 1] + 1) % radix;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if (input & GDX_NET_ROOM_IN_DOWN) {
        digits[sEdit - 1] = (digits[sEdit - 1] + radix - 1) % radix;
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
    /* Done: a number outside what the field can hold becomes the nearest one it can. Every code is one. */
    if (sCursorId == ROW_CODE_FIELD || sCursorId == ROW_FRIEND_FIELD) {
    } else if (sCursorId == ROW_ADDR_FIELD) {
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

/* The code being typed, "ABC-DEF". */
static void code_text(char* out) {
    int i;
    int at = 0;

    for (i = 0; i < GDX_NET_ROOM_CODE_CHARS; i++) {
        if (i == GDX_NET_ROOM_CODE_CHARS / 2) {
            out[at++] = '-';
        }
        out[at++] = kCodeAlphabet[sCode[i]];
    }
    out[at] = '\0';
}

/* Asks the directory for a page. A press inside the cooldown sends nothing and keeps what was left of it. */
static void ask_directory(int ask) {
    int wait = 0;

    sWaitSeconds = 0;
    sQuickNone = 0;
    if (gdx_lan_room_ask(ask, &wait)) {
        sAsked = 1;
        sAskSerial = sView.dirSerial;
        return;
    }
    sAsked = 0;
    sWaitSeconds = wait;
    sSound = GDX_NET_ROOM_SOUND_REFUSE;
}

/* Quick race's answer, once it comes: its room is joined at once, and none is a question for the player. */
static void quick_poll(void) {
    if (sPage != GDX_NET_ROOM_PAGE_QUICK || !sAsked || sView.dirSerial == sAskSerial) {
        return;
    }
    sAsked = 0;
    if (sView.dirAsk != GDX_NET_ROOM_ASK_QUICK || sView.dirStatus != GDX_NET_ROOM_DIR_READY) {
        return; /* a failure: the page shows the directory's text */
    }
    if (!sView.quickFound) {
        sQuickNone = 1;
    } else if (!gdx_lan_room_join_listed(-1, 0)) {
        sSound = GDX_NET_ROOM_SOUND_REFUSE;
    }
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

/* A character of a pasted code as a place in kCodeAlphabet, read the way the code parser reads it: small
 * letters as capitals, I and L as 1, O as 0. -1 for any other character. */
static int code_place(char c) {
    const char* at;

    if (c >= 'a' && c <= 'z') {
        c = (char) (c - 'a' + 'A');
    }
    if (c == 'I' || c == 'L') {
        c = '1';
    } else if (c == 'O') {
        c = '0';
    }
    at = c != '\0' ? strchr(kCodeAlphabet, c) : NULL;
    return at != NULL ? (int) (at - kCodeAlphabet) : -1;
}

/* The clipboard into the friend code field. Spaces, hyphens and line ends are passed over; anything else that
 * is not a letter of a code, or a length other than a code's, leaves the field as it was. */
static void paste_code(void) {
    char text[GDX_NET_ROOM_FRIEND_CODE_BYTES * 2];
    int places[GDX_NET_ROOM_FRIEND_CODE_CHARS];
    int count = 0;
    int i;

    if (!gdx_lan_room_clipboard_get(text, (int) sizeof(text))) {
        sSound = GDX_NET_ROOM_SOUND_REFUSE;
        set_note(GDX_NET_ROOM_COLOUR_RED, "THE CLIPBOARD HOLDS NO TEXT.");
        return;
    }
    for (i = 0; text[i] != '\0' && count >= 0; i++) {
        const char c = text[i];
        int place;

        if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        place = code_place(c);
        if (place < 0 || count == GDX_NET_ROOM_FRIEND_CODE_CHARS) {
            count = -1;
            break;
        }
        places[count++] = place;
    }
    if (count != GDX_NET_ROOM_FRIEND_CODE_CHARS) {
        sSound = GDX_NET_ROOM_SOUND_REFUSE;
        set_note(GDX_NET_ROOM_COLOUR_RED, "THE CLIPBOARD HOLDS NO FRIEND CODE.");
        return;
    }
    memcpy(sFriendCode, places, sizeof(sFriendCode));
    sCursorId = ROW_ADD_FRIEND;
    set_note(GDX_NET_ROOM_COLOUR_WHITE, "PASTED. CHECK IT AND ADD THIS FRIEND.");
}

/* What an add did, in the note. name is the friend's name after it. */
static void friend_add_note(int result, const char* name) {
    char text[GDX_NET_ROOM_STATUS_BYTES];

    switch (result) {
        case GDX_NET_ROOM_FRIEND_ADDED:
            snprintf(text, sizeof(text), "%s IS ON YOUR LIST.", name);
            set_note(GDX_NET_ROOM_COLOUR_WHITE, text);
            return;
        case GDX_NET_ROOM_FRIEND_RENAMED:
            snprintf(text, sizeof(text), "YOUR LIST NOW CALLS THIS FRIEND %s.", name);
            set_note(GDX_NET_ROOM_COLOUR_WHITE, text);
            return;
        case GDX_NET_ROOM_FRIEND_LISTED:
            set_note(GDX_NET_ROOM_COLOUR_DIM, "THIS FRIEND IS ON YOUR LIST ALREADY.");
            return;
        case GDX_NET_ROOM_FRIEND_BAD_CODE:
            set_note(GDX_NET_ROOM_COLOUR_RED, "THIS IS NOT A FRIEND CODE. CHECK EVERY LETTER.");
            break;
        case GDX_NET_ROOM_FRIEND_SELF:
            set_note(GDX_NET_ROOM_COLOUR_RED, "THIS IS YOUR OWN FRIEND CODE.");
            break;
        case GDX_NET_ROOM_FRIEND_FULL:
            set_note(GDX_NET_ROOM_COLOUR_RED, "THE LIST IS FULL. REMOVE A FRIEND FIRST.");
            break;
        case GDX_NET_ROOM_FRIEND_BAD_NAME:
            set_note(GDX_NET_ROOM_COLOUR_RED, "THIS NAME CANNOT GO ON YOUR LIST.");
            break;
        case GDX_NET_ROOM_FRIEND_GONE:
            set_note(GDX_NET_ROOM_COLOUR_RED, "THIS PLAYER IS NO LONGER LISTED.");
            break;
        default:
            set_note(GDX_NET_ROOM_COLOUR_RED, "YOUR LIST COULD NOT BE SAVED.");
            break;
    }
    sSound = GDX_NET_ROOM_SOUND_REFUSE;
}

/* ADD THIS FRIEND: the typed code onto the list. A new friend takes the cursor to their row of the list. */
static void add_typed_friend(void) {
    char code[GDX_NET_ROOM_FRIEND_CODE_CHARS + 1];
    char name[GDX_NET_ROOM_NAME_BYTES];
    int result;
    int count;
    int i;

    for (i = 0; i < GDX_NET_ROOM_FRIEND_CODE_CHARS; i++) {
        code[i] = kCodeAlphabet[sFriendCode[i]];
    }
    code[GDX_NET_ROOM_FRIEND_CODE_CHARS] = '\0';
    result = gdx_lan_room_friend_add_code(code);
    if (result != GDX_NET_ROOM_FRIEND_ADDED) {
        friend_add_note(result, "");
        return;
    }
    count = gdx_lan_room_friend_count();
    gdx_lan_room_friend_name(count - 1, name, (int) sizeof(name));
    memset(sFriendCode, 0, sizeof(sFriendCode));
    go(GDX_NET_ROOM_PAGE_FRIENDS, ROW_LIST + count - 1);
    friend_add_note(result, name);
}

static void ask_friend(void) {
    int wait = 0;

    sWaitSeconds = 0;
    if (gdx_lan_room_ask_friend(sFriendIndex, &wait)) {
        sAsked = 1;
        sAskSerial = sView.dirSerial;
        sFriendRoomsOf = sFriendIndex;
        return;
    }
    sAsked = 0;
    sWaitSeconds = wait;
    sSound = GDX_NET_ROOM_SOUND_REFUSE;
}

/* REMOVE, the second press: the cursor goes to the friend after them on the list, or the one before. */
static void remove_friend(void) {
    char name[GDX_NET_ROOM_NAME_BYTES];
    char text[GDX_NET_ROOM_STATUS_BYTES];
    int count;

    gdx_lan_room_friend_name(sFriendIndex, name, (int) sizeof(name));
    if (!gdx_lan_room_friend_remove(sFriendIndex)) {
        sSound = GDX_NET_ROOM_SOUND_REFUSE;
        set_note(GDX_NET_ROOM_COLOUR_RED, "YOUR LIST COULD NOT BE SAVED.");
        return;
    }
    sFriendRoomsOf = -1;
    sAsked = 0;
    count = gdx_lan_room_friend_count();
    go(GDX_NET_ROOM_PAGE_FRIENDS,
       count == 0 ? ROW_ADD_CODE : ROW_LIST + (sFriendIndex < count ? sFriendIndex : count - 1));
    snprintf(text, sizeof(text), "%s IS OFF YOUR LIST.", name);
    set_note(GDX_NET_ROOM_COLOUR_WHITE, text);
    sSound = GDX_NET_ROOM_SOUND_BACK;
}

static int back(void);

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
        } else if (sPage == GDX_NET_ROOM_PAGE_OPEN && key < sView.listedCount) {
            /* A full room still has its watch seats. */
            if (!gdx_lan_room_join_listed(key, sView.listed[key].freeSeats == 0)) {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
            }
        } else if (sPage == GDX_NET_ROOM_PAGE_ROOM && key != sView.localId && player_by_id(key) != NULL) {
            sPlayerId = key;
            go(GDX_NET_ROOM_PAGE_PLAYER, ROW_BACK);
        } else if (sPage == GDX_NET_ROOM_PAGE_FRIENDS && key < gdx_lan_room_friend_count()) {
            sFriendIndex = key;
            sWaitSeconds = 0;
            go(GDX_NET_ROOM_PAGE_FRIEND, ROW_NONE);
        } else if (sPage == GDX_NET_ROOM_PAGE_FRIEND && friend_rooms_shown() && key < sView.listedCount) {
            if (!gdx_lan_room_join_listed(key, sView.listed[key].freeSeats == 0)) {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
            }
        } else if (sPage == GDX_NET_ROOM_PAGE_MET && key < gdx_lan_room_met_count()) {
            char name[GDX_NET_ROOM_NAME_BYTES];
            const int result = gdx_lan_room_met_add(key);

            gdx_lan_room_met_name(key, name, (int) sizeof(name));
            friend_add_note(result, name);
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
        case ROW_HOST_OPEN:
            sHostError[0] = '\0';
            if (!gdx_lan_room_host(sCursorId == ROW_HOST_OPEN ? GDX_NET_ROOM_TYPE_OPEN : sHostType, sHostError,
                                   (int) sizeof(sHostError))) {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
                if (sPage != GDX_NET_ROOM_PAGE_FRONT) {
                    go(GDX_NET_ROOM_PAGE_FRONT, ROW_HOST); /* where the reason shows */
                }
            }
            break;
        case ROW_QUICK:
            sHostError[0] = '\0';
            go(GDX_NET_ROOM_PAGE_QUICK, ROW_NONE);
            ask_directory(GDX_NET_ROOM_ASK_QUICK);
            break;
        case ROW_OPEN:
            sHostError[0] = '\0';
            go(GDX_NET_ROOM_PAGE_OPEN, ROW_NONE);
            ask_directory(GDX_NET_ROOM_ASK_OPEN);
            break;
        case ROW_REFRESH:
            ask_directory(sPage == GDX_NET_ROOM_PAGE_QUICK ? GDX_NET_ROOM_ASK_QUICK : GDX_NET_ROOM_ASK_OPEN);
            break;
        case ROW_CODE:
            sHostError[0] = '\0';
            go(GDX_NET_ROOM_PAGE_CODE, ROW_NONE);
            break;
        case ROW_ABOUT:
            sNoticeFromAbout = 1;
            go(GDX_NET_ROOM_PAGE_NOTICE, ROW_BACK);
            break;
        case ROW_NOTICE_OK:
            gdx_lan_room_notice_accept();
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_NONE);
            break;
        case ROW_FRIENDS:
            sHostError[0] = '\0';
            go(GDX_NET_ROOM_PAGE_FRIENDS, ROW_NONE);
            break;
        case ROW_COPY_CODE:
            if (sMyCode[0] != '\0' && gdx_lan_room_clipboard_set(sMyCode)) {
                set_note(GDX_NET_ROOM_COLOUR_WHITE, "YOUR FRIEND CODE IS ON THE CLIPBOARD.");
            } else {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
                set_note(GDX_NET_ROOM_COLOUR_RED, "THE CLIPBOARD COULD NOT BE SET.");
            }
            break;
        case ROW_ADD_CODE:
            go(GDX_NET_ROOM_PAGE_FRIEND_ADD, ROW_PASTE);
            break;
        case ROW_ADD_MET:
            go(GDX_NET_ROOM_PAGE_MET, ROW_NONE);
            break;
        case ROW_PASTE:
            paste_code();
            break;
        case ROW_ADD_FRIEND:
            add_typed_friend();
            break;
        case ROW_FIND_FRIEND:
            if (sAsked || sView.dirStatus == GDX_NET_ROOM_DIR_ASKING) {
                sSound = GDX_NET_ROOM_SOUND_REFUSE;
            } else {
                ask_friend();
            }
            break;
        case ROW_REMOVE_FRIEND:
            if (second_press(ROW_REMOVE_FRIEND)) {
                remove_friend();
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
        case ROW_CODE_FIELD:
        case ROW_FRIEND_FIELD:
            start_edit();
            break;
        case ROW_BACK:
            return back();
        case ROW_STOP_SEARCH:
        case ROW_CANCEL:
            sSound = GDX_NET_ROOM_SOUND_BACK;
            gdx_lan_room_stop();
            break;
        case ROW_JOIN_RACE:
        case ROW_JOIN_WATCH:
            if (sPage == GDX_NET_ROOM_PAGE_CODE) {
                char code[GDX_NET_ROOM_CODE_BYTES];

                code_text(code);
                if (!gdx_lan_room_join_code(code, sCursorId == ROW_JOIN_WATCH)) {
                    sSound = GDX_NET_ROOM_SOUND_REFUSE;
                }
            } else {
                join(sCursorId == ROW_JOIN_WATCH);
            }
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
        case ROW_CUSTOM:
            gdx_lan_room_custom_tracks(!sView.customTracks);
            break;
        case ROW_CPU_FILL:
            gdx_lan_room_cpu_fill(!sView.cpuFill);
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
        case ROW_BAN:
            /* Asked twice: the player cannot come back while this copy hosts the room (O7.3). */
            if (second_press(ROW_BAN)) {
                gdx_lan_room_ban(sPlayerId);
                go(GDX_NET_ROOM_PAGE_ROOM, ROW_NONE);
            }
            break;
        case ROW_SAVE_TRACK:
            switch (gdx_net_track_save_begin(&sView)) {
                case GDX_NET_TRACK_SAVE_SAVING:
                case GDX_NET_TRACK_SAVE_SAVED:
                    break;
                default:
                    sSound = GDX_NET_ROOM_SOUND_REFUSE;
                    break;
            }
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
        case GDX_NET_ROOM_PAGE_CODE:
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_CODE);
            break;
        case GDX_NET_ROOM_PAGE_OPEN:
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_OPEN);
            break;
        case GDX_NET_ROOM_PAGE_QUICK:
            sAsked = 0; /* an answer that comes after this joins nothing */
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_QUICK);
            break;
        case GDX_NET_ROOM_PAGE_NOTICE:
            /* Not read: the screen is left, as nothing on it may be used yet. */
            if (!gdx_lan_room_notice_seen()) {
                return GDX_NET_ROOM_SCREEN_LEAVE;
            }
            go(GDX_NET_ROOM_PAGE_FRONT, sNoticeFromAbout ? ROW_ABOUT : ROW_NONE);
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
        case GDX_NET_ROOM_PAGE_FRIENDS:
            go(GDX_NET_ROOM_PAGE_FRONT, ROW_FRIENDS);
            break;
        case GDX_NET_ROOM_PAGE_FRIEND_ADD:
            go(GDX_NET_ROOM_PAGE_FRIENDS, ROW_ADD_CODE);
            break;
        case GDX_NET_ROOM_PAGE_FRIEND:
            sAsked = 0;
            go(GDX_NET_ROOM_PAGE_FRIENDS, ROW_LIST + sFriendIndex);
            break;
        case GDX_NET_ROOM_PAGE_MET:
            go(GDX_NET_ROOM_PAGE_FRIENDS, ROW_ADD_MET);
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
    sAsked = 0;
    sQuickNone = 0;
    sWaitSeconds = 0;
    sNoticeFromAbout = 0;
    sSound = GDX_NET_ROOM_SOUND_NONE;
    sPage = -1; /* no page: sync puts the cursor at the top of the one the session gives */
    sync();
}

int gdx_net_room_screen_update(unsigned input) {
    int result = GDX_NET_ROOM_SCREEN_STAY;

    sSound = GDX_NET_ROOM_SOUND_NONE;
    gdx_net_track_save_poll(); /* a save queued before the page closed is collected all the same */
    sync();
    quick_poll();
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
    if ((input & (GDX_NET_ROOM_IN_LEFT | GDX_NET_ROOM_IN_RIGHT)) && sCursorId == ROW_HOST && sDirectoryOk) {
        sHostType = (sHostType + ((input & GDX_NET_ROOM_IN_RIGHT) ? 1 : GDX_NET_ROOM_TYPES - 1)) % GDX_NET_ROOM_TYPES;
        sSound = GDX_NET_ROOM_SOUND_MOVE;
    }
    if ((input & (GDX_NET_ROOM_IN_LEFT | GDX_NET_ROOM_IN_RIGHT)) && sCursorId == ROW_SEATS) {
        const int cap = sView.racerCap + ((input & GDX_NET_ROOM_IN_RIGHT) ? 1 : -1);

        if (cap >= GDX_NET_ROOM_SEATS_MIN && cap <= GDX_NET_ROOM_SEATS_MAX) {
            gdx_lan_room_racer_cap(cap);
            sSound = GDX_NET_ROOM_SOUND_MOVE;
        }
    }
    if ((input & (GDX_NET_ROOM_IN_LEFT | GDX_NET_ROOM_IN_RIGHT)) && sCursorId == ROW_CUSTOM) {
        const int on = (input & GDX_NET_ROOM_IN_RIGHT) ? 1 : 0;

        if (on != sView.customTracks) {
            gdx_lan_room_custom_tracks(on);
            sSound = GDX_NET_ROOM_SOUND_MOVE;
        }
    }
    if ((input & (GDX_NET_ROOM_IN_LEFT | GDX_NET_ROOM_IN_RIGHT)) && sCursorId == ROW_CPU_FILL) {
        const int on = (input & GDX_NET_ROOM_IN_RIGHT) ? 1 : 0;

        if (on != sView.cpuFill) {
            gdx_lan_room_cpu_fill(on);
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
#define PARAGRAPH_LINES_MAX 14
static int put_paragraph(int y, int colour, const char* text, int maxLines) {
    char lines[PARAGRAPH_LINES_MAX][GDX_NET_ROOM_TEXT_BYTES];
    int count;
    int i;

    if (maxLines > PARAGRAPH_LINES_MAX) {
        maxLines = PARAGRAPH_LINES_MAX;
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
            /* With no directory a room is on this network, and there is nothing to choose. */
            if (!sDirectoryOk) {
                return "HOST A ROOM";
            }
            switch (sHostType) {
                case GDX_NET_ROOM_TYPE_CODE:
                    return "HOST A ROOM BY CODE";
                case GDX_NET_ROOM_TYPE_OPEN:
                    return "HOST AN OPEN ROOM";
                case GDX_NET_ROOM_TYPE_FRIENDS:
                    return "HOST A ROOM FOR FRIENDS";
                default:
                    return "HOST A ROOM ON THIS NETWORK";
            }
        case ROW_HOST_OPEN:
            return "HOST AN OPEN ROOM";
        case ROW_QUICK:
            return "QUICK RACE";
        case ROW_OPEN:
            return "OPEN ROOMS";
        case ROW_CODE:
            return "JOIN BY CODE";
        case ROW_CODE_FIELD:
            return "CODE";
        case ROW_REFRESH:
            return sPage == GDX_NET_ROOM_PAGE_QUICK ? "ASK AGAIN" : "REFRESH";
        case ROW_ABOUT:
            return "WHAT G-NET SENDS";
        case ROW_NOTICE_OK:
            return "I UNDERSTAND";
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
        case ROW_CUSTOM:
            return "CUSTOM TRACKS";
        case ROW_CPU_FILL:
            return "CPU RACERS";
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
        case ROW_BAN:
            return sArmed == ROW_BAN ? "PRESS A AGAIN TO BAN" : "BAN FROM THE ROOM";
        case ROW_FRIENDS:
            return "FRIENDS";
        case ROW_COPY_CODE:
            return "COPY MY CODE";
        case ROW_ADD_CODE:
            return "ADD BY CODE";
        case ROW_ADD_MET:
            return "ADD FROM THE LAST ROOM";
        case ROW_FRIEND_FIELD:
            return "CODE";
        case ROW_PASTE:
            return "PASTE";
        case ROW_ADD_FRIEND:
            return "ADD THIS FRIEND";
        case ROW_FIND_FRIEND:
            return "FIND THEIR ROOMS";
        case ROW_REMOVE_FRIEND:
            return sArmed == ROW_REMOVE_FRIEND ? "PRESS A AGAIN TO REMOVE" : "REMOVE FROM FRIENDS";
        case ROW_SAVE_TRACK:
            switch (gdx_net_track_save_state(&sView)) {
                case GDX_NET_TRACK_SAVE_SAVING:
                    return "SAVING THE TRACK";
                case GDX_NET_TRACK_SAVE_SAVED:
                    return "SAVED IN YOUR EDIT CUP";
                case GDX_NET_TRACK_SAVE_FULL:
                    return "YOUR EDIT CUP IS FULL";
                case GDX_NET_TRACK_SAVE_FAILED:
                    return "TRY SAVING AGAIN";
                case GDX_NET_TRACK_SAVE_OWN:
                    return "ALREADY IN YOUR EDIT CUP";
                default:
                    return "SAVE TO MY TRACKS";
            }
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
 * digit being changed takes the cursor's colour. A room code's places are letters of kCodeAlphabet and its
 * groups have a hyphen between them. */
static void put_digits(int x, int y, const int* digits, int count, int grouped, int editing, int colour,
                       int code) {
    char one[2] = { '0', '\0' };
    int i;

    for (i = 0; i < count; i++) {
        if (grouped && i > 0 && (i % 3) == 0) {
            put_text(x, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, code ? "-" : ".");
            x += DIGIT_STEP;
        }
        one[0] = code ? kCodeAlphabet[digits[i]] : (char) ('0' + digits[i]);
        put_text(x, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 (editing && sEdit == i + 1) ? GDX_NET_ROOM_COLOUR_CURSOR : colour, one);
        x += DIGIT_STEP;
    }
}

/* A number field's row: the label, and the digits in white while they are being changed. */
static void put_field_row(int index, int y, const int* digits, int count, int grouped) {
    const int editing = index == sCursor && sEdit != 0;

    put_row(index, y);
    put_digits(FIELD_LEFT, y, digits, count, grouped, editing, editing ? GDX_NET_ROOM_COLOUR_WHITE : row_colour(index),
               sRows[index].id == ROW_CODE_FIELD);
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
        case GDX_NET_ROOM_PHASE_ASSETS:
            return "GETTING THE TRACK";
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
        case GDX_NET_ROOM_PHASE_ASSETS:
            return player->inRace ? "DOWNLOADING" : "SITS OUT";
        default:
            return "";
    }
}

static const char* player_name(int id) {
    const GdxNetRoomPlayer* player = player_by_id(id);

    return player != NULL ? player->name : "A PLAYER WHO LEFT";
}

/* A track's name as the room screens show it: a stock track's, or a custom track's title. */
static void track_text(int track, char* out, int size) {
    if (track == GDX_NET_ROOM_TRACK_CUSTOM) {
        snprintf(out, (size_t) size, "%s", sView.trackTitle[0] != '\0' ? sView.trackTitle : "A CUSTOM TRACK");
        return;
    }
    gdx_lan_room_track_name(track, out, size);
}

/* What the room is doing, as a sentence or two. */
static void room_info(char* out, int size) {
    char track[GDX_NET_ROOM_TEXT_BYTES];
    int voted = 0;
    int i;

    track_text(sView.phase == GDX_NET_ROOM_PHASE_LOTTERY ? sView.lotteryTrack : sView.track, track,
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
        case GDX_NET_ROOM_PHASE_ASSETS:
            if (sView.trackChunksOf > 0u && !sView.trackHeld && !sView.trackFailed) {
                snprintf(out, (size_t) size, "NEXT TRACK %s.\nGOT %u OF %u PARTS FROM THE HOST.", track,
                         sView.trackChunks, sView.trackChunksOf);
            } else {
                snprintf(out, (size_t) size, "NEXT TRACK %s.\nTHE RACERS GET IT FROM THE HOST.", track);
            }
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

/* "192.168.1.20:24302" as the view gives it becomes "192.168.1.20 PORT 24302": the small font has no colon.
 * An IPv6 address comes as "[2001:db8::1]:24302" and becomes "2001 DB8 .. 1 PORT 24302": a space between
 * groups, ".." where the zero groups were left out, letters in capitals. Returns 1 for an IPv6 address. */
static int address_words(const char* address, char* out, int size) {
    const char* colon;
    int at = 0;

    if (address[0] == '[') {
        const char* end = strstr(address, "]:");
        const char* c;

        if (end == NULL || size < 1) {
            snprintf(out, (size_t) size, "%s", address);
            return 1;
        }
        for (c = address + 1; c < end && at < size - 1; c++) {
            if (*c == ':') {
                if (c[1] == ':') {
                    at += snprintf(out + at, (size_t) (size - at), "%s..%s", c == address + 1 ? "" : " ",
                                   c + 2 == end ? "" : " ");
                    c++;
                } else {
                    out[at++] = ' ';
                }
            } else {
                out[at++] = (char) toupper((unsigned char) *c);
            }
            if (at >= size) {
                at = size - 1;
            }
        }
        out[at] = '\0';
        snprintf(out + at, (size_t) (size - at), " PORT %s", end + 2);
        return 1;
    }
    colon = strchr(address, ':');
    if (colon == NULL) {
        snprintf(out, (size_t) size, "%s", address);
    } else {
        snprintf(out, (size_t) size, "%.*s PORT %s", (int) (colon - address), address, colon + 1);
    }
    return 0;
}

static void format_address(unsigned ip, unsigned port, char* out, int size) {
    snprintf(out, (size_t) size, "%u.%u.%u.%u PORT %u", (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu,
             ip & 0xFFu, port);
}

/* The notice of ODD 10: what this copy sends once the player hosts, searches or joins, and to whom. Fourteen
 * lines at most, from y 58. */
static const char kNotice[] =
    "A ROOM GETS YOUR PROFILE NAME AND COLOUR AND MACHINE AND FRIEND CODE AND YOUR MESSAGES. ITS HOST CAN BAN "
    "YOUR FRIEND CODE. THE HOST SEES YOUR IP ADDRESS. IF YOU ARE NEXT TO HOST THE WHOLE ROOM GETS IT. PLAYERS "
    "ON YOUR NETWORK CAN SEE IT TOO. A ROOM BY CODE OR AN OPEN ROOM OR A ROOM FOR FRIENDS IS LISTED IN THE "
    "ROOM DIRECTORY WITH THE ADDRESS AND FRIEND CODE OF ITS HOST. YOUR FRIEND CODE IS THE PUBLIC HALF OF YOUR "
    "PROFILE KEY. NOTHING IS SENT BEFORE YOU HOST OR SEARCH OR JOIN.";

#define NOTICE_Y 58
#define NOTICE_LINES 14

static void draw_notice(void) {
    const int rowsY = NOTICE_Y + NOTICE_LINES * LIST_STEP + 6;
    int i;

    put_title("WHAT G-NET SENDS");
    put_paragraph(NOTICE_Y, GDX_NET_ROOM_COLOUR_WHITE, kNotice, NOTICE_LINES);
    for (i = 0; i < sRowCount; i++) {
        put_row(i, rowsY + i * MENU_STEP);
    }
}

static void draw_front(void) {
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
        const int y = FRONT_ROWS_Y + i * MENU_STEP;

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
        put_paragraph(FRONT_TEXT_Y, GDX_NET_ROOM_COLOUR_DIM,
                      "LEFT AND RIGHT PICK A DIGIT. UP AND DOWN CHANGE IT. A KEEPS THE PORT. B DOES NOT.", 3);
    } else if (sHostError[0] != '\0') {
        put_paragraph(FRONT_TEXT_Y, GDX_NET_ROOM_COLOUR_RED, sHostError, 3);
    } else {
        char next[GDX_NET_ROOM_NAME_BYTES];
        char text[GDX_NET_ROOM_STATUS_BYTES];

        /* The host left the room in a race (D28.5): the room is still this copy's to run until the results. */
        if (gdx_lan_room_handing_over(next, (int) sizeof(next))) {
            snprintf(text, sizeof(text),
                     "YOUR ROOM FINISHES THIS RACE WITHOUT YOU. THEN %s HOSTS IT. CLOSING THE GAME NOW ENDS THE "
                     "RACE.",
                     next[0] != '\0' ? next : "THE NEXT PLAYER");
            put_paragraph(FRONT_TEXT_Y, GDX_NET_ROOM_COLOUR_WHITE, text, 3);
        } else if (sCursorId == ROW_HOST && sDirectoryOk) {
            put_paragraph(FRONT_TEXT_Y, GDX_NET_ROOM_COLOUR_DIM, "LEFT AND RIGHT CHANGE THE KIND OF ROOM.", 1);
        }
    }
    if (!sDirectoryOk) {
        put_paragraph(212, GDX_NET_ROOM_COLOUR_DIM, "NO ROOM DIRECTORY IS SET. ROOMS STAY ON THIS NETWORK.", 2);
    }
}

static void draw_code(void) {
    put_title("JOIN BY CODE");
    put_field_row(0, 66, sCode, GDX_NET_ROOM_CODE_CHARS, 1);
    put_row(1, 86);
    put_row(2, 86 + MENU_STEP);
    put_row(3, 86 + 2 * MENU_STEP);
    if (sEdit != 0) {
        put_paragraph(134, GDX_NET_ROOM_COLOUR_DIM,
                      "LEFT AND RIGHT PICK A LETTER. UP AND DOWN CHANGE IT. A KEEPS THE CODE. B DOES NOT.", 3);
    } else {
        put_paragraph(134, GDX_NET_ROOM_COLOUR_DIM,
                      "ASK THE HOST FOR THE CODE OF THEIR ROOM. IT HAS SIX LETTERS AND DIGITS.", 3);
    }
}

/* The line under a directory page's title: what the directory said, or why nothing was asked. */
static void put_directory_status(int ask, const char* none) {
    char text[GDX_NET_ROOM_TEXT_BYTES];

    if (sWaitSeconds > 0) {
        snprintf(text, sizeof(text), "ASK AGAIN IN %d SECONDS.", sWaitSeconds);
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, text);
    } else if (sView.dirStatus == GDX_NET_ROOM_DIR_ASKING) {
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, "ASKING THE ROOM DIRECTORY.");
    } else if (sView.dirAsk != ask) {
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, "PRESS REFRESH TO ASK THE ROOM DIRECTORY.");
    } else if (sView.dirStatus == GDX_NET_ROOM_DIR_FAILED) {
        put_line(60, GDX_NET_ROOM_COLOUR_RED, sView.dirText[0] != '\0' ? sView.dirText : "THE DIRECTORY FAILED.");
    } else if (none != NULL) {
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, none);
    }
}

static void put_listed_rows(int first, int count, int y0, int visible);

static void draw_open(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int first;
    int count;
    int y0;
    int visible;

    put_title("OPEN ROOMS");
    list_geometry(&first, &count, &y0, &visible);
    if (count == 0) {
        put_directory_status(GDX_NET_ROOM_ASK_OPEN, "NO OPEN ROOM RIGHT NOW.");
    } else {
        snprintf(text, sizeof(text), "%d OPEN %s", count, count == 1 ? "ROOM" : "ROOMS");
        put_directory_status(GDX_NET_ROOM_ASK_OPEN, text);
    }
    put_listed_rows(first, count, y0, visible);
    put_row(sRowCount - 2, ROOMS_TOP + ROOMS_VISIBLE * LIST_STEP + 10);
    put_row(sRowCount - 1, ROOMS_TOP + ROOMS_VISIBLE * LIST_STEP + 10 + MENU_STEP);
}

static void draw_quick(void) {
    int i;

    put_title("QUICK RACE");
    if (sAsked) {
        put_line(60, GDX_NET_ROOM_COLOUR_WHITE, "LOOKING FOR AN OPEN ROOM.");
    } else if (sQuickNone) {
        put_line(60, GDX_NET_ROOM_COLOUR_WHITE, "NO OPEN ROOM RIGHT NOW. YOU CAN HOST ONE.");
    } else {
        put_directory_status(GDX_NET_ROOM_ASK_QUICK, NULL);
    }
    for (i = 0; i < sRowCount; i++) {
        put_row(i, 86 + i * MENU_STEP);
    }
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
        const int colour = i == sCursor ? GDX_NET_ROOM_COLOUR_CURSOR : GDX_NET_ROOM_COLOUR_WHITE;

        if (i == sCursor) {
            put_bar(y);
        }
        put_line(y, colour, room->hostName);
        snprintf(text, sizeof(text), "%u OF %u", (unsigned) room->racers, (unsigned) room->racerCap);
        put_text(164, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, text);
        put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, colour,
                 room->phase == GDX_NET_ROOM_PHASE_LOBBY ? "LOBBY" : "RACING");
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
    if (room->racers >= room->racerCap) {
        y = put_paragraph(y, GDX_NET_ROOM_COLOUR_DIM, "EVERY RACE SEAT IS TAKEN.", 1);
    }
    if (room->watchers >= GDX_NET_ROOM_WATCH_SEATS) {
        put_paragraph(y, GDX_NET_ROOM_COLOUR_DIM, "EVERY WATCH SEAT IS TAKEN.", 1);
    }
}

static void draw_joining(void) {
    char text[GDX_NET_ROOM_ADDRESS_BYTES + 24]; /* "JOINING", the groups and the port on three lines */
    char address[GDX_NET_ROOM_ADDRESS_BYTES + 8];

    put_title("JOINING");
    if (sView.joinCode[0] != '\0') {
        snprintf(text, sizeof(text), "JOINING ROOM %s", sView.joinCode);
    } else {
        const char* port;

        if (address_words(sView.hostText, address, (int) sizeof(address)) &&
            (port = strstr(address, " PORT ")) != NULL) {
            /* Eight groups and the port pass 40 columns: one line each. */
            snprintf(text, sizeof(text), "JOINING%c%.*s%c%s", 10, (int) (port - address), address, 10, port + 1);
        } else {
            snprintf(text, sizeof(text), "JOINING %s", address);
        }
    }
    put_paragraph(70, GDX_NET_ROOM_COLOUR_WHITE, text, 3);
    put_row(0, 100);
}

static void draw_closed(void) {
    put_title("G-NET");
    put_paragraph(70, GDX_NET_ROOM_COLOUR_WHITE, sView.status[0] != '\0' ? sView.status : "THE ROOM CLOSED.", 4);
    put_row(0, 122);
}

static void draw_player(void) {
    const GdxNetRoomPlayer* player = player_by_id(sPlayerId);
    int y;
    int i;

    put_title(player_name(sPlayerId));
    for (i = 0; i < sRowCount; i++) {
        put_row(i, 80 + i * MENU_STEP);
    }
    /* No apostrophe: the small font draws one far from its letter. */
    y = put_paragraph(80 + sRowCount * MENU_STEP + 12, GDX_NET_ROOM_COLOUR_DIM,
                      (player != NULL && player->muted) ? "MESSAGES FROM THIS PLAYER ARE HIDDEN ON YOUR SCREEN."
                                                        : "MUTING HIDES MESSAGES FROM THIS PLAYER ON YOUR SCREEN ONLY.",
                      2);
    if (sView.mode == GDX_NET_ROOM_MODE_HOSTING) {
        put_paragraph(y + 6, GDX_NET_ROOM_COLOUR_DIM,
                      "A BAN REMOVES THE PLAYER AND REFUSES THEIR FRIEND CODE AND THEIR ADDRESS FOR AS LONG AS THIS "
                      "ROOM LASTS.",
                      4);
    }
}

/* A friend code, "ABCD EFGH ...", on two lines of seven groups from y. */
static void put_code_lines(int y, int colour, const char* code) {
    const size_t lineChars = CODE_GROUPS_PER_LINE * (CODE_GROUP + 1) - 1;
    char line[GDX_NET_ROOM_FRIEND_CODE_BYTES];

    snprintf(line, sizeof(line), "%.*s", (int) lineChars, code);
    put_line(y, colour, line);
    if (strlen(code) > lineChars + 1) {
        put_line(y + LIST_STEP, colour, code + lineChars + 1);
    }
}

/* The friend code field: its places in groups of four, on two lines from y. The place being changed takes the
 * cursor's colour. */
static void put_friend_field(int y, int index) {
    const int editing = index == sCursor && sEdit != 0;
    const int colour = editing ? GDX_NET_ROOM_COLOUR_WHITE : row_colour(index);
    const int perLine = CODE_GROUPS_PER_LINE * CODE_GROUP;
    char one[2] = { '0', '\0' };
    int i;

    for (i = 0; i < GDX_NET_ROOM_FRIEND_CODE_CHARS; i++) {
        const int column = i % perLine;
        const int x = MARGIN_LEFT + (column + column / CODE_GROUP) * DIGIT_STEP;

        one[0] = kCodeAlphabet[sFriendCode[i]];
        put_text(x, y + (i / perLine) * LIST_STEP, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT,
                 (editing && sEdit == i + 1) ? GDX_NET_ROOM_COLOUR_CURSOR : colour, one);
    }
}

static void put_note(void) {
    if (sNote[0] != '\0') {
        put_paragraph(NOTE_Y, sNoteColour, sNote, 2);
    }
}

/* The rows of a list of rooms from the directory, as the Open rooms page has them. */
static void put_listed_rows(int first, int count, int y0, int visible) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int i;

    for (i = sTop; i < count && i < sTop + visible; i++) {
        const GdxNetRoomListed* room = &sView.listed[i];
        const int y = y0 + (i - sTop) * LIST_STEP;
        const int selected = first + i == sCursor;
        const int colour = selected ? GDX_NET_ROOM_COLOUR_CURSOR : GDX_NET_ROOM_COLOUR_WHITE;

        if (selected) {
            put_bar(y);
        }
        put_line(y, colour, room->code);
        snprintf(text, sizeof(text), "%u %s", (unsigned) room->humans, room->humans == 1 ? "PLAYER" : "PLAYERS");
        put_text(100, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, colour, text);
        if (room->freeSeats == 0) {
            snprintf(text, sizeof(text), "FULL - WATCH");
        } else {
            snprintf(text, sizeof(text), "%u %s FREE", (unsigned) room->freeSeats,
                     room->freeSeats == 1 ? "SEAT" : "SEATS");
        }
        put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, colour, text);
    }
    put_scroll(y0, count, visible);
}

static void draw_friends(void) {
    char text[GDX_NET_ROOM_TEXT_BYTES];
    int first;
    int count;
    int y0;
    int visible;
    int i;

    put_title("FRIENDS");
    put_line(58, GDX_NET_ROOM_COLOUR_DIM, "YOUR FRIEND CODE");
    if (sMyCode[0] != '\0') {
        put_code_lines(68, GDX_NET_ROOM_COLOUR_WHITE, sMyCode);
    } else {
        put_line(68, GDX_NET_ROOM_COLOUR_RED, "YOUR PROFILE HAS NO FRIEND CODE.");
    }
    for (i = 0; i < 4 && i < sRowCount; i++) {
        put_row(i, FRIENDS_ROWS_Y + i * MENU_STEP);
    }
    list_geometry(&first, &count, &y0, &visible);
    if (count == 0) {
        put_paragraph(FRIENDS_LIST_Y - LIST_STEP, GDX_NET_ROOM_COLOUR_DIM,
                      "NO FRIEND YET. ADD ONE BY CODE OR FROM YOUR LAST ROOM.", 2);
    } else {
        snprintf(text, sizeof(text), "%d %s", count, count == 1 ? "FRIEND" : "FRIENDS");
        put_line(FRIENDS_LIST_Y - LIST_STEP, GDX_NET_ROOM_COLOUR_DIM, text);
    }
    for (i = sTop; i < count && i < sTop + visible; i++) {
        const int y = y0 + (i - sTop) * LIST_STEP;

        if (first + i == sCursor) {
            put_bar(y);
        }
        gdx_lan_room_friend_name(i, text, (int) sizeof(text));
        put_line(y, row_colour(first + i), text);
    }
    put_scroll(y0, count, visible);
    put_note();
}

static void draw_friend_add(void) {
    put_title("ADD A FRIEND");
    put_row(0, 66);
    put_friend_field(FRIEND_FIELD_Y, 0);
    put_row(1, 106);
    put_row(2, 106 + MENU_STEP);
    put_row(3, 106 + 2 * MENU_STEP);
    if (sEdit != 0) {
        put_paragraph(150, GDX_NET_ROOM_COLOUR_DIM,
                      "LEFT AND RIGHT PICK A LETTER. UP AND DOWN CHANGE IT. A KEEPS THE CODE. B DOES NOT.", 3);
    } else {
        put_paragraph(150, GDX_NET_ROOM_COLOUR_DIM,
                      "PASTE THE CODE YOUR FRIEND COPIED FROM THEIR FRIENDS PAGE OR SET IT ONE LETTER AT A TIME.", 3);
    }
    put_note();
}

/* The line over a friend's rooms: what the directory said, or why nothing was asked. */
static void put_friend_status(int count) {
    char text[GDX_NET_ROOM_TEXT_BYTES];

    if (!sDirectoryOk) {
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_DIM, "FINDING ROOMS NEEDS A ROOM DIRECTORY.");
    } else if (sWaitSeconds > 0) {
        snprintf(text, sizeof(text), "ASK AGAIN IN %d SECONDS.", sWaitSeconds);
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_DIM, text);
    } else if (sFriendRoomsOf != sFriendIndex || (sView.dirAsk != GDX_NET_ROOM_ASK_FRIEND && !sAsked)) {
        return;
    } else if (sAsked || sView.dirStatus == GDX_NET_ROOM_DIR_ASKING) {
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_DIM, "ASKING THE ROOM DIRECTORY.");
    } else if (sView.dirStatus == GDX_NET_ROOM_DIR_FAILED) {
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_RED,
                 sView.dirText[0] != '\0' ? sView.dirText : "THE DIRECTORY FAILED.");
    } else if (count == 0) {
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_DIM, "NO ROOM OF THEIRS RIGHT NOW.");
    } else {
        snprintf(text, sizeof(text), "%d %s OF THEIRS", count, count == 1 ? "ROOM" : "ROOMS");
        put_line(FRIEND_STATUS_Y, GDX_NET_ROOM_COLOUR_DIM, text);
    }
}

static void draw_friend(void) {
    char name[GDX_NET_ROOM_NAME_BYTES];
    char code[GDX_NET_ROOM_FRIEND_CODE_BYTES];
    int first;
    int count;
    int y0;
    int visible;
    int i;

    gdx_lan_room_friend_name(sFriendIndex, name, (int) sizeof(name));
    gdx_lan_room_friend_code(sFriendIndex, code, (int) sizeof(code));
    put_title(name);
    put_code_lines(60, GDX_NET_ROOM_COLOUR_DIM, code);
    for (i = 0; i < 3 && i < sRowCount; i++) {
        put_row(i, FRIEND_ROWS_Y + i * MENU_STEP);
    }
    list_geometry(&first, &count, &y0, &visible);
    put_friend_status(count);
    put_listed_rows(first, count, y0, visible);
    put_note();
}

static void draw_met(void) {
    char name[GDX_NET_ROOM_NAME_BYTES];
    int first;
    int count;
    int y0;
    int visible;
    int i;

    put_title("LAST ROOM");
    list_geometry(&first, &count, &y0, &visible);
    if (count == 0) {
        put_paragraph(60, GDX_NET_ROOM_COLOUR_DIM,
                      "NOBODY YET. THE PLAYERS OF THE NEXT ROOM YOU ARE IN SHOW HERE.", 2);
    } else {
        put_line(60, GDX_NET_ROOM_COLOUR_DIM, "PICK A PLAYER TO ADD AS A FRIEND.");
    }
    for (i = sTop; i < count && i < sTop + visible; i++) {
        const int y = y0 + (i - sTop) * LIST_STEP;

        if (first + i == sCursor) {
            put_bar(y);
        }
        gdx_lan_room_met_name(i, name, (int) sizeof(name));
        put_line(y, row_colour(first + i), name);
        if (!sRows[first + i].enabled) {
            put_text(MARGIN_RIGHT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_RIGHT, GDX_NET_ROOM_COLOUR_DIM,
                     "ON YOUR LIST");
        }
    }
    put_scroll(y0, count, visible);
    put_row(sRowCount - 1, MET_LIST_Y + MET_VISIBLE * LIST_STEP + 8);
    put_note();
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

/* The room page's first line: where the room is, and for a directory room its code or how its listing goes.
 * On the vote result, a member whose own check of the draw failed sees that instead (decision 56).
 * Returns the line's colour. */
static int room_header(char* out, int size) {
    static const char* const kTypeWords[GDX_NET_ROOM_TYPES] = { "", "CODE", "OPEN", "FRIENDS" };
    const int type = sView.roomType >= 0 && sView.roomType < GDX_NET_ROOM_TYPES ? sView.roomType : 0;

    if (sView.mode != GDX_NET_ROOM_MODE_HOSTING) {
        char address[GDX_NET_ROOM_ADDRESS_BYTES + 8];

        if (sView.phase == GDX_NET_ROOM_PHASE_LOTTERY && !sView.lotteryChecked) {
            snprintf(out, (size_t) size, "THE HOST'S DRAW FAILED THE CHECK.");
            return GDX_NET_ROOM_COLOUR_RED;
        }
        if (sView.code[0] != '\0') {
            /* The type tells a member what kind of room the code it passes on opens. */
            if (type == GDX_NET_ROOM_TYPE_OPEN || type == GDX_NET_ROOM_TYPE_FRIENDS) {
                snprintf(out, (size_t) size, "%s ROOM CODE %s", kTypeWords[type], sView.code);
            } else {
                snprintf(out, (size_t) size, "ROOM CODE %s", sView.code);
            }
        } else {
            if (address_words(sView.hostText, address, (int) sizeof(address))) {
                /* One line has no room for eight groups: the JOINING page showed the whole address. */
                const char* port = strstr(address, " PORT ");
                snprintf(out, (size_t) size, "IPV6 ROOM%s", port != NULL ? port : "");
            } else {
                snprintf(out, (size_t) size, "ROOM AT %s", address);
            }
        }
        return GDX_NET_ROOM_COLOUR_DIM;
    }
    if (type == GDX_NET_ROOM_TYPE_LAN) {
        snprintf(out, (size_t) size, "YOUR ROOM ON PORT %u", sView.port);
        return GDX_NET_ROOM_COLOUR_DIM;
    }
    switch (sView.listing) {
        case GDX_NET_ROOM_LISTING_LISTED:
            snprintf(out, (size_t) size, "%s ROOM %s%s", kTypeWords[type], sView.code,
                     (type == GDX_NET_ROOM_TYPE_OPEN && !sView.reachable) ? " - NOT REACHABLE" : "");
            return GDX_NET_ROOM_COLOUR_DIM;
        case GDX_NET_ROOM_LISTING_FAILED:
            snprintf(out, (size_t) size, "%s", sView.listingText[0] != '\0' ? sView.listingText : "NOT LISTED.");
            return GDX_NET_ROOM_COLOUR_RED;
        default:
            snprintf(out, (size_t) size, "%s",
                     sView.listingText[0] != '\0' ? sView.listingText : "LISTING YOUR ROOM.");
            return GDX_NET_ROOM_COLOUR_DIM;
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
        put_line(58, room_header(text, (int) sizeof(text)), text);
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
            /* A room on the internet runs on the host's upload: the figure each seat costs is measured (L2).
               The host sends to every seat but its own and to the two watch seats. */
            if (sView.roomType != GDX_NET_ROOM_TYPE_LAN) {
                const unsigned kbit = (unsigned) (sView.racerCap + 1) * UPLOAD_KBIT_PER_SEAT;
                const unsigned tenths = (kbit + 50u) / 100u;

                snprintf(text, sizeof(text), "NEEDS %u.%u MBIT UPLOAD", tenths / 10u, tenths % 10u);
                put_text(124, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, GDX_NET_ROOM_COLOUR_DIM, text);
            }
        } else if (sRows[i].id == ROW_CUSTOM) {
            put_text(FIELD_LEFT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, row_colour(i),
                     sView.customTracks ? "ON" : "OFF");
        } else if (sRows[i].id == ROW_CPU_FILL) {
            put_text(FIELD_LEFT, y, GDX_NET_ROOM_FONT_SMALL, GDX_NET_ROOM_ALIGN_LEFT, row_colour(i),
                     sView.cpuFill ? "ON" : "OFF");
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
        case GDX_NET_ROOM_PAGE_CODE:
            draw_code();
            break;
        case GDX_NET_ROOM_PAGE_OPEN:
            draw_open();
            break;
        case GDX_NET_ROOM_PAGE_QUICK:
            draw_quick();
            break;
        case GDX_NET_ROOM_PAGE_NOTICE:
            draw_notice();
            break;
        case GDX_NET_ROOM_PAGE_FRIENDS:
            draw_friends();
            break;
        case GDX_NET_ROOM_PAGE_FRIEND_ADD:
            draw_friend_add();
            break;
        case GDX_NET_ROOM_PAGE_FRIEND:
            draw_friend();
            break;
        case GDX_NET_ROOM_PAGE_MET:
            draw_met();
            break;
        default:
            draw_closed();
            break;
    }
    *count = sItemCount;
    return sItems;
}
