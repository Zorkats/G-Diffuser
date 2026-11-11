/* port/tests/gdx_net_room_screen_tests.c -- unit tests for the G-NET room screen (gdx_net_room_screen.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_room_screen.c with the two modules it calls, gdx_net_text.c and
 * gdx_net_track_choice.c. The room session is the functions of gdx_net_room_view.h, written here as a fake
 * that changes its mode the way the real session does, and the port menu is one function more, so no socket,
 * thread or game code is involved. Expected values are written out by hand from the headers.
 *
 * The screen keeps the address the player typed between visits, so the address tests run in order.
 */

#include "../gdx_net_room_screen.h"
#include "../gdx_net_track_choice.h"

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

/* ---- the session, as the screen sees it ---- */

static GdxNetRoomView sFake;
static int sFakeProfile;
static unsigned sFakePort;
static int sHostAnswer; /* what gdx_lan_room_host returns */
static int sJoinAnswer;
static int sHostCalls;
static int sBrowseCalls;
static int sJoinCalls;
static unsigned sJoinIp;
static unsigned sJoinPort;
static int sJoinWatch;
static int sStopCalls;
static int sDismissCalls;
static int sReadyCalls;
static int sReadyValue;
static int sRaceNextCalls;
static int sRaceNextValue;
static int sStartCalls;
static int sCapCalls;
static int sCapValue;
static int sKickCalls;
static int sKickId;
static int sSetPortCalls;
static int sVoteCalls;
static int sPickCalls;
static int sMenuCloseCalls;
static int sSkipCalls;
static int sEndSeriesCalls;
static int sMessageCalls;
static int sMessageIndex;
static int sMuteCalls;
static int sMuteId;
static int sMuteValue;

void gdx_lan_room_view(GdxNetRoomView* out) {
    *out = sFake;
}

int gdx_lan_room_profile_ok(void) {
    return sFakeProfile;
}

unsigned gdx_lan_room_port(void) {
    return sFakePort;
}

void gdx_lan_room_set_port(unsigned port) {
    sSetPortCalls++;
    sFakePort = port;
}

int gdx_lan_room_host(char* error, int size) {
    sHostCalls++;
    if (!sHostAnswer) {
        snprintf(error, (size_t) size, "Port 24302 is in use by another program.");
        return 0;
    }
    sFake.mode = GDX_NET_ROOM_MODE_HOSTING;
    return 1;
}

void gdx_lan_room_browse(void) {
    sBrowseCalls++;
    sFake.mode = GDX_NET_ROOM_MODE_BROWSING;
}

int gdx_lan_room_join(unsigned ip, unsigned port, int watch) {
    sJoinCalls++;
    sJoinIp = ip;
    sJoinPort = port;
    sJoinWatch = watch;
    if (sJoinAnswer) {
        sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    }
    return sJoinAnswer;
}

void gdx_lan_room_stop(void) {
    sStopCalls++;
    sFake.mode = GDX_NET_ROOM_MODE_IDLE;
}

void gdx_lan_room_dismiss(void) {
    sDismissCalls++;
    sFake.mode = GDX_NET_ROOM_MODE_IDLE;
}

void gdx_lan_room_ready(int ready) {
    sReadyCalls++;
    sReadyValue = ready;
}

void gdx_lan_room_race_next(int raceNext) {
    sRaceNextCalls++;
    sRaceNextValue = raceNext;
}

void gdx_lan_room_start(void) {
    sStartCalls++;
}

void gdx_lan_room_racer_cap(int cap) {
    sCapCalls++;
    sCapValue = cap;
}

void gdx_lan_room_kick(int id) {
    sKickCalls++;
    sKickId = id;
}

void gdx_lan_room_skip_lottery(void) {
    sSkipCalls++;
}

void gdx_lan_room_end_series(void) {
    sEndSeriesCalls++;
}

void gdx_lan_room_message(int index) {
    sMessageCalls++;
    sMessageIndex = index;
}

/* The eight presets of net/gdx_net_lobby.cpp. */
void gdx_lan_room_message_text(int index, char* out, int size) {
    static const char* const kText[GDX_NET_ROOM_MESSAGES] = { "Hello!",  "Good luck!", "Good race!", "Nice one!",
                                                              "Again?",  "Thanks!",    "Sorry!",     "I have to go." };

    snprintf(out, (size_t) size, "%s", (index >= 0 && index < GDX_NET_ROOM_MESSAGES) ? kText[index] : "");
}

/* As the session does it: the mark shows in the next view. */
void gdx_lan_room_set_muted(int id, int muted) {
    int i;

    sMuteCalls++;
    sMuteId = id;
    sMuteValue = muted;
    for (i = 0; i < sFake.playerCount; i++) {
        if (sFake.players[i].id == id) {
            sFake.players[i].muted = (unsigned char) (muted != 0);
        }
    }
}

void gdx_lan_room_vote(int track) {
    (void) track;
    sVoteCalls++;
}

void gdx_lan_room_pick(int track) {
    (void) track;
    sPickCalls++;
}

/* The port menu steps aside when the course select opens for the room (gdx_net_track_choice.h). */
void gdx_menu_request_close(void) {
    sMenuCloseCalls++;
}

void gdx_lan_room_track_name(int track, char* out, int size) {
    if (track == GDX_NET_ROOM_TRACK_RANDOM) {
        snprintf(out, (size_t) size, "Random");
    } else if (track < 0 || track >= 24) {
        snprintf(out, (size_t) size, "No track");
    } else {
        snprintf(out, (size_t) size, "Track %d", track);
    }
}

/* No session, a profile, the default port, and every counter at zero. */
static void reset(void) {
    memset(&sFake, 0, sizeof(sFake));
    sFake.localId = GDX_NET_ROOM_NOBODY;
    sFake.racerCap = 8;
    sFake.track = GDX_NET_ROOM_TRACK_NONE;
    sFake.lastTrack = GDX_NET_ROOM_TRACK_NONE;
    sFake.picker = GDX_NET_ROOM_NOBODY;
    sFake.lotteryWinner = -1;
    sFakeProfile = 1;
    sFakePort = 24302u;
    sHostAnswer = 1;
    sJoinAnswer = 1;
    sHostCalls = 0;
    sBrowseCalls = 0;
    sJoinCalls = 0;
    sJoinIp = 0;
    sJoinPort = 0;
    sJoinWatch = -1;
    sStopCalls = 0;
    sDismissCalls = 0;
    sReadyCalls = 0;
    sReadyValue = -1;
    sRaceNextCalls = 0;
    sRaceNextValue = -1;
    sStartCalls = 0;
    sCapCalls = 0;
    sCapValue = -1;
    sKickCalls = 0;
    sKickId = -1;
    sSetPortCalls = 0;
    sVoteCalls = 0;
    sPickCalls = 0;
    sMenuCloseCalls = 0;
    sSkipCalls = 0;
    sEndSeriesCalls = 0;
    sMessageCalls = 0;
    sMessageIndex = -1;
    sMuteCalls = 0;
    sMuteId = -1;
    sMuteValue = -1;
    /* The course select is not the room's, and nothing was declined or asked for. */
    gdx_net_track_choice_close();
    gdx_net_track_choice_poll_open();
}

/* A room of count players with ids 0 to count - 1 named "P00" on; id 0 hosts and the last watchers of them
 * hold watch seats. */
static void fake_room(int mode, int phase, int localId, int count, int watchers) {
    int i;

    sFake.mode = mode;
    sFake.phase = phase;
    sFake.localId = localId;
    sFake.port = 24302u;
    snprintf(sFake.hostText, sizeof(sFake.hostText), "192.168.1.20:24302");
    sFake.playerCount = count;
    for (i = 0; i < count; i++) {
        GdxNetRoomPlayer* p = &sFake.players[i];

        memset(p, 0, sizeof(*p));
        p->id = (unsigned char) i;
        p->host = i == 0;
        p->watcher = i >= count - watchers;
        p->machine = 0xFF;
        p->colour = 0x102030u + (unsigned) i;
        p->pingMs = 10u + (unsigned) i;
        snprintf(p->name, sizeof(p->name), "P%02d", i);
    }
}

static void remove_player(int id) {
    int i;
    int kept = 0;

    for (i = 0; i < sFake.playerCount; i++) {
        if (sFake.players[i].id != id) {
            sFake.players[kept++] = sFake.players[i];
        }
    }
    sFake.playerCount = kept;
}

/* A search that found count rooms "Den00" on, at 192.168.1.0 on, each with 3 of 8 racers in its lobby. */
static void fake_search(int count) {
    int i;

    sFake.mode = GDX_NET_ROOM_MODE_BROWSING;
    sFake.foundCount = count;
    for (i = 0; i < count; i++) {
        GdxNetRoomFound* room = &sFake.found[i];

        memset(room, 0, sizeof(*room));
        room->ip = 0xC0A80100u + (unsigned) i;
        room->port = 24302u;
        room->racers = 3;
        room->racerCap = 8;
        room->sameBuild = 1;
        snprintf(room->hostName, sizeof(room->hostName), "Den%02d", i);
    }
}

/* A line of the room's log: a message of player from, or a notice when from is GDX_NET_ROOM_NOBODY. */
static void fake_line(int from, const char* text) {
    GdxNetRoomLine* line = &sFake.lines[sFake.lineCount++];

    memset(line, 0, sizeof(*line));
    line->from = (unsigned char) from;
    if (from != GDX_NET_ROOM_NOBODY) {
        line->colour = 0x102030u + (unsigned) from;
        snprintf(line->name, sizeof(line->name), "P%02d", from);
    }
    snprintf(line->text, sizeof(line->text), "%s", text);
}

/* The table of the race being run: count lines, ids in order of place. */
static void fake_table(int final, int count, const int* ids, const int* kinds, const unsigned* timesMs,
                       const int* points) {
    int i;

    sFake.haveResults = 1;
    sFake.raceIndex = 4u;
    sFake.resultsRace = 4u;
    sFake.resultsFinal = final;
    sFake.resultCount = count;
    for (i = 0; i < count; i++) {
        sFake.results[i].id = (unsigned char) ids[i];
        sFake.results[i].kind = (unsigned char) kinds[i];
        sFake.results[i].timeMs = timesMs[i];
        sFake.results[i].points = (unsigned char) points[i];
    }
}

/* ---- the screen ---- */

static int press(unsigned input) {
    return gdx_net_room_screen_update(input);
}

static void press_times(unsigned input, int times) {
    while (times-- > 0) {
        press(input);
    }
}

static const GdxNetRoomDrawItem* find_text(const char* text) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
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

/* How many texts start with prefix. */
static int count_prefix(const char* prefix) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        found += items[i].kind == GDX_NET_ROOM_DRAW_TEXT && strncmp(items[i].text, prefix, strlen(prefix)) == 0;
    }
    return found;
}

static int count_colour(int colour) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        found += items[i].kind == GDX_NET_ROOM_DRAW_TEXT && items[i].colour == colour;
    }
    return found;
}

static const GdxNetRoomDrawItem* find_text_at(const char* text, int y) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int i;

    for (i = 0; i < count; i++) {
        if (items[i].kind == GDX_NET_ROOM_DRAW_TEXT && items[i].y == y && strcmp(items[i].text, text) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

/* How many 6 x 6 swatches have their top on row y. */
static int count_swatches_at(int y) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        found += items[i].kind == GDX_NET_ROOM_DRAW_RECT && items[i].w == 6 && items[i].h == 6 && items[i].y == y;
    }
    return found;
}

/* How many rectangles are w wide. */
static int count_rects(int w) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        found += items[i].kind == GDX_NET_ROOM_DRAW_RECT && items[i].w == w;
    }
    return found;
}

static int draw_count(void) {
    int count;

    gdx_net_room_screen_draw(&count);
    return count;
}

/* The digits and periods of the number field in the row labelled label, as one string. */
static void field_text(const char* label, char* out, int size) {
    const GdxNetRoomDrawItem* row = find_text(label);
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int length = 0;
    int i;

    out[0] = '\0';
    if (row == NULL) {
        return;
    }
    for (i = 0; i < count && length < size - 1; i++) {
        if (items[i].kind == GDX_NET_ROOM_DRAW_TEXT && items[i].y == row->y && items[i].x > row->x &&
            strlen(items[i].text) == 1) {
            out[length++] = items[i].text[0];
        }
    }
    out[length] = '\0';
}

static int field_is(const char* label, const char* expected) {
    char text[32];

    field_text(label, text, (int) sizeof(text));
    return strcmp(text, expected) == 0;
}

/* With the cursor on the row labelled label: opens its number field, sets it to target ("192.168.000.001" or
 * "24302") a digit at a time, and keeps it. */
static void set_field(const char* label, const char* target) {
    char now[32];
    int i;

    press(GDX_NET_ROOM_IN_CONFIRM);
    for (i = 0; target[i] != '\0'; i++) {
        if (target[i] == '.') {
            continue;
        }
        field_text(label, now, (int) sizeof(now));
        press_times(GDX_NET_ROOM_IN_UP, (target[i] - now[i] + 10) % 10);
        press(GDX_NET_ROOM_IN_RIGHT);
    }
    press(GDX_NET_ROOM_IN_CONFIRM);
}

/* ---- the pieces ---- */

static void test_filter(void) {
    char out[16];

    /* Small letters become capitals: the small font draws katakana for them. */
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "Ana_99?", out, (int) sizeof(out));
    CHECK(strcmp(out, "ANA 99 ") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_TITLE, "Dr.Stewart", out, (int) sizeof(out));
    CHECK(strcmp(out, "DR.STEWART") == 0);
    /* Each font keeps the signs it has. The small one has no colon and no comma, the title one no '"' and
     * no '+', and neither has '!' or a bracket. */
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "a'b\"c&d+e-f.g", out, (int) sizeof(out));
    CHECK(strcmp(out, "A'B\"C&D+E-F.G") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "1:2,3!4(5)6", out, (int) sizeof(out));
    CHECK(strcmp(out, "1 2 3 4 5 6") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_TITLE, "a'b&c-d:e.f,g", out, (int) sizeof(out));
    CHECK(strcmp(out, "A'B&C-D:E.F,G") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_TITLE, "1\"2+3!4(5)6", out, (int) sizeof(out));
    CHECK(strcmp(out, "1 2 3 4 5 6") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "/\\%$*<>_#@[]", out, (int) sizeof(out));
    CHECK(strcmp(out, "            ") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "\xC3\xA9", out, (int) sizeof(out));
    CHECK(strcmp(out, "  ") == 0);
    /* Cut at 15 characters and a NUL. */
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "abcdefghijklmnopqrstuvwxyz", out, (int) sizeof(out));
    CHECK(strcmp(out, "ABCDEFGHIJKLMNO") == 0);
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, NULL, out, (int) sizeof(out));
    CHECK(out[0] == '\0');
    memset(out, 'x', sizeof(out));
    gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, "abc", out, 1);
    CHECK(out[0] == '\0' && out[1] == 'x');
}

static void test_track_name(void) {
    char out[16];

    /* The game's names are in one case; each word gets a capital. */
    gdx_net_text_track_name("devil's forest", out, (int) sizeof(out));
    CHECK(strcmp(out, "Devil's Forest") == 0);
    gdx_net_text_track_name("MUTE CITY 2", out, (int) sizeof(out));
    CHECK(strcmp(out, "Mute City 2") == 0);
    /* The two Greek letters are spelled out, and what the filter makes of the name has no gap. */
    gdx_net_text_track_name("sector \243\301", out, (int) sizeof(out));
    CHECK(strcmp(out, "Sector Alpha") == 0);
    gdx_net_text_track_name("sector \243\302", out, (int) sizeof(out));
    CHECK(strcmp(out, "Sector Beta") == 0);
    {
        char drawn[16];

        gdx_net_text_filter(GDX_NET_ROOM_FONT_SMALL, out, drawn, (int) sizeof(drawn));
        CHECK(strcmp(drawn, "SECTOR BETA") == 0);
    }
    /* Any other pair of the game's encoding is left as it is. */
    gdx_net_text_track_name("a\243\303", out, (int) sizeof(out));
    CHECK(strcmp(out, "A\243\303") == 0);
    /* Cut at the buffer, in the middle of a spelled letter too, and never past it. */
    gdx_net_text_track_name("sector sector \243\301", out, (int) sizeof(out));
    CHECK(strcmp(out, "Sector Sector A") == 0);
    gdx_net_text_track_name("abcdefghijklmnopqrstuvwxyz", out, (int) sizeof(out));
    CHECK(strcmp(out, "Abcdefghijklmno") == 0);
    gdx_net_text_track_name(NULL, out, (int) sizeof(out));
    CHECK(out[0] == '\0');
    memset(out, 'x', sizeof(out));
    gdx_net_text_track_name("abc", out, 1);
    CHECK(out[0] == '\0' && out[1] == 'x');
}

static void test_wrap(void) {
    char lines[4][12];

    CHECK(gdx_net_text_wrap("one two three four", 9, lines[0], 12, 4) == 3);
    CHECK(strcmp(lines[0], "one two") == 0);
    CHECK(strcmp(lines[1], "three") == 0);
    CHECK(strcmp(lines[2], "four") == 0);
    /* A line that ends exactly at a space keeps its last word. */
    CHECK(gdx_net_text_wrap("abcd efgh ij", 9, lines[0], 12, 4) == 2);
    CHECK(strcmp(lines[0], "abcd efgh") == 0);
    CHECK(strcmp(lines[1], "ij") == 0);
    /* A word longer than a line is cut. */
    CHECK(gdx_net_text_wrap("abcdefghijklm no", 5, lines[0], 12, 4) == 4);
    CHECK(strcmp(lines[0], "abcde") == 0);
    CHECK(strcmp(lines[1], "fghij") == 0);
    CHECK(strcmp(lines[2], "klm") == 0);
    CHECK(strcmp(lines[3], "no") == 0);
    /* What does not fit the lines is dropped. */
    CHECK(gdx_net_text_wrap("a b c d e f", 1, lines[0], 12, 2) == 2);
    CHECK(strcmp(lines[0], "a") == 0);
    CHECK(strcmp(lines[1], "b") == 0);
    /* A line never holds more than its bytes. */
    CHECK(gdx_net_text_wrap("abcdefghijklmnopqrstuvwxyz", 40, lines[0], 12, 4) == 3);
    CHECK(strcmp(lines[0], "abcdefghijk") == 0);
    CHECK(strcmp(lines[2], "wxyz") == 0);
    /* A line feed ends a line where it stands. */
    CHECK(gdx_net_text_wrap("ab cd.\nef", 9, lines[0], 12, 4) == 2);
    CHECK(strcmp(lines[0], "ab cd.") == 0);
    CHECK(strcmp(lines[1], "ef") == 0);
    CHECK(gdx_net_text_wrap("  ab  \n  cd  ", 9, lines[0], 12, 4) == 2);
    CHECK(strcmp(lines[0], "ab") == 0);
    CHECK(strcmp(lines[1], "cd") == 0);
    CHECK(gdx_net_text_wrap("", 9, lines[0], 12, 4) == 0);
    CHECK(gdx_net_text_wrap(NULL, 9, lines[0], 12, 4) == 0);
    CHECK(gdx_net_text_wrap("ab", 0, lines[0], 12, 4) == 0);
}

static void test_timer(void) {
    char out[16];

    gdx_net_text_format_timer(42000u, out, (int) sizeof(out));
    CHECK(strcmp(out, "0:42") == 0);
    gdx_net_text_format_timer(41001u, out, (int) sizeof(out));
    CHECK(strcmp(out, "0:42") == 0);
    gdx_net_text_format_timer(60000u, out, (int) sizeof(out));
    CHECK(strcmp(out, "1:00") == 0);
    gdx_net_text_format_timer(61000u, out, (int) sizeof(out));
    CHECK(strcmp(out, "1:01") == 0);
    gdx_net_text_format_timer(1u, out, (int) sizeof(out));
    CHECK(strcmp(out, "0:01") == 0);
    gdx_net_text_format_timer(0u, out, (int) sizeof(out));
    CHECK(strcmp(out, "0:00") == 0);
}

/* ---- the pages before a room ---- */

static void test_wanted(void) {
    reset();
    CHECK(gdx_net_room_screen_wanted() == 0);
    sFake.mode = GDX_NET_ROOM_MODE_BROWSING;
    CHECK(gdx_net_room_screen_wanted() == 1);
    sFake.mode = GDX_NET_ROOM_MODE_HOSTING;
    CHECK(gdx_net_room_screen_wanted() == 1);
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    CHECK(gdx_net_room_screen_wanted() == 1);
    sFake.mode = GDX_NET_ROOM_MODE_JOINED;
    CHECK(gdx_net_room_screen_wanted() == 1);
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    CHECK(gdx_net_room_screen_wanted() == 1);
}

/* A racer who left the room from the race's quit box: the screen is wanted with no session, once, and it
   opens on the front page. */
static void test_wanted_for_the_front_page(void) {
    reset();
    CHECK(gdx_net_room_screen_wanted() == 0);
    gdx_net_room_screen_want_front();
    CHECK(gdx_net_room_screen_wanted() == 1);
    CHECK(gdx_net_room_screen_wanted() == 1); /* asking does not use it up: the menu asks more than once */
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(gdx_net_room_screen_wanted() == 0);
    /* A session keeps it wanted as before, and opening the screen in one still forgets the request. */
    gdx_net_room_screen_want_front();
    sFake.mode = GDX_NET_ROOM_MODE_BROWSING;
    gdx_net_room_screen_open();
    sFake.mode = GDX_NET_ROOM_MODE_IDLE;
    CHECK(gdx_net_room_screen_wanted() == 0);
}

static void test_front(void) {
    reset();
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    CHECK(has_text("G-NET"));
    CHECK(has_text("HOST A ROOM"));
    CHECK(has_text("FIND ROOMS"));
    CHECK(has_text("JOIN BY ADDRESS"));
    CHECK(has_text("PORT"));
    CHECK(has_text("BACK"));
    CHECK(field_is("PORT", "24302"));
    CHECK(find_text("HOST A ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("FIND ROOMS")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(count_rects(288) == 1); /* the cursor's bar */
    /* What the page shares is said before anything is sent, in at most six lines, down to its last word. */
    CHECK(has_text("HOSTING OR SEARCHING OR JOINING SHARES"));
    CHECK(has_text("IS SENT BEFORE THAT."));
    CHECK(count_colour(GDX_NET_ROOM_COLOUR_DIM) == 6);
    CHECK(sHostCalls == 0 && sBrowseCalls == 0 && sJoinCalls == 0);

    /* The cursor stops at both ends. */
    CHECK(press(GDX_NET_ROOM_IN_UP) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE); /* a sound is taken once */
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    CHECK(gdx_net_room_screen_row() == 4);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 4);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    CHECK(find_text("BACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);

    /* BACK and B both leave for the cards. */
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_LEAVE);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_BACK);
    gdx_net_room_screen_open();
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_LEAVE);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_BACK);
    CHECK(sStopCalls == 0 && sDismissCalls == 0);
}

static void test_no_profile(void) {
    reset();
    sFakeProfile = 0;
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(!has_text("HOST A ROOM"));
    CHECK(!has_text("FIND ROOMS"));
    CHECK(!has_text("JOIN BY ADDRESS"));
    CHECK(!has_text("PORT"));
    CHECK(has_text("MAKE A PROFILE FIRST. OPEN THE MENU AND"));
    CHECK(has_text("BACK"));
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_LEAVE);
    CHECK(sHostCalls == 0 && sBrowseCalls == 0 && sJoinCalls == 0);
}

static void test_host(void) {
    reset();
    gdx_net_room_screen_open();
    fake_room(GDX_NET_ROOM_MODE_IDLE, GDX_NET_ROOM_PHASE_LOBBY, 0, 1, 0);
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(sHostCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM); /* in the same tick */
    CHECK(gdx_net_room_screen_row() == 0);

    /* A port that is taken: the page stays and says why. */
    reset();
    sHostAnswer = 0;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sHostCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(has_text("PORT 24302 IS IN USE BY ANOTHER PROGRAM."));
    CHECK(find_text("PORT 24302 IS IN USE BY ANOTHER PROGRAM.")->colour == GDX_NET_ROOM_COLOUR_RED);
    /* The reason goes when the player does something else. */
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(count_colour(GDX_NET_ROOM_COLOUR_RED) == 0);
}

static void test_port(void) {
    reset();
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    CHECK(gdx_net_room_screen_editing() == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(has_text("LEFT AND RIGHT PICK A DIGIT. UP AND DOWN"));

    /* Up and down change the digit and do not move the cursor. */
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 3);
    CHECK(field_is("PORT", "34302"));
    CHECK(sSetPortCalls == 0);
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(gdx_net_room_screen_editing() == 1);
    press_times(GDX_NET_ROOM_IN_RIGHT, 4);
    CHECK(gdx_net_room_screen_editing() == 5);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(gdx_net_room_screen_editing() == 5);
    press(GDX_NET_ROOM_IN_DOWN); /* 2 to 1 */
    press(GDX_NET_ROOM_IN_DOWN); /* 1 to 0 */
    press(GDX_NET_ROOM_IN_DOWN); /* 0 wraps to 9 */
    CHECK(field_is("PORT", "34309"));
    press(GDX_NET_ROOM_IN_UP); /* 9 wraps to 0 */
    CHECK(field_is("PORT", "34300"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 0);
    CHECK(sSetPortCalls == 1 && sFakePort == 34300u);
    CHECK(field_is("PORT", "34300"));

    /* A port the game cannot use becomes the nearest one it can. */
    set_field("PORT", "00080");
    CHECK(sSetPortCalls == 2 && sFakePort == 1024u);
    CHECK(field_is("PORT", "01024"));
    set_field("PORT", "99999");
    CHECK(sSetPortCalls == 3 && sFakePort == 65535u);
    CHECK(field_is("PORT", "65535"));

    /* B gives the change up. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(field_is("PORT", "55535"));
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_editing() == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(sSetPortCalls == 3 && sFakePort == 65535u);
    CHECK(field_is("PORT", "65535"));
}

static void test_rooms(void) {
    /* No room yet: one row. */
    reset();
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sBrowseCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(has_text("FIND ROOMS"));
    CHECK(has_text("SEARCHING YOUR NETWORK. NO ROOM YET."));
    CHECK(find_text("STOP SEARCHING")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* The first rooms to answer take the cursor: A is then about a room and does not end the search. */
    fake_search(3);
    press(0);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("DEN00")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(has_text("3 ROOMS ON YOUR NETWORK"));
    CHECK(has_text("DEN00") && has_text("DEN02"));
    CHECK(has_text("3 OF 8"));
    CHECK(has_text("LOBBY"));
    /* Only then. A cursor the player moved to the last row stays there when more rooms answer. */
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    CHECK(gdx_net_room_screen_row() == 3);
    fake_search(5);
    press(0);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(find_text("STOP SEARCHING")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 1); /* back on FIND ROOMS */

    /* A full list: ten rows at a time, following the cursor. */
    reset();
    fake_search(GDX_NET_ROOM_FOUND);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(count_prefix("DEN") == 10);
    CHECK(has_text("DEN00") && has_text("DEN09") && !has_text("DEN10"));
    CHECK(count_rects(2) == 2); /* the scroll bar */
    press_times(GDX_NET_ROOM_IN_DOWN, 31);
    CHECK(gdx_net_room_screen_row() == 31);
    CHECK(count_prefix("DEN") == 10);
    CHECK(has_text("DEN31") && has_text("DEN22") && !has_text("DEN21"));
    CHECK(find_text("DEN31")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX - 8);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 32);
    CHECK(find_text("STOP SEARCHING")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 32);
    press_times(GDX_NET_ROOM_IN_UP, 32);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(has_text("DEN00"));

    /* B stops the search. */
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
}

static void test_found(void) {
    reset();
    fake_search(8);
    sFake.found[5].watchers = 1;
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FOUND);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("DEN05") != NULL && find_text("DEN05")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(has_text("ADDRESS 192.168.1.5 PORT 24302"));
    CHECK(has_text("3 OF 8 RACERS - 1 OF 2 WATCHING"));
    CHECK(has_text("IN THE LOBBY."));
    CHECK(has_text("JOIN TO RACE") && has_text("JOIN TO WATCH") && has_text("BACK"));
    CHECK(sJoinCalls == 0);

    /* B goes back to that room's row. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM); /* BACK */
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);
    CHECK(gdx_net_room_screen_row() == 5);

    /* Join to race. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1 && sJoinIp == 0xC0A80105u && sJoinPort == 24302u && sJoinWatch == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);

    /* Join to watch. */
    reset();
    fake_search(8);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1 && sJoinIp == 0xC0A80102u && sJoinPort == 24302u && sJoinWatch == 1);

    /* Every race seat taken: only the watch seat can be chosen. */
    reset();
    fake_search(8);
    sFake.found[0].racers = 8;
    sFake.found[0].phase = GDX_NET_ROOM_PHASE_RACING;
    gdx_net_room_screen_open();
    CHECK(has_text("RACING"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(find_text("JOIN TO RACE")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(has_text("EVERY RACE SEAT IS TAKEN."));
    CHECK(!has_text("EVERY WATCH SEAT IS TAKEN."));
    CHECK(has_text("RACING. YOU JOIN FOR THE NEXT RACE."));
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1 && sJoinWatch == 1);

    /* Every watch seat taken too: nothing but BACK. */
    reset();
    fake_search(8);
    sFake.found[0].racers = 8;
    sFake.found[0].watchers = 2;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(has_text("EVERY RACE SEAT IS TAKEN.") && has_text("EVERY WATCH SEAT IS TAKEN."));
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 2);

    /* Another build: nothing but BACK, and the reason. */
    reset();
    fake_search(8);
    sFake.found[0].sameBuild = 0;
    gdx_net_room_screen_open();
    CHECK(has_text("OTHER BUILD"));
    CHECK(find_text("DEN01")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(find_text("DEN00")->colour == GDX_NET_ROOM_COLOUR_DIM);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FOUND);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(has_text("THIS ROOM RUNS ANOTHER BUILD OF THE"));
    CHECK(find_text("THIS ROOM RUNS ANOTHER BUILD OF THE")->colour == GDX_NET_ROOM_COLOUR_RED);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);

    /* The room goes while the player looks at it: back to the list. */
    reset();
    fake_search(8);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FOUND);
    sFake.foundCount = 7;
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOMS);
    /* The list changes order under the page: the page keeps its room by address. */
    reset();
    fake_search(8);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    press(GDX_NET_ROOM_IN_CONFIRM);
    sFake.found[0] = sFake.found[7];
    sFake.foundCount = 1;
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FOUND);
    CHECK(has_text("DEN07"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1 && sJoinIp == 0xC0A80107u);
}

static void test_address(void) {
    reset();
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ADDRESS);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(has_text("JOIN BY ADDRESS"));
    CHECK(field_is("ADDRESS", "192.168.001.001")); /* where most home networks are */
    CHECK(field_is("PORT", "24302"));
    CHECK(sJoinCalls == 0 && sBrowseCalls == 0);

    /* Join the address as it stands. */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(gdx_net_room_screen_row() == 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1 && sJoinIp == 0xC0A80101u && sJoinPort == 24302u && sJoinWatch == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);
    CHECK(has_text("JOINING 192.168.1.20 PORT 24302") == 0); /* no host text yet in this fake */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* A part over 255 becomes 255, and the typed address is kept between visits. */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ADDRESS);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 1);
    press_times(GDX_NET_ROOM_IN_RIGHT, 12);
    CHECK(gdx_net_room_screen_editing() == 12);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_editing() == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ADDRESS);
    set_field("ADDRESS", "999.168.010.077");
    CHECK(gdx_net_room_screen_editing() == 0);
    CHECK(field_is("ADDRESS", "255.168.010.077"));
    press(GDX_NET_ROOM_IN_DOWN);
    set_field("PORT", "00080");
    CHECK(field_is("PORT", "01024"));
    CHECK(sSetPortCalls == 0); /* the room's port, not this copy's setting */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(gdx_net_room_screen_row() == 3);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 2 && sJoinIp == 0xFFA80A4Du && sJoinPort == 1024u && sJoinWatch == 1);

    /* B in a field gives the change up. */
    reset();
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(field_is("ADDRESS", "255.168.010.077"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(field_is("ADDRESS", "355.168.010.077"));
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(field_is("ADDRESS", "255.168.010.077"));

    /* No address: the join rows cannot be chosen. */
    set_field("ADDRESS", "000.000.000.000");
    CHECK(field_is("ADDRESS", "000.000.000.000"));
    CHECK(has_text("SET THE ADDRESS OF THE HOST FIRST."));
    CHECK(find_text("JOIN TO RACE")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text("JOIN TO WATCH")->colour == GDX_NET_ROOM_COLOUR_DIM);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 4); /* over the two join rows */
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(sJoinCalls == 0);

    /* An address the session refuses: the page stays and the refusal sounds. */
    press(GDX_NET_ROOM_IN_UP);
    set_field("ADDRESS", "192.168.000.001");
    press(GDX_NET_ROOM_IN_DOWN);
    set_field("PORT", "24302");
    sJoinAnswer = 0;
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ADDRESS);

    /* B and BACK return to the front page, on the row the player came through. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    CHECK(gdx_net_room_screen_row() == 4);
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 2);
}

static void test_joining_and_closed(void) {
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    snprintf(sFake.hostText, sizeof(sFake.hostText), "192.168.1.20:24302");
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);
    CHECK(has_text("JOINING 192.168.1.20 PORT 24302"));
    CHECK(has_text("CANCEL"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* The host answers: the room. */
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    gdx_net_room_screen_open();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 1, 2, 0);
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);

    /* The host refuses: the reason, then the front page. */
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    snprintf(sFake.status, sizeof(sFake.status), "The room is full.");
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_CLOSED);
    CHECK(has_text("THE ROOM IS FULL."));
    CHECK(has_text("BACK"));
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(sDismissCalls == 1 && sStopCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    reset();
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    gdx_net_room_screen_open();
    CHECK(has_text("THE ROOM CLOSED."));
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(sDismissCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* A long reason wraps. */
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    snprintf(sFake.status, sizeof(sFake.status), "The host did not answer. Check the address and that both are on one network.");
    gdx_net_room_screen_open();
    CHECK(has_text("THE HOST DID NOT ANSWER. CHECK THE"));
    CHECK(has_text("ADDRESS AND THAT BOTH ARE ON ONE"));
    CHECK(has_text("NETWORK."));
}

/* ---- the room ---- */

static void test_room_racer(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("LOBBY") != NULL && find_text("LOBBY")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(has_text("ROOM AT 192.168.1.20 PORT 24302"));
    CHECK(has_text("4 OF 8 RACERS - 0 OF 2 WATCHING"));
    CHECK(has_text("THE RACES START WHEN EVERYONE IS READY"));
    CHECK(has_text("OR WHEN THE HOST STARTS THEM."));
    CHECK(has_text("READY UP") && has_text("SEND A MESSAGE") && has_text("LEAVE THE ROOM"));
    CHECK(find_text("READY UP")->y == 108 && find_text("SEND A MESSAGE")->y == 118);
    CHECK(find_text("LEAVE THE ROOM")->y == 128);
    CHECK(!has_text("START THE RACES") && !has_text("RACE SEATS") && !has_text("RACE NEXT"));
    CHECK(!has_text("SKIP THE LOTTERY") && !has_text("BACK TO THE LOBBY"));
    CHECK(has_text("P00") && has_text("P01") && has_text("P02") && has_text("P03"));
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_GOLD); /* the local player */
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(has_text("HOST") && has_text("11 MS") && has_text("13 MS") && !has_text("10 MS"));
    CHECK(count_prefix("NOT READY") == 4);
    CHECK(count_rects(6) == 4); /* a swatch of each player's colour */
    CHECK(!has_text("0:00"));

    /* Ready, and back. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sReadyCalls == 1 && sReadyValue == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    sFake.players[2].ready = 1;
    CHECK(has_text("CANCEL READY") && !has_text("READY UP"));
    CHECK(find_text("READY") != NULL && find_text("READY")->colour == GDX_NET_ROOM_COLOUR_GREEN);
    CHECK(count_prefix("NOT READY") == 3);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sReadyCalls == 2 && sReadyValue == 0);

    /* B takes the cursor to the leave row and leaves nothing. A there asks once more. */
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_BACK);
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 0);
    CHECK(has_text("PRESS A AGAIN TO LEAVE THE ROOM") && !has_text("LEAVE THE ROOM"));
    /* Moving away takes the question back. */
    press(GDX_NET_ROOM_IN_UP);
    CHECK(has_text("LEAVE THE ROOM"));
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 0);
    CHECK(has_text("PRESS A AGAIN TO LEAVE THE ROOM"));
    /* So does B. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(has_text("LEAVE THE ROOM"));
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* The player's own row does nothing. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    CHECK(sKickCalls == 0 && sReadyCalls == 0 && sStopCalls == 0 && sMuteCalls == 0);
    /* Left and right do nothing without the seats row. */
    press(GDX_NET_ROOM_IN_RIGHT);
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(sCapCalls == 0);

    /* A name with characters the font lacks is drawn with spaces in their place. */
    snprintf(sFake.players[1].name, sizeof(sFake.players[1].name), "A_b?");
    CHECK(has_text("A B "));
    CHECK(!has_text("A_B?"));
}

static void test_room_watcher(void) {
    int phase;

    for (phase = GDX_NET_ROOM_PHASE_LOBBY; phase <= GDX_NET_ROOM_PHASE_RESULTS; phase++) {
        reset();
        fake_room(GDX_NET_ROOM_MODE_JOINED, phase, 3, 4, 1);
        gdx_net_room_screen_open();
        CHECK(gdx_net_room_screen_row() == 0);
        CHECK(has_text("RACE NEXT") && !has_text("READY UP") && !has_text("KEEP WATCHING"));
        CHECK(has_text("3 OF 8 RACERS - 1 OF 2 WATCHING"));
        CHECK(has_text("WATCHING"));
        press(GDX_NET_ROOM_IN_CONFIRM);
        CHECK(sRaceNextCalls == 1 && sRaceNextValue == 1);
        sFake.players[3].raceNext = 1;
        CHECK(has_text("KEEP WATCHING") && !has_text("RACE NEXT"));
        CHECK(has_text("NEXT RACE") && !has_text("WATCHING"));
        press(GDX_NET_ROOM_IN_CONFIRM);
        CHECK(sRaceNextCalls == 2 && sRaceNextValue == 0);
        CHECK(sReadyCalls == 0);
        press(GDX_NET_ROOM_IN_DOWN);
        CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
        press(GDX_NET_ROOM_IN_DOWN);
        CHECK(has_text("LEAVE THE ROOM"));
        CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    }
}

static void test_room_host(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 4, 0);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(has_text("YOUR ROOM ON PORT 24302"));
    CHECK(has_text("OR WHEN YOU START THEM."));
    CHECK(has_text("READY UP") && has_text("START THE RACES") && has_text("RACE SEATS") && has_text("CLOSE THE ROOM"));
    CHECK(find_text("SEND A MESSAGE") != NULL && find_text("SEND A MESSAGE")->y == 138);
    CHECK(find_text("CLOSE THE ROOM")->y == 148);
    CHECK(!has_text("LEAVE THE ROOM"));
    CHECK(!has_text("SKIP THE LOTTERY") && !has_text("BACK TO THE LOBBY"));
    CHECK(find_text("8") != NULL && find_text("8")->y == find_text("RACE SEATS")->y);

    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStartCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);

    /* Race seats: left and right, from 8 to 12. */
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 0); /* not on that row */
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 2);
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(sCapCalls == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 1 && sCapValue == 9);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    sFake.racerCap = 12;
    CHECK(has_text("12"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 1);
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(sCapCalls == 2 && sCapValue == 11);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    CHECK(sCapCalls == 2 && sStartCalls == 1);

    /* Close the room: asked twice, like leaving. */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("PRESS A AGAIN TO CLOSE THE ROOM"));
    CHECK(sStopCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1);

    /* The host's own row does nothing; another player's opens that player. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(find_text("P00")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(gdx_net_room_screen_row() == 2); /* on BACK: removing takes a move and a press */
    CHECK(find_text("P01") != NULL && find_text("P01")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(has_text("MUTE THIS PLAYER") && has_text("REMOVE FROM THE ROOM") && has_text("BACK"));
    CHECK(find_text("MUTE THIS PLAYER")->y == 80 && find_text("REMOVE FROM THE ROOM")->y == 92);
    CHECK(find_text("BACK")->y == 104);
    CHECK(sKickCalls == 0);
    /* BACK and B return to that player's row. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 6);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 6);
    CHECK(sKickCalls == 0 && sStopCalls == 0);
    /* Remove. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sKickCalls == 1 && sKickId == 1);
    CHECK(sMuteCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    /* The player leaves while the host looks at them: back to the room. */
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    remove_player(2);
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sKickCalls == 1);

    /* Outside the lobby the host has no start and no seats row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_VOTING, 0, 4, 0);
    sFake.players[0].voted = 1; /* the host's ballot is in, so the vote adds no row either */
    gdx_net_room_screen_open();
    CHECK(!has_text("READY UP") && !has_text("START THE RACES") && !has_text("RACE SEATS"));
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("CLOSE THE ROOM") != NULL && find_text("CLOSE THE ROOM")->y == 118);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 0);
}

static void test_room_phases(void) {
    static const char* const kTitles[9] = { "LOBBY",          "TRACK VOTE", "VOTE RESULT", "TRACK PICK", "MACHINE SELECT",
                                            "LOADING",        "RACE START", "RACE",        "RESULTS" };
    int phase;

    for (phase = GDX_NET_ROOM_PHASE_LOBBY; phase <= GDX_NET_ROOM_PHASE_RESULTS; phase++) {
        reset();
        fake_room(GDX_NET_ROOM_MODE_JOINED, phase, 2, 5, 1);
        gdx_net_room_screen_open();
        CHECK(find_text(kTitles[phase]) != NULL && find_text(kTitles[phase])->font == GDX_NET_ROOM_FONT_TITLE);
        CHECK(has_text("READY UP") == (phase == GDX_NET_ROOM_PHASE_LOBBY));
        CHECK(has_text("LEAVE THE ROOM"));
        CHECK(has_text("WATCHING"));
    }

    /* The timer, when the phase has one. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    sFake.phaseTotalMs = 30000u;
    sFake.phaseLeftMs = 12001u;
    sFake.players[1].voted = 1;
    gdx_net_room_screen_open();
    CHECK(has_text("0:13"));
    CHECK(find_text("0:13")->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(find_text("0:13")->font == GDX_NET_ROOM_FONT_TITLE); /* the font with a colon */
    CHECK(has_text("THE RACERS VOTE FOR THE NEXT TRACK."));
    CHECK(has_text("1 OF 4 VOTED."));
    CHECK(count_prefix("VOTING") == 3);
    CHECK(find_text("VOTED") != NULL && find_text("VOTED")->colour == GDX_NET_ROOM_COLOUR_GREEN);
    sFake.phaseTotalMs = 0u;
    CHECK(!has_text("0:13"));

    /* The lottery. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOTTERY, 2, 5, 1);
    sFake.lotteryTrack = 7;
    gdx_net_room_screen_open();
    CHECK(has_text("NOBODY VOTED."));
    CHECK(has_text("THE ROOM DREW TRACK 7."));
    sFake.lotteryCount = 2;
    sFake.lottery[0].id = 1;
    sFake.lottery[0].track = 3;
    sFake.lottery[1].id = 3;
    sFake.lottery[1].track = 7;
    sFake.lotteryWinner = 1;
    CHECK(has_text("P03 WON THE VOTE."));
    CHECK(has_text("NEXT TRACK TRACK 7."));
    sFake.lottery[1].id = 9; /* the winner left the room */
    CHECK(has_text("A PLAYER WHO LEFT WON THE VOTE."));

    /* The pick. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 5, 1);
    sFake.picker = 3;
    gdx_net_room_screen_open();
    CHECK(has_text("P03 FINISHED LAST AND PICKS THE NEXT"));
    CHECK(has_text("TRACK."));
    CHECK(count_prefix("PICKS") == 1);
    CHECK(count_prefix("WAITING") == 3);
    CHECK(find_text("PICKS")->y == find_text("P03")->y);
    sFake.picker = 2;
    CHECK(has_text("YOU FINISHED LAST AND PICK THE NEXT"));
    CHECK(has_text("TRACK."));
    CHECK(find_text("PICKS")->y == find_text("P02")->y);

    /* Machine select. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_MACHINE, 2, 5, 1);
    sFake.track = 3;
    sFake.lockedCount = 1;
    sFake.racers = 3;
    sFake.players[0].inRace = 1;
    sFake.players[1].inRace = 1;
    sFake.players[1].locked = 1;
    sFake.players[2].inRace = 1;
    gdx_net_room_screen_open();
    CHECK(has_text("NEXT TRACK TRACK 3."));
    CHECK(has_text("1 OF 3 LOCKED IN."));
    CHECK(count_prefix("CHOOSING") == 2);
    CHECK(find_text("LOCKED IN") != NULL && find_text("LOCKED IN")->colour == GDX_NET_ROOM_COLOUR_GREEN);
    CHECK(find_text("LOCKED IN")->y == find_text("P01")->y);
    CHECK(find_text("SITS OUT") != NULL && find_text("SITS OUT")->y == find_text("P03")->y);

    /* The race. */
    sFake.phase = GDX_NET_ROOM_PHASE_GRID_SYNC;
    CHECK(has_text("LOADING TRACK 3."));
    CHECK(count_prefix("RACING") == 3 && count_prefix("SITS OUT") == 1);
    sFake.phase = GDX_NET_ROOM_PHASE_COUNTDOWN;
    CHECK(has_text("TRACK 3 IS ABOUT TO START."));
    sFake.phase = GDX_NET_ROOM_PHASE_RACING;
    CHECK(has_text("THE RACE IS ON AT TRACK 3."));
    CHECK(find_text("NOBODY HAS FINISHED YET.") != NULL && find_text("NOBODY HAS FINISHED YET.")->y == 92);
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    CHECK(has_text("THE RACE AT TRACK 3 IS OVER."));

    /* Points show once anyone has some. */
    CHECK(!has_text("0"));
    sFake.players[1].points = 25;
    CHECK(has_text("25"));
    CHECK(find_text("25")->y == find_text("P01")->y && find_text("25")->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(count_prefix("0") == 4);
}

/* The row that opens the course select for the vote or the pick (gdx_net_track_choice.h). */
static void test_room_track_row(void) {
    int listY;

    /* A racer whose ballot is in: no such row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    sFake.players[2].voted = 1;
    gdx_net_room_screen_open();
    CHECK(!has_text("VOTE FOR A TRACK") && !has_text("PICK THE TRACK"));
    CHECK(find_text("SEND A MESSAGE")->y == 108);
    CHECK(find_text("LEAVE THE ROOM")->y == 118);
    listY = find_text("P00")->y;
    CHECK(listY == 136);

    /* A racer who has not voted: the row is the top one, the cursor starts on it, and the list moves down
       a row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 2, 5, 1);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("VOTE FOR A TRACK") != NULL && find_text("VOTE FOR A TRACK")->y == 108);
    CHECK(find_text("VOTE FOR A TRACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("VOTE FOR A TRACK")->font == GDX_NET_ROOM_FONT_SMALL);
    CHECK(!has_text("PICK THE TRACK"));
    CHECK(find_text("SEND A MESSAGE")->y == 118);
    CHECK(find_text("LEAVE THE ROOM")->y == 128);
    CHECK(find_text("P00")->y == listY + 10);
    CHECK(find_text("VOTING")->y == find_text("P00")->y);

    /* The course select opened by itself and the player came back with B: A on the row opens it again. It
       sends nothing itself. */
    CHECK(gdx_net_track_choice_poll_open() == 1);
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 0);
    gdx_net_room_screen_take_sound();
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(sVoteCalls == 0 && sPickCalls == 0 && sReadyCalls == 0 && sStopCalls == 0);
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(sMenuCloseCalls == 2);
    /* The other rows do not ask for it. */
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 0);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(gdx_net_track_choice_poll_open() == 0);

    /* The ballot went in: the row is gone and the rows under it are a row higher again. */
    sFake.players[2].voted = 1;
    press(0);
    CHECK(!has_text("VOTE FOR A TRACK"));
    CHECK(find_text("SEND A MESSAGE")->y == 108);
    CHECK(find_text("LEAVE THE ROOM")->y == 118);
    CHECK(find_text("P00")->y == listY);

    /* A watcher has no ballot. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_VOTING, 4, 5, 1);
    gdx_net_room_screen_open();
    CHECK(!has_text("VOTE FOR A TRACK") && !has_text("PICK THE TRACK"));

    /* The host votes as any racer does, from the top row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_VOTING, 0, 5, 1);
    gdx_net_room_screen_open();
    CHECK(find_text("VOTE FOR A TRACK") != NULL && find_text("VOTE FOR A TRACK")->y == 108);
    CHECK(find_text("VOTE FOR A TRACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);

    /* The pick: the row is the picker's alone. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 3, 0);
    sFake.picker = 1;
    gdx_net_room_screen_open();
    CHECK(!has_text("VOTE FOR A TRACK") && !has_text("PICK THE TRACK"));
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_PICK, 2, 3, 0);
    sFake.picker = 2;
    gdx_net_room_screen_open();
    CHECK(find_text("PICK THE TRACK") != NULL && find_text("PICK THE TRACK")->y == 108);
    CHECK(find_text("PICK THE TRACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(!has_text("VOTE FOR A TRACK"));
    CHECK(gdx_net_track_choice_poll_open() == 1);
    gdx_net_track_choice_back();
    CHECK(gdx_net_track_choice_poll_open() == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_track_choice_poll_open() == 1);
    CHECK(sPickCalls == 0 && sVoteCalls == 0);

    /* No other phase has the row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOTTERY, 2, 5, 1);
    gdx_net_room_screen_open();
    CHECK(!has_text("VOTE FOR A TRACK") && !has_text("PICK THE TRACK"));
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 5, 1);
    gdx_net_room_screen_open();
    CHECK(!has_text("VOTE FOR A TRACK") && !has_text("PICK THE TRACK"));
    CHECK(find_text("READY UP")->y == 108);
}

static void test_room_full(void) {
    int i;

    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, GDX_NET_ROOM_PLAYERS, 2);
    sFake.racerCap = 12;
    for (i = 0; i < GDX_NET_ROOM_PLAYERS; i++) {
        sFake.players[i].points = 100u + (unsigned) i;
        sFake.players[i].pingMs = 900u + (unsigned) i;
        snprintf(sFake.players[i].name, sizeof(sFake.players[i].name), "WWWWWWWWWWWWWW%02d", i);
    }
    gdx_net_room_screen_open();
    CHECK(has_text("12 OF 12 RACERS - 2 OF 2 WATCHING"));
    /* Five rows of the host's, then five players at a time: the list ends above the two lines of the log. */
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 5);
    CHECK(has_text("WWWWWWWWWWWWWW00") && has_text("WWWWWWWWWWWWWW04") && !has_text("WWWWWWWWWWWWWW05"));
    CHECK(find_text("WWWWWWWWWWWWWW00")->y == 166 && find_text("WWWWWWWWWWWWWW04")->y == 206);
    CHECK(count_rects(2) == 2);
    CHECK(count_rects(6) == 5);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    press_times(GDX_NET_ROOM_IN_DOWN, 18);
    CHECK(gdx_net_room_screen_row() == 18);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 18);
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 5);
    CHECK(has_text("WWWWWWWWWWWWWW13") && has_text("WWWWWWWWWWWWWW09") && !has_text("WWWWWWWWWWWWWW08"));
    CHECK(find_text("WWWWWWWWWWWWWW13")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(has_text("113") && has_text("913 MS"));
    CHECK(count_rects(288) == 1);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    /* Every row of a player stays above the log's lines. */
    CHECK(find_text("WWWWWWWWWWWWWW13")->y == 206);
    /* The list shortens under the cursor: the cursor ends on its last row. */
    sFake.playerCount = 5;
    press(0);
    CHECK(gdx_net_room_screen_row() == 9);
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 5);
    CHECK(count_rects(2) == 0);

    /* A client outside the lobby has two rows of its own, so more players fit. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RACING, 5, GDX_NET_ROOM_PLAYERS, 2);
    gdx_net_room_screen_open();
    CHECK(count_prefix("P") == 8);
    press_times(GDX_NET_ROOM_IN_DOWN, 15);
    CHECK(gdx_net_room_screen_row() == 15);
    CHECK(has_text("P13") && has_text("P06") && !has_text("P05"));
    CHECK(find_text("P13")->y == 206);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
}

static void test_cursor_follows(void) {
    /* The ready row goes with the lobby: the cursor stays on the leave row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(gdx_net_room_screen_row() == 2);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    press(0);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* The frame between the change and the next tick draws the right row too. */
    sFake.phase = GDX_NET_ROOM_PHASE_LOBBY;
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("READY UP")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(gdx_net_room_screen_row() == 2);

    /* The cursor on the ready row when it goes: the row in its place takes it, which never leaves the room. */
    press_times(GDX_NET_ROOM_IN_UP, 2);
    CHECK(gdx_net_room_screen_row() == 0);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    press(0);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 0 && sMessageCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);

    /* A vote starts under the cursor: it stays on the leave row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    press(0);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("VOTE FOR A TRACK")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    /* The ballot goes in: still the leave row, a row higher. */
    sFake.players[2].voted = 1;
    press(0);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* From the ready row, which goes, the vote's row takes it: the row in its place. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    sFake.phase = GDX_NET_ROOM_PHASE_VOTING;
    press(0);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("VOTE FOR A TRACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);

    /* A player above the cursor's leaves: the cursor stays on its player. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_MACHINE, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(find_text("P03")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    remove_player(1);
    press(0);
    CHECK(gdx_net_room_screen_row() == 4);
    CHECK(find_text("P03")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* The cursor's own player leaves: the last row left takes it. */
    remove_player(3);
    press(0);
    CHECK(gdx_net_room_screen_row() == 3);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_CURSOR);

    /* The room closes under the screen: the closed page, then the front page. */
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    snprintf(sFake.status, sizeof(sFake.status), "The host closed the room.");
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_CLOSED);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(has_text("THE HOST CLOSED THE ROOM."));
}

/* A room that just opened has no roster for a moment: the cursor is on the top row when the roster comes. */
static void test_room_before_roster(void) {
    /* The host: A is then "READY UP", not "START THE RACES". */
    reset();
    sFake.localId = 0;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sHostCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(!has_text("READY UP") && has_text("START THE RACES"));
    press(0);
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 1, 0);
    press(0);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("READY UP")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sReadyCalls == 1 && sStartCalls == 0);

    /* A cursor the player moved before the roster came stays on its row. */
    reset();
    sFake.localId = 0;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 1); /* race seats, under "START THE RACES" */
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 1, 0);
    press(0);
    CHECK(gdx_net_room_screen_row() == 2);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 1 && sCapValue == 9);

    /* A joiner whose welcome came before the roster: A is "READY UP", not the leave row. */
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINED;
    sFake.localId = 2;
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(has_text("LEAVE THE ROOM") && !has_text("READY UP"));
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    CHECK(find_text("READY UP")->colour == GDX_NET_ROOM_COLOUR_CURSOR); /* in the frame before the next tick */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sReadyCalls == 1 && sStopCalls == 0);
    CHECK(find_text("LEAVE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_WHITE);
}

/* The colour of the 6 x 6 swatch whose top is on row y, or 0 when there is none. */
static unsigned swatch_rgba_at(int y) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int i;

    for (i = 0; i < count; i++) {
        if (items[i].kind == GDX_NET_ROOM_DRAW_RECT && items[i].w == 6 && items[i].h == 6 && items[i].y == y) {
            return items[i].rgba;
        }
    }
    return 0;
}

/* The host's rows of a phase: the lottery can be skipped, a race ended and the results left for the lobby. */
static void test_room_host_controls(void) {
    /* The lottery: one press draws the track. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOTTERY, 0, 4, 0);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(find_text("SKIP THE LOTTERY") != NULL && find_text("SKIP THE LOTTERY")->y == 108);
    CHECK(find_text("SKIP THE LOTTERY")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("SEND A MESSAGE")->y == 118 && find_text("CLOSE THE ROOM")->y == 128);
    CHECK(!has_text("BACK TO THE LOBBY") && !has_text("START THE RACES"));
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sSkipCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(sEndSeriesCalls == 0 && sStopCalls == 0);
    /* A client has no such row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOTTERY, 2, 4, 0);
    gdx_net_room_screen_open();
    CHECK(!has_text("SKIP THE LOTTERY"));
    CHECK(find_text("SEND A MESSAGE")->y == 108);

    /* The race: nobody ends it for the others, the host included. The host's page has the rows a client's
     * has, with the room's own word for leaving. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RACING, 0, 4, 0);
    gdx_net_room_screen_open();
    CHECK(!has_text("SKIP THE LOTTERY") && !has_text("BACK TO THE LOBBY") && !has_text("START THE RACES"));
    CHECK(find_text("SEND A MESSAGE") != NULL && find_text("SEND A MESSAGE")->y == 108);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("CLOSE THE ROOM") != NULL && find_text("CLOSE THE ROOM")->y == 118);
    CHECK(gdx_net_room_screen_row() == 0);

    /* The results: the row that ends the series comes in above. A cursor the player has not moved waits at
     * the top of the page, so it is on the new row. */
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    press(0);
    CHECK(find_text("BACK TO THE LOBBY") != NULL && find_text("BACK TO THE LOBBY")->y == 108);
    CHECK(find_text("BACK TO THE LOBBY")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("SEND A MESSAGE")->y == 118 && find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(sEndSeriesCalls == 0 && sStopCalls == 0);
    /* A cursor the player has moved stays with its own row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RACING, 0, 4, 0);
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(find_text("SEND A MESSAGE")->y == 108 && find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    press(0);
    CHECK(find_text("BACK TO THE LOBBY") != NULL && find_text("BACK TO THE LOBBY")->y == 108);
    CHECK(find_text("BACK TO THE LOBBY")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(find_text("SEND A MESSAGE")->y == 118 && find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(sEndSeriesCalls == 0 && sStopCalls == 0);

    /* Going back to the lobby is asked twice. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RESULTS, 0, 4, 0);
    gdx_net_room_screen_open();
    CHECK(find_text("BACK TO THE LOBBY") != NULL && find_text("BACK TO THE LOBBY")->y == 108);
    CHECK(find_text("BACK TO THE LOBBY")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sEndSeriesCalls == 0);
    CHECK(has_text("PRESS A AGAIN TO GO BACK TO THE LOBBY") && !has_text("BACK TO THE LOBBY"));
    /* B answers no and moves nothing. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(has_text("BACK TO THE LOBBY") && !has_text("PRESS A AGAIN TO GO BACK TO THE LOBBY"));
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(sEndSeriesCalls == 0);
    /* Moving away answers no too. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(has_text("BACK TO THE LOBBY"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sEndSeriesCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sEndSeriesCalls == 1);
    CHECK(has_text("BACK TO THE LOBBY"));
    CHECK(sStopCalls == 0 && sSkipCalls == 0);

    /* A client has no such row. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RACING, 2, 4, 0);
    gdx_net_room_screen_open();
    CHECK(find_text("SEND A MESSAGE")->y == 108 && find_text("LEAVE THE ROOM")->y == 118);
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    CHECK(!has_text("BACK TO THE LOBBY"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sEndSeriesCalls == 0);

    /* Each row asks for itself: one press of "BACK TO THE LOBBY" and one of "CLOSE THE ROOM" do neither. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RESULTS, 0, 4, 0);
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(find_text("CLOSE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 0 && sEndSeriesCalls == 0);
    CHECK(has_text("PRESS A AGAIN TO CLOSE THE ROOM") && has_text("BACK TO THE LOBBY"));
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1 && sEndSeriesCalls == 0);

    /* A race ends while "CLOSE THE ROOM" waits for its second press: the cursor is still on that row, one
     * line lower, and the second press is the answer to it. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RACING, 0, 4, 0);
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("PRESS A AGAIN TO CLOSE THE ROOM") && sStopCalls == 0);
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    press(0);
    CHECK(find_text("PRESS A AGAIN TO CLOSE THE ROOM") != NULL && find_text("PRESS A AGAIN TO CLOSE THE ROOM")->y == 128);
    CHECK(find_text("PRESS A AGAIN TO CLOSE THE ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sStopCalls == 1 && sEndSeriesCalls == 0);
}

/* The messages page: the eight messages a player can send, and the room's last lines above them. */
static void test_room_messages(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(sMessageCalls == 0);
    CHECK(find_text("MESSAGES") != NULL && find_text("MESSAGES")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(find_text("NO MESSAGE YET.") != NULL && find_text("NO MESSAGE YET.")->y == 60);
    CHECK(find_text("NO MESSAGE YET.")->colour == GDX_NET_ROOM_COLOUR_DIM);
    /* The eight in the room's order, in the glyphs the small font has, and "BACK" a little apart. */
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(find_text("HELLO ") != NULL && find_text("HELLO ")->y == 126);
    CHECK(find_text("HELLO ")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("GOOD LUCK ") != NULL && find_text("GOOD LUCK ")->y == 136);
    CHECK(find_text("GOOD RACE ") != NULL && find_text("GOOD RACE ")->y == 146);
    CHECK(find_text("NICE ONE ") != NULL && find_text("NICE ONE ")->y == 156);
    CHECK(find_text("AGAIN ") != NULL && find_text("AGAIN ")->y == 166);
    CHECK(find_text("THANKS ") != NULL && find_text("THANKS ")->y == 176);
    CHECK(find_text("SORRY ") != NULL && find_text("SORRY ")->y == 186);
    CHECK(find_text("I HAVE TO GO.") != NULL && find_text("I HAVE TO GO.")->y == 196);
    CHECK(find_text("BACK") != NULL && find_text("BACK")->y == 210);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* Sending returns to the room, the cursor on the row the player came through. */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(find_text("GOOD RACE ")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMessageCalls == 1 && sMessageIndex == 2);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* The last of them. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    CHECK(find_text("I HAVE TO GO.")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMessageCalls == 2 && sMessageIndex == 7);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);

    /* BACK and B send nothing. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 8);
    CHECK(gdx_net_room_screen_row() == 8);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 8);
    CHECK(find_text("BACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 1);
    CHECK(sMessageCalls == 2);
    CHECK(sReadyCalls == 0 && sStopCalls == 0);

    /* The page lasts through a change of phase, and ends with the room. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    sFake.mode = GDX_NET_ROOM_MODE_CLOSED;
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_CLOSED);

    /* The host has the same page. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMessageCalls == 1 && sMessageIndex == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 3);
    CHECK(sStartCalls == 0 && sReadyCalls == 0 && sStopCalls == 0);
}

/* The room's log: two lines at the foot of the room page, six on the messages page. */
static void test_room_log(void) {
    const GdxNetRoomDrawItem* item;

    /* No line: nothing at the foot. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    CHECK(count_swatches_at(215) == 0 && count_swatches_at(225) == 0);
    CHECK(count_rects(6) == 4);

    /* A notice and a message, oldest first. The notice is dim; the message is white behind the square of its
     * player's colour and reads NAME - MESSAGE, because the small font has no colon. */
    fake_line(GDX_NET_ROOM_NOBODY, "P03 joined the room.");
    fake_line(1, "Hello!");
    item = find_text_at("P03 JOINED THE ROOM.", 222);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_DIM && item->x == 20);
    item = find_text_at("P01 - HELLO ", 232);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_WHITE && item->x == 32);
    CHECK(count_swatches_at(215) == 0 && count_swatches_at(225) == 1);
    CHECK(swatch_rgba_at(225) == 0x102031FFu);
    CHECK(count_rects(6) == 5);
    /* Two more: the foot has the two newest. */
    fake_line(3, "Thanks!");
    fake_line(0, "Good race!");
    CHECK(!has_text("P03 JOINED THE ROOM.") && !has_text("P01 - HELLO "));
    CHECK(find_text_at("P03 - THANKS ", 222) != NULL);
    CHECK(find_text_at("P00 - GOOD RACE ", 232) != NULL);
    CHECK(swatch_rgba_at(215) == 0x102033FFu && swatch_rgba_at(225) == 0x102030FFu);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* A notice of two lines is the whole foot. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    fake_line(1, "Hello!");
    fake_line(GDX_NET_ROOM_NOBODY, "P01 sent too many messages too fast and has to wait.");
    gdx_net_room_screen_open();
    CHECK(find_text_at("P01 SENT TOO MANY MESSAGES TOO FAST AND", 222) != NULL);
    CHECK(find_text_at("HAS TO WAIT.", 232) != NULL);
    CHECK(find_text_at("HAS TO WAIT.", 232)->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(!has_text("P01 - HELLO "));
    CHECK(count_swatches_at(215) == 0 && count_swatches_at(225) == 0);

    /* An entry is never shown in part: the newest takes a line and the one before it needs two. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    fake_line(GDX_NET_ROOM_NOBODY, "P01 sent too many messages too fast and has to wait.");
    fake_line(1, "Sorry!");
    gdx_net_room_screen_open();
    CHECK(find_text_at("P01 - SORRY ", 222) != NULL);
    CHECK(!has_text("HAS TO WAIT.") && !has_text("P01 SENT TOO MANY MESSAGES TOO FAST AND"));
    CHECK(count_swatches_at(215) == 1 && count_swatches_at(225) == 0);

    /* The messages page has room for both, and for a message of two lines: the square stands before its
     * first line only. */
    fake_line(3, "one two three four five six seven eight nine");
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    CHECK(!has_text("NO MESSAGE YET."));
    item = find_text_at("P01 SENT TOO MANY MESSAGES TOO FAST AND", 60);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_DIM && item->x == 20);
    CHECK(find_text_at("HAS TO WAIT.", 70) != NULL);
    item = find_text_at("P01 - SORRY ", 80);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_WHITE && item->x == 32);
    item = find_text_at("P03 - ONE TWO THREE FOUR FIVE SIX", 90);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_WHITE && item->x == 32);
    item = find_text_at("SEVEN EIGHT NINE", 100);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_WHITE && item->x == 32);
    CHECK(swatch_rgba_at(73) == 0x102031FFu && swatch_rgba_at(83) == 0x102033FFu);
    CHECK(count_swatches_at(93) == 0);
    CHECK(count_rects(6) == 2);
    /* Six lines at most: a fourth entry of two lines pushes the oldest out whole. */
    fake_line(GDX_NET_ROOM_NOBODY, "P02 sent too many messages too fast and has to wait.");
    CHECK(!has_text("P01 SENT TOO MANY MESSAGES TOO FAST AND"));
    CHECK(find_text_at("P01 - SORRY ", 60) != NULL);
    CHECK(find_text_at("P03 - ONE TWO THREE FOUR FIVE SIX", 70) != NULL);
    CHECK(find_text_at("SEVEN EIGHT NINE", 80) != NULL);
    CHECK(find_text_at("P02 SENT TOO MANY MESSAGES TOO FAST AND", 90) != NULL);
    CHECK(find_text_at("HAS TO WAIT.", 100) != NULL);
    CHECK(count_prefix("HAS TO WAIT.") == 1);
    /* The lowest line of the log stays above the first message to send. */
    CHECK(find_text("HELLO ")->y == 126);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* A full room of the host's with a full foot. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, GDX_NET_ROOM_PLAYERS, 2);
    fake_line(5, "Good luck!");
    fake_line(6, "Good luck!");
    gdx_net_room_screen_open();
    CHECK(find_text("P04")->y == 206 && !has_text("P05"));
    CHECK(find_text_at("P05 - GOOD LUCK ", 222) != NULL && find_text_at("P06 - GOOD LUCK ", 232) != NULL);
    CHECK(count_rects(6) == 7);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
}

/* The page of another player: anyone can hide that player's messages, and only the host can remove them. */
static void test_room_mute(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_CONFIRM);
    CHECK(find_text("P01") != NULL && find_text("P01")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(has_text("MUTE THIS PLAYER") && has_text("BACK") && !has_text("REMOVE FROM THE ROOM"));
    CHECK(find_text("MUTE THIS PLAYER")->y == 80 && find_text("BACK")->y == 92);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(find_text_at("MUTING HIDES MESSAGES FROM THIS PLAYER", 116) != NULL);
    CHECK(find_text_at("ON YOUR SCREEN ONLY.", 126) != NULL);
    CHECK(find_text_at("ON YOUR SCREEN ONLY.", 126)->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(sMuteCalls == 0);

    /* Mute: the page stays and says what it did. */
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMuteCalls == 1 && sMuteId == 1 && sMuteValue == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER && gdx_net_room_screen_row() == 0);
    CHECK(has_text("UNMUTE THIS PLAYER") && !has_text("MUTE THIS PLAYER"));
    CHECK(find_text_at("MESSAGES FROM THIS PLAYER ARE HIDDEN ON", 116) != NULL);
    CHECK(find_text_at("YOUR SCREEN.", 126) != NULL);
    CHECK(sKickCalls == 0 && sStopCalls == 0);
    /* The roster says it where the ping was. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 4);
    CHECK(find_text("MUTED") != NULL && find_text("MUTED")->y == find_text("P01")->y);
    CHECK(find_text("MUTED")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text("MUTED")->align == GDX_NET_ROOM_ALIGN_RIGHT && find_text("MUTED")->x == 300);
    CHECK(!has_text("11 MS") && has_text("13 MS") && has_text("HOST"));
    CHECK(count_prefix("MUTED") == 1);

    /* Unmute. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(has_text("UNMUTE THIS PLAYER"));
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMuteCalls == 2 && sMuteId == 1 && sMuteValue == 0);
    CHECK(has_text("MUTE THIS PLAYER") && !has_text("UNMUTE THIS PLAYER"));
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(!has_text("MUTED") && has_text("11 MS"));

    /* A muted host reads MUTED too. */
    press(GDX_NET_ROOM_IN_UP);
    CHECK(find_text("P00")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMuteCalls == 3 && sMuteId == 0 && sMuteValue == 1);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(has_text("MUTED") && !has_text("HOST"));
    /* The player leaves while their page is open: back to the room. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    remove_player(0);
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(!has_text("MUTED"));

    /* The host has the row above "REMOVE FROM THE ROOM", and muting removes nobody. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 6);
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER && gdx_net_room_screen_row() == 2);
    CHECK(find_text_at("MUTING HIDES MESSAGES FROM THIS PLAYER", 128) != NULL);
    CHECK(find_text_at("ON YOUR SCREEN ONLY.", 138) != NULL);
    press_times(GDX_NET_ROOM_IN_UP, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMuteCalls == 1 && sMuteId == 1 && sMuteValue == 1);
    CHECK(sKickCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(has_text("UNMUTE THIS PLAYER") && has_text("REMOVE FROM THE ROOM"));
}

/* The table of a race stands where the roster does, from the first racer over the line to the end of the
 * results. */
static void test_room_table(void) {
    static const int kRunIds[4] = { 1, 3, 0, 2 };
    static const int kRunKinds[4] = { GDX_NET_ROOM_RESULT_FINISHED, GDX_NET_ROOM_RESULT_RETIRED,
                                      GDX_NET_ROOM_RESULT_UNFINISHED, GDX_NET_ROOM_RESULT_UNFINISHED };
    static const unsigned kRunTimes[4] = { 83456u, 0u, 0u, 0u };
    static const int kRunPoints[4] = { 0, 0, 0, 0 };
    static const int kEndIds[4] = { 1, 2, 0, 3 };
    static const int kEndKinds[4] = { GDX_NET_ROOM_RESULT_FINISHED, GDX_NET_ROOM_RESULT_FINISHED,
                                      GDX_NET_ROOM_RESULT_UNFINISHED, GDX_NET_ROOM_RESULT_RETIRED };
    static const unsigned kEndTimes[4] = { 83456u, 125007u, 0u, 0u };
    static const int kEndPoints[4] = { 25, 18, 4, 0 };
    int ids[GDX_NET_ROOM_RACERS];
    int kinds[GDX_NET_ROOM_RACERS];
    unsigned times[GDX_NET_ROOM_RACERS];
    int points[GDX_NET_ROOM_RACERS];
    const GdxNetRoomDrawItem* item;
    int i;

    /* A race nobody has finished: the roster, and a line that says so. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RACING, 2, 5, 1);
    sFake.track = 3;
    sFake.raceIndex = 4u;
    for (i = 0; i < 4; i++) {
        sFake.players[i].inRace = 1;
    }
    sFake.players[0].points = 7u;
    sFake.players[1].points = 30u;
    sFake.players[2].points = 12u;
    gdx_net_room_screen_open();
    CHECK(has_text("THE RACE IS ON AT TRACK 3.") && has_text("NOBODY HAS FINISHED YET."));
    CHECK(has_text("ROOM AT 192.168.1.20 PORT 24302"));
    CHECK(!has_text("TIME") && !has_text("TOTAL") && !has_text("PTS"));
    CHECK(find_text("SEND A MESSAGE")->y == 108 && find_text("P00")->y == 136);
    CHECK(count_rects(6) == 5);
    /* The table of an earlier race is not this race's. */
    fake_table(0, 4, kRunIds, kRunKinds, kRunTimes, kRunPoints);
    sFake.resultsRace = 3u;
    CHECK(has_text("NOBODY HAS FINISHED YET.") && !has_text("TIME"));
    CHECK(find_text("P00")->y == 136);

    /* The first racer is over the line. The table takes the rows of the room's address and its seat counts,
     * and has no squares. */
    sFake.resultsRace = 4u;
    press(0);
    CHECK(has_text("TIME") && has_text("TOTAL") && !has_text("PTS"));
    CHECK(!has_text("NOBODY HAS FINISHED YET."));
    CHECK(!has_text("ROOM AT 192.168.1.20 PORT 24302") && count_prefix("4 OF 8 RACERS") == 0);
    CHECK(find_text_at("THE RACE IS ON AT TRACK 3.", 58) != NULL);
    CHECK(find_text("SEND A MESSAGE")->y == 84 && find_text("LEAVE THE ROOM")->y == 94);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    item = find_text_at("TIME", 112);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    item = find_text_at("TOTAL", 112);
    CHECK(item != NULL && item->x == 300 && item->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(count_rects(6) == 0);
    /* In the table's order, then whoever is not in it. */
    CHECK(find_text("P01")->y == 122 && find_text("P03")->y == 132 && find_text("P00")->y == 142);
    CHECK(find_text("P02")->y == 152 && find_text("P04")->y == 162);
    CHECK(find_text("P01")->x == 34 && find_text("P01")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_GOLD);
    /* A place only for a racer who has one: the others are still out there. */
    item = find_text_at("1", 122);
    CHECK(item != NULL && item->x == 30 && item->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(find_text_at("2", 132) == NULL && find_text_at("3", 142) == NULL && find_text_at("4", 152) == NULL);
    item = find_text_at("1'23\"456", 122);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_WHITE);
    item = find_text_at("RETIRED", 132);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_RED);
    item = find_text_at("RACING", 142);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text_at("RACING", 152) != NULL);
    CHECK(count_prefix("RACING") == 2 && !has_text("DID NOT FINISH"));
    /* A watcher has their seat where a time would be. */
    item = find_text_at("WATCHING", 162);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    /* The series' points at the right margin, and nothing for the race until it is over. */
    item = find_text_at("30", 122);
    CHECK(item != NULL && item->x == 300 && item->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(find_text_at("0", 132) != NULL && find_text_at("7", 142) != NULL && find_text_at("12", 152) != NULL);
    CHECK(find_text_at("0", 162) != NULL);
    CHECK(count_prefix("+") == 0);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* The cursor goes through the table as it does through the roster, and another player's line opens
     * their page. */
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(count_rects(288) == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(has_text("MUTE THIS PLAYER"));
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 2);
    /* The player's own line does nothing. */
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);

    /* The race is over: every line has its place and what the race was worth. */
    sFake.phase = GDX_NET_ROOM_PHASE_RESULTS;
    fake_table(1, 4, kEndIds, kEndKinds, kEndTimes, kEndPoints);
    sFake.players[0].points = 11u;
    sFake.players[1].points = 55u;
    sFake.players[2].points = 30u;
    press(0);
    CHECK(find_text_at("THE RACE AT TRACK 3 IS OVER.", 58) != NULL);
    CHECK(find_text("P02")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    item = find_text_at("PTS", 112);
    CHECK(item != NULL && item->x == 262 && item->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(item != NULL && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text("P01")->y == 122 && find_text("P02")->y == 132 && find_text("P00")->y == 142);
    CHECK(find_text("P03")->y == 152 && find_text("P04")->y == 162);
    CHECK(find_text_at("1", 122) != NULL && find_text_at("2", 132) != NULL && find_text_at("3", 142) != NULL);
    CHECK(find_text_at("4", 152) != NULL && find_text_at("5", 162) == NULL);
    CHECK(find_text_at("1'23\"456", 122) != NULL && find_text_at("2'05\"007", 132) != NULL);
    item = find_text_at("DID NOT FINISH", 142);
    CHECK(item != NULL && item->x == 152 && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text_at("RETIRED", 152) != NULL && !has_text("RACING"));
    CHECK(find_text_at("WATCHING", 162) != NULL);
    item = find_text_at("+25", 122);
    CHECK(item != NULL && item->x == 262 && item->align == GDX_NET_ROOM_ALIGN_RIGHT);
    CHECK(find_text_at("+18", 132) != NULL && find_text_at("+4", 142) != NULL && find_text_at("+0", 152) != NULL);
    CHECK(count_prefix("+") == 4);
    CHECK(find_text_at("55", 122) != NULL && find_text_at("30", 132) != NULL && find_text_at("11", 142) != NULL);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* A racer who left keeps their line. It has no name and no total, and the cursor passes it by. */
    remove_player(0);
    press(0);
    item = find_text_at("LEFT THE ROOM", 142);
    CHECK(item != NULL && item->x == 34 && item->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(!has_text("P00"));
    CHECK(find_text_at("3", 142) != NULL && find_text_at("DID NOT FINISH", 142) != NULL);
    CHECK(find_text_at("+4", 142) != NULL && find_text_at("11", 142) == NULL);
    CHECK(gdx_net_room_screen_row() == 3);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(find_text("P03")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(gdx_net_room_screen_row() == 3);
    CHECK(sStopCalls == 0 && sMuteCalls == 0);

    /* The next track choice starts: the roster is back, also while the room still counts the same race. */
    sFake.phase = GDX_NET_ROOM_PHASE_PICK;
    press(0);
    CHECK(!has_text("TIME") && !has_text("TOTAL") && !has_text("LEFT THE ROOM"));
    CHECK(has_text("ROOM AT 192.168.1.20 PORT 24302"));
    CHECK(find_text("SEND A MESSAGE")->y == 108);

    /* The host's results: "BACK TO THE LOBBY" is the row above the table's. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RESULTS, 0, 5, 1);
    sFake.track = 3;
    fake_table(1, 4, kEndIds, kEndKinds, kEndTimes, kEndPoints);
    gdx_net_room_screen_open();
    CHECK(!has_text("YOUR ROOM ON PORT 24302"));
    CHECK(find_text("BACK TO THE LOBBY")->y == 84 && find_text("SEND A MESSAGE")->y == 94);
    CHECK(find_text("CLOSE THE ROOM")->y == 104);
    CHECK(find_text_at("TIME", 122) != NULL && find_text("P01")->y == 132 && find_text("P04")->y == 172);

    /* A full grid and both watch seats: nine lines at a time, none below the log's. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RESULTS, 5, GDX_NET_ROOM_PLAYERS, 2);
    for (i = 0; i < GDX_NET_ROOM_RACERS; i++) {
        ids[i] = GDX_NET_ROOM_RACERS - 1 - i;
        kinds[i] = GDX_NET_ROOM_RESULT_FINISHED;
        times[i] = 599999u;
        points[i] = 100;
        sFake.players[i].points = 1200u;
    }
    fake_table(1, GDX_NET_ROOM_RACERS, ids, kinds, times, points);
    fake_line(5, "Good race!");
    fake_line(6, "Good race!");
    gdx_net_room_screen_open();
    CHECK(count_prefix("9'59\"999") == 9);
    CHECK(find_text("P11")->y == 122 && find_text("P03")->y == 202 && !has_text("P02"));
    CHECK(count_rects(2) == 2);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    press_times(GDX_NET_ROOM_IN_DOWN, 15);
    CHECK(gdx_net_room_screen_row() == 15);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 15);
    CHECK(find_text("P13")->y == 202 && find_text("P13")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("P06")->y == 122 && !has_text("P07"));
    CHECK(find_text_at("NEXT RACE", 202) == NULL && find_text_at("WATCHING", 202) != NULL);
    CHECK(find_text_at("P05 - GOOD RACE ", 222) != NULL && find_text_at("P06 - GOOD RACE ", 232) != NULL);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
}

int main(void) {
    test_filter();
    test_track_name();
    test_wrap();
    test_timer();
    test_wanted();
    test_wanted_for_the_front_page();
    test_front();
    test_no_profile();
    test_host();
    test_port();
    test_rooms();
    test_found();
    test_address();
    test_joining_and_closed();
    test_room_racer();
    test_room_watcher();
    test_room_host();
    test_room_phases();
    test_room_track_row();
    test_room_full();
    test_cursor_follows();
    test_room_before_roster();
    test_room_host_controls();
    test_room_messages();
    test_room_log();
    test_room_mute();
    test_room_table();

    if (sFailures != 0) {
        printf("gdx_net_room_screen_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_room_screen_tests: %d checks passed\n", sChecks);
    return 0;
}
