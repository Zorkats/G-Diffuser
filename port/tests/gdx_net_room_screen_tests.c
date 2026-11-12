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
#include "../gdx_net_track_save.h"

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
static int sCustomCalls;
static int sCustomValue;
static int sCpuFillCalls;
static int sCpuFillValue;
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

static int sHostTypeArg;    /* the type the last gdx_lan_room_host asked for */
static int sFakeDirectory;  /* what gdx_lan_room_directory_ok returns */
static int sFakeNotice;     /* what gdx_lan_room_notice_seen returns */
static int sNoticeAccepts;
static int sJoinCodeCalls;
static int sJoinCodeAnswer;
static int sJoinCodeWatch;
static char sJoinCodeText[16];
static int sAskCalls;
static int sAskLast;
static int sAskAnswer;
static int sAskWait;
static int sJoinListedCalls;
static int sJoinListedIndex;
static int sJoinListedWatch;

int gdx_lan_room_host(int type, char* error, int size) {
    sHostCalls++;
    sHostTypeArg = type;
    if (!sHostAnswer) {
        snprintf(error, (size_t) size, "Port 24302 is in use by another program.");
        return 0;
    }
    sFake.mode = GDX_NET_ROOM_MODE_HOSTING;
    return 1;
}

int gdx_lan_room_directory_ok(void) {
    return sFakeDirectory;
}

int gdx_lan_room_join_code(const char* code, int watch) {
    sJoinCodeCalls++;
    snprintf(sJoinCodeText, sizeof(sJoinCodeText), "%s", code);
    sJoinCodeWatch = watch;
    if (sJoinCodeAnswer) {
        sFake.mode = GDX_NET_ROOM_MODE_JOINING;
        snprintf(sFake.joinCode, sizeof(sFake.joinCode), "%s", code);
    }
    return sJoinCodeAnswer;
}

int gdx_lan_room_ask(int ask, int* waitSeconds) {
    sAskCalls++;
    sAskLast = ask;
    *waitSeconds = 0;
    if (!sAskAnswer) {
        *waitSeconds = sAskWait;
        return 0;
    }
    sFake.dirStatus = GDX_NET_ROOM_DIR_ASKING;
    sFake.dirAsk = ask;
    return 1;
}

int gdx_lan_room_join_listed(int index, int watch) {
    sJoinListedCalls++;
    sJoinListedIndex = index;
    sJoinListedWatch = watch;
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    return 1;
}

int gdx_lan_room_notice_seen(void) {
    return sFakeNotice;
}

void gdx_lan_room_notice_accept(void) {
    sNoticeAccepts++;
    sFakeNotice = 1;
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

static int sHandingOver;          /* what gdx_lan_room_handing_over returns */
static const char* sHandingOverTo = "";

int gdx_lan_room_handing_over(char* next, int size) {
    if (sHandingOver && next != NULL && size > 0) {
        snprintf(next, (size_t) size, "%s", sHandingOverTo);
    }
    return sHandingOver;
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

void gdx_lan_room_custom_tracks(int on) {
    sCustomCalls++;
    sCustomValue = on;
}

void gdx_lan_room_cpu_fill(int on) {
    sCpuFillCalls++;
    sCpuFillValue = on;
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

void gdx_lan_room_vote_custom(const unsigned char* id, const char* title) {
    (void) id;
    (void) title;
    sVoteCalls++;
}

void gdx_lan_room_pick_custom(const unsigned char* id, const char* title) {
    (void) id;
    (void) title;
    sPickCalls++;
}

/* This copy's Edit Cup holds nothing a room can race: the course select's tests cover it. */
int gdx_net_own_track(int slot, unsigned char* id, char* title, int titleSize) {
    (void) slot;
    (void) id;
    (void) title;
    (void) titleSize;
    return 0;
}

void gdx_net_own_tracks_forget(void) {
}

/* "Save to my tracks" (gdx_net_track_save.h): its own tests cover the disk; the screen only shows its state
 * and passes the press on. */
static int sSaveState;
static int sSaveBeginAnswer;
static int sSaveBeginCalls;
static int sSavePollCalls;

int gdx_net_track_save_state(const GdxNetRoomView* view) {
    (void) view;
    return sSaveState;
}

int gdx_net_track_save_begin(const GdxNetRoomView* view) {
    (void) view;
    sSaveBeginCalls++;
    sSaveState = sSaveBeginAnswer;
    return sSaveState;
}

void gdx_net_track_save_poll(void) {
    sSavePollCalls++;
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

/* ---- friends (O7.4) and the ban (O7.3) ---- */

static char sFakeMyCode[GDX_NET_ROOM_FRIEND_CODE_BYTES];
static char sFakeFriendNames[GDX_NET_ROOM_FRIENDS_MAX][GDX_NET_ROOM_NAME_BYTES];
static char sFakeFriendCodes[GDX_NET_ROOM_FRIENDS_MAX][GDX_NET_ROOM_FRIEND_CODE_CHARS + 1];
static int sFakeFriendCount;
static int sFakeAddAnswer; /* -1: the fake adds the code; otherwise what gdx_lan_room_friend_add_code returns */
static int sAddCodeCalls;
static char sAddCodeText[96];
static int sRemoveCalls;
static int sRemoveIndex;
static int sAskFriendCalls;
static int sAskFriendIndex;
static char sFakeMetNames[GDX_NET_ROOM_MET_MAX][GDX_NET_ROOM_NAME_BYTES];
static int sFakeMetListed[GDX_NET_ROOM_MET_MAX];
static int sFakeMetCount;
static int sMetAddCalls;
static int sMetAddIndex;
static int sBanCalls;
static int sBanId;
static char sClipboard[160];
static int sClipboardOk;
static int sClipboardSets;

static void fake_friend(const char* name, char fill) {
    memset(sFakeFriendCodes[sFakeFriendCount], fill, GDX_NET_ROOM_FRIEND_CODE_CHARS);
    sFakeFriendCodes[sFakeFriendCount][GDX_NET_ROOM_FRIEND_CODE_CHARS] = '\0';
    snprintf(sFakeFriendNames[sFakeFriendCount], GDX_NET_ROOM_NAME_BYTES, "%s", name);
    sFakeFriendCount++;
}

int gdx_lan_room_my_friend_code(char* out, int size) {
    snprintf(out, (size_t) size, "%s", sFakeMyCode);
    return sFakeMyCode[0] != '\0';
}

int gdx_lan_room_friend_count(void) {
    return sFakeFriendCount;
}

int gdx_lan_room_friend_name(int index, char* out, int size) {
    if (index < 0 || index >= sFakeFriendCount) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, (size_t) size, "%s", sFakeFriendNames[index]);
    return 1;
}

/* The display form: groups of four with a space between. */
int gdx_lan_room_friend_code(int index, char* out, int size) {
    int at = 0;
    int i;

    if (index < 0 || index >= sFakeFriendCount || size < GDX_NET_ROOM_FRIEND_CODE_BYTES) {
        out[0] = '\0';
        return 0;
    }
    for (i = 0; i < GDX_NET_ROOM_FRIEND_CODE_CHARS; i++) {
        if (i > 0 && i % 4 == 0) {
            out[at++] = ' ';
        }
        out[at++] = sFakeFriendCodes[index][i];
    }
    out[at] = '\0';
    return 1;
}

int gdx_lan_room_friend_add_code(const char* code) {
    sAddCodeCalls++;
    snprintf(sAddCodeText, sizeof(sAddCodeText), "%s", code);
    if (sFakeAddAnswer >= 0) {
        return sFakeAddAnswer;
    }
    if (strlen(code) != GDX_NET_ROOM_FRIEND_CODE_CHARS) {
        return GDX_NET_ROOM_FRIEND_BAD_CODE;
    }
    snprintf(sFakeFriendCodes[sFakeFriendCount], sizeof(sFakeFriendCodes[0]), "%s", code);
    snprintf(sFakeFriendNames[sFakeFriendCount], GDX_NET_ROOM_NAME_BYTES, "Friend %d", sFakeFriendCount + 1);
    sFakeFriendCount++;
    return GDX_NET_ROOM_FRIEND_ADDED;
}

int gdx_lan_room_friend_remove(int index) {
    int i;

    sRemoveCalls++;
    sRemoveIndex = index;
    if (index < 0 || index >= sFakeFriendCount) {
        return 0;
    }
    for (i = index; i + 1 < sFakeFriendCount; i++) {
        memcpy(sFakeFriendNames[i], sFakeFriendNames[i + 1], sizeof(sFakeFriendNames[0]));
        memcpy(sFakeFriendCodes[i], sFakeFriendCodes[i + 1], sizeof(sFakeFriendCodes[0]));
    }
    sFakeFriendCount--;
    return 1;
}

int gdx_lan_room_ask_friend(int index, int* waitSeconds) {
    sAskFriendCalls++;
    sAskFriendIndex = index;
    *waitSeconds = 0;
    if (!sAskAnswer) {
        *waitSeconds = sAskWait;
        return 0;
    }
    sFake.dirStatus = GDX_NET_ROOM_DIR_ASKING;
    sFake.dirAsk = GDX_NET_ROOM_ASK_FRIEND;
    return 1;
}

int gdx_lan_room_met_count(void) {
    return sFakeMetCount;
}

int gdx_lan_room_met_name(int index, char* out, int size) {
    if (index < 0 || index >= sFakeMetCount) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, (size_t) size, "%s", sFakeMetNames[index]);
    return 1;
}

int gdx_lan_room_met_listed(int index) {
    return index >= 0 && index < sFakeMetCount && sFakeMetListed[index];
}

int gdx_lan_room_met_add(int index) {
    sMetAddCalls++;
    sMetAddIndex = index;
    if (index < 0 || index >= sFakeMetCount) {
        return GDX_NET_ROOM_FRIEND_GONE;
    }
    if (sFakeAddAnswer >= 0) {
        return sFakeAddAnswer;
    }
    sFakeMetListed[index] = 1;
    return GDX_NET_ROOM_FRIEND_ADDED;
}

void gdx_lan_room_ban(int id) {
    sBanCalls++;
    sBanId = id;
}

int gdx_lan_room_clipboard_set(const char* text) {
    if (!sClipboardOk) {
        return 0;
    }
    sClipboardSets++;
    snprintf(sClipboard, sizeof(sClipboard), "%s", text);
    return 1;
}

int gdx_lan_room_clipboard_get(char* out, int size) {
    snprintf(out, (size_t) size, "%s", sClipboard);
    return out[0] != '\0';
}

static void reset_friends(void) {
    sFakeMyCode[0] = '\0';
    sFakeFriendCount = 0;
    sFakeAddAnswer = -1;
    sAddCodeCalls = 0;
    sAddCodeText[0] = '\0';
    sRemoveCalls = 0;
    sRemoveIndex = -1;
    sAskFriendCalls = 0;
    sAskFriendIndex = -1;
    sFakeMetCount = 0;
    memset(sFakeMetListed, 0, sizeof(sFakeMetListed));
    sMetAddCalls = 0;
    sMetAddIndex = -1;
    sBanCalls = 0;
    sBanId = -1;
    sClipboard[0] = '\0';
    sClipboardOk = 1;
    sClipboardSets = 0;
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
    sHostTypeArg = -1;
    sFakeDirectory = 0;
    sFakeNotice = 1; /* the notice has its own test */
    reset_friends();
    sNoticeAccepts = 0;
    sJoinCodeCalls = 0;
    sJoinCodeAnswer = 1;
    sJoinCodeWatch = -1;
    sJoinCodeText[0] = '\0';
    sAskCalls = 0;
    sAskLast = -1;
    sAskAnswer = 1;
    sAskWait = 0;
    sJoinListedCalls = 0;
    sJoinListedIndex = -2;
    sJoinListedWatch = -1;
    sBrowseCalls = 0;
    sJoinCalls = 0;
    sJoinIp = 0;
    sJoinPort = 0;
    sJoinWatch = -1;
    sStopCalls = 0;
    sDismissCalls = 0;
    sHandingOver = 0;
    sHandingOverTo = "";
    sReadyCalls = 0;
    sReadyValue = -1;
    sRaceNextCalls = 0;
    sRaceNextValue = -1;
    sStartCalls = 0;
    sCapCalls = 0;
    sCapValue = -1;
    sCustomCalls = 0;
    sCustomValue = -1;
    sCpuFillCalls = 0;
    sCpuFillValue = -1;
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
    sSaveState = 0;
    sSaveBeginAnswer = 0;
    sSaveBeginCalls = 0;
    sSavePollCalls = 0;
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
    sFake.cpuFill = 1; /* the room's default (D2) */
    sFake.successor = GDX_NET_ROOM_NOBODY;
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
    CHECK(has_text("WHAT G-NET SENDS"));
    CHECK(has_text("BACK"));
    CHECK(field_is("PORT", "24302"));
    CHECK(find_text("HOST A ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("FIND ROOMS")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(count_rects(288) == 1); /* the cursor's bar */
    /* No directory: the rooms on the internet are not offered, and the page says why in two lines. */
    CHECK(!has_text("QUICK RACE") && !has_text("OPEN ROOMS") && !has_text("JOIN BY CODE"));
    CHECK(find_text_at("NO ROOM DIRECTORY IS SET. ROOMS STAY ON", 212) != NULL);
    CHECK(find_text_at("THIS NETWORK.", 222) != NULL);
    CHECK(count_colour(GDX_NET_ROOM_COLOUR_DIM) == 2);
    CHECK(!has_text("LEFT AND RIGHT CHANGE THE KIND OF ROOM."));
    CHECK(sHostCalls == 0 && sBrowseCalls == 0 && sJoinCalls == 0);

    /* The cursor stops at both ends. */
    CHECK(press(GDX_NET_ROOM_IN_UP) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE); /* a sound is taken once */
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(gdx_net_room_screen_row() == 6);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 6);
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

/* The host left its room in a race (D28.5): the front page says the room runs on until the results, who gets
 * it, and that closing the game ends the race. It goes with the room, and a host error comes first. */
static void test_front_while_the_room_is_handed_over(void) {
    reset();
    sHandingOver = 1;
    sHandingOverTo = "Ana";
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(find_text_at("YOUR ROOM FINISHES THIS RACE WITHOUT", 182) != NULL);
    CHECK(find_text_at("YOU. THEN ANA HOSTS IT. CLOSING THE GAME", 192) != NULL);
    CHECK(find_text_at("NOW ENDS THE RACE.", 202) != NULL);
    CHECK(find_text("NOW ENDS THE RACE.")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    CHECK(count_colour(GDX_NET_ROOM_COLOUR_DIM) == 2); /* the directory line is all there */

    /* The longest name still fits three lines. */
    sHandingOverTo = "ABCDEFGHIJKLMNOP";
    CHECK(find_text_at("YOU. THEN ABCDEFGHIJKLMNOP HOSTS IT.", 192) != NULL);
    CHECK(find_text_at("CLOSING THE GAME NOW ENDS THE RACE.", 202) != NULL);

    sHandingOverTo = "";
    CHECK(find_text_at("YOU. THEN THE NEXT PLAYER HOSTS IT.", 192) != NULL);

    sHostAnswer = 0;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(!has_text("YOUR ROOM FINISHES THIS RACE WITHOUT"));

    reset();
    gdx_net_room_screen_open();
    CHECK(!has_text("YOUR ROOM FINISHES THIS RACE WITHOUT"));
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
    CHECK(sHostCalls == 1 && sHostTypeArg == GDX_NET_ROOM_TYPE_LAN);
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

    /* Every listed room speaks this copy's protocol (decision 25): no row is dimmed or marked as another build. */
    reset();
    fake_search(8);
    gdx_net_room_screen_open();
    CHECK(!has_text("OTHER BUILD"));
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(find_text("DEN00")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    press(GDX_NET_ROOM_IN_UP);

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

/* An IPv6 host: groups split by spaces, ".." for the zero groups left out, on two lines when long; the room's
 * one-line header names only the family and the port. */
static void test_joining_ipv6(void) {
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    snprintf(sFake.hostText, sizeof(sFake.hostText), "[2001:db8::1]:24302");
    gdx_net_room_screen_open();
    CHECK(has_text("JOINING") && has_text("2001 DB8 .. 1") && has_text("PORT 24302"));
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    snprintf(sFake.hostText, sizeof(sFake.hostText), "[2001:db8:14fc:5:114e:c00:abcd:ef01]:24302");
    gdx_net_room_screen_open();
    CHECK(has_text("2001 DB8 14FC 5 114E C00 ABCD EF01"));
    CHECK(has_text("PORT 24302"));
    reset();
    sFake.mode = GDX_NET_ROOM_MODE_JOINING;
    snprintf(sFake.hostText, sizeof(sFake.hostText), "[::1]:9");
    gdx_net_room_screen_open();
    CHECK(has_text(".. 1") && has_text("PORT 9"));
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    snprintf(sFake.hostText, sizeof(sFake.hostText), "[2001:db8::1]:24302");
    gdx_net_room_screen_open();
    CHECK(has_text("IPV6 ROOM PORT 24302"));
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
    CHECK(has_text("CUSTOM TRACKS") && find_text("OFF") != NULL && find_text("OFF")->y == find_text("CUSTOM TRACKS")->y);
    CHECK(find_text("SEND A MESSAGE") != NULL && find_text("SEND A MESSAGE")->y == 158);
    CHECK(find_text("CLOSE THE ROOM")->y == 168);
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

    /* Custom tracks: A flips the switch, right turns it on and left turns it off. */
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 3);
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(sCustomCalls == 0); /* already off */
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCustomCalls == 1 && sCustomValue == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    sFake.customTracks = 1;
    CHECK(find_text("ON") != NULL && find_text("ON")->y == find_text("CUSTOM TRACKS")->y);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCustomCalls == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sCustomCalls == 2 && sCustomValue == 0);
    sFake.customTracks = 0;
    CHECK(sCapCalls == 2);

    /* CPU racers: on by default; A flips it, right turns it on and left turns it off. */
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 4);
    CHECK(find_text("CPU RACERS") != NULL && find_text("CPU RACERS")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("ON") != NULL && find_text("ON")->y == find_text("CPU RACERS")->y);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCpuFillCalls == 0); /* already on */
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(sCpuFillCalls == 1 && sCpuFillValue == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    sFake.cpuFill = 0;
    press(0);
    CHECK(find_text_at("OFF", find_text("CPU RACERS")->y) != NULL); /* CUSTOM TRACKS draws OFF too */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sCpuFillCalls == 2 && sCpuFillValue == 1);
    sFake.cpuFill = 1;
    CHECK(sCustomCalls == 2 && sCapCalls == 2);

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
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    CHECK(find_text("P00")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(gdx_net_room_screen_row() == 3); /* on BACK: removing takes a move and a press */
    CHECK(find_text("P01") != NULL && find_text("P01")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(has_text("MUTE THIS PLAYER") && has_text("REMOVE FROM THE ROOM") && has_text("BACK"));
    CHECK(find_text("MUTE THIS PLAYER")->y == 80 && find_text("REMOVE FROM THE ROOM")->y == 92);
    CHECK(find_text("BAN FROM THE ROOM")->y == 104 && find_text("BACK")->y == 116);
    CHECK(sKickCalls == 0);
    /* BACK and B return to that player's row. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 8);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    CHECK(gdx_net_room_screen_row() == 8);
    CHECK(sKickCalls == 0 && sStopCalls == 0);
    /* Remove. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_UP, 2);
    CHECK(gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sKickCalls == 1 && sKickId == 1);
    CHECK(sMuteCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);
    /* The player leaves while the host looks at them: back to the room. */
    press_times(GDX_NET_ROOM_IN_DOWN, 9);
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
    CHECK(!has_text("CUSTOM TRACKS") && !has_text("CPU RACERS"));
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("CLOSE THE ROOM") != NULL && find_text("CLOSE THE ROOM")->y == 118);
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(sCapCalls == 0);
}

static void test_room_phases(void) {
    static const char* const kTitles[10] = { "LOBBY",   "TRACK VOTE", "VOTE RESULT", "TRACK PICK", "MACHINE SELECT",
                                             "LOADING", "RACE START", "RACE",        "RESULTS",    "GETTING THE TRACK" };
    int phase;

    for (phase = GDX_NET_ROOM_PHASE_LOBBY; phase <= GDX_NET_ROOM_PHASE_ASSETS; phase++) {
        reset();
        fake_room(GDX_NET_ROOM_MODE_JOINED, phase, 2, 5, 1);
        gdx_net_room_screen_open();
        CHECK(find_text(kTitles[phase]) != NULL && find_text(kTitles[phase])->font == GDX_NET_ROOM_FONT_TITLE);
        CHECK(has_text("READY UP") == (phase == GDX_NET_ROOM_PHASE_LOBBY));
        CHECK(has_text("LEAVE THE ROOM"));
        CHECK(has_text("WATCHING"));
    }

    /* Getting a custom track: the racers download it, the others sit out. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_ASSETS, 2, 5, 0);
    sFake.track = 7;
    sFake.players[1].inRace = 1;
    gdx_net_room_screen_open();
    CHECK(has_text("THE RACERS GET IT FROM THE HOST."));
    CHECK(has_text("DOWNLOADING") && has_text("SITS OUT"));

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
    /* Seven rows of the host's, then three players at a time: the list ends above the two lines of the log. */
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 3);
    CHECK(has_text("WWWWWWWWWWWWWW00") && has_text("WWWWWWWWWWWWWW02") && !has_text("WWWWWWWWWWWWWW03"));
    CHECK(find_text("WWWWWWWWWWWWWW00")->y == 186 && find_text("WWWWWWWWWWWWWW02")->y == 206);
    CHECK(count_rects(2) == 2);
    CHECK(count_rects(6) == 3);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    press_times(GDX_NET_ROOM_IN_DOWN, 20);
    CHECK(gdx_net_room_screen_row() == 20);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 20);
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 3);
    CHECK(has_text("WWWWWWWWWWWWWW13") && has_text("WWWWWWWWWWWWWW11") && !has_text("WWWWWWWWWWWWWW10"));
    CHECK(find_text("WWWWWWWWWWWWWW13")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(has_text("113") && has_text("913 MS"));
    CHECK(count_rects(288) == 1);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    /* Every row of a player stays above the log's lines. */
    CHECK(find_text("WWWWWWWWWWWWWW13")->y == 206);
    /* The list shortens under the cursor: the cursor ends on its last row. */
    sFake.playerCount = 4;
    press(0);
    CHECK(gdx_net_room_screen_row() == 10);
    CHECK(count_prefix("WWWWWWWWWWWWWW") == 3);
    CHECK(count_rects(2) == 2);

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
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MESSAGES);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMessageCalls == 1 && sMessageIndex == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM && gdx_net_room_screen_row() == 5);
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
    /* The CPU RACERS row (D28.1) takes one line of the list. */
    CHECK(find_text("P02") != NULL && find_text("P02")->y == 206 && !has_text("P03"));
    CHECK(find_text_at("P05 - GOOD LUCK ", 222) != NULL && find_text_at("P06 - GOOD LUCK ", 232) != NULL);
    CHECK(count_rects(6) == 5); /* three player rows and the log's two */
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
    press_times(GDX_NET_ROOM_IN_DOWN, 8);
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER && gdx_net_room_screen_row() == 3);
    CHECK(find_text_at("MUTING HIDES MESSAGES FROM THIS PLAYER", 140) != NULL);
    CHECK(find_text_at("ON YOUR SCREEN ONLY.", 150) != NULL);
    press_times(GDX_NET_ROOM_IN_UP, 3);
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

/* Moves the cursor down onto text, at most rows times. */
static int cursor_to(const char* text, int rows) {
    while (rows-- > 0) {
        const GdxNetRoomDrawItem* item = find_text(text);

        if (item != NULL && item->colour == GDX_NET_ROOM_COLOUR_CURSOR) {
            return 1;
        }
        press(GDX_NET_ROOM_IN_DOWN);
    }
    return 0;
}

/* The results row that keeps a custom track (plan decision 14): there only when the save module says so, the
 * row above "SEND A MESSAGE", pressable while it offers to save or to try again. */
static void test_room_save_track(void) {
    static const struct {
        int state;
        const char* text;
        int pressable;
    } kRows[] = {
        { GDX_NET_TRACK_SAVE_READY, "SAVE TO MY TRACKS", 1 },
        { GDX_NET_TRACK_SAVE_SAVING, "SAVING THE TRACK", 0 },
        { GDX_NET_TRACK_SAVE_SAVED, "SAVED IN YOUR EDIT CUP", 0 },
        { GDX_NET_TRACK_SAVE_FULL, "YOUR EDIT CUP IS FULL", 0 },
        { GDX_NET_TRACK_SAVE_FAILED, "TRY SAVING AGAIN", 1 },
        { GDX_NET_TRACK_SAVE_OWN, "ALREADY IN YOUR EDIT CUP", 0 },
    };
    int i;

    /* No row while the module answers NONE, and the screen still collects a finished save every frame. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RESULTS, 2, 4, 0);
    sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
    gdx_net_room_screen_open();
    press(0);
    for (i = 0; i < (int) (sizeof(kRows) / sizeof(kRows[0])); i++) {
        CHECK(!has_text(kRows[i].text));
    }
    CHECK(sSavePollCalls >= 1);

    /* Each state's words, in the row above "SEND A MESSAGE"; dim when pressing it does nothing. */
    for (i = 0; i < (int) (sizeof(kRows) / sizeof(kRows[0])); i++) {
        const GdxNetRoomDrawItem* row;
        const GdxNetRoomDrawItem* message;

        reset();
        fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RESULTS, 2, 4, 0);
        sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
        sSaveState = kRows[i].state;
        gdx_net_room_screen_open();
        press_times(GDX_NET_ROOM_IN_DOWN, 4);
        row = find_text(kRows[i].text);
        message = find_text("SEND A MESSAGE");
        CHECK(row != NULL && message != NULL);
        if (row != NULL && message != NULL) {
            CHECK(row->y + 10 == message->y);
            if (!kRows[i].pressable) {
                CHECK(row->colour == GDX_NET_ROOM_COLOUR_DIM);
            }
        }
    }

    /* A press that queues the save: the row turns to "SAVING THE TRACK" and the cursor leaves it. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_RESULTS, 2, 4, 0);
    sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
    sSaveState = GDX_NET_TRACK_SAVE_READY;
    sSaveBeginAnswer = GDX_NET_TRACK_SAVE_SAVING;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_UP);
    CHECK(cursor_to("SAVE TO MY TRACKS", 6));
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sSaveBeginCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() != GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(has_text("SAVING THE TRACK") && !has_text("SAVE TO MY TRACKS"));
    CHECK(find_text("SAVING THE TRACK")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text("SEND A MESSAGE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    /* The disk finishes: the words follow, and pressing a done row asks nothing more of the module. */
    sSaveState = GDX_NET_TRACK_SAVE_SAVED;
    press(0);
    CHECK(has_text("SAVED IN YOUR EDIT CUP"));
    CHECK(sStopCalls == 0 && sMessageCalls == 0);

    /* A refused press (the cup is full): the refuse sound, and the row says why. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_RESULTS, 0, 4, 0);
    sFake.track = GDX_NET_ROOM_TRACK_CUSTOM;
    sSaveState = GDX_NET_TRACK_SAVE_FAILED;
    sSaveBeginAnswer = GDX_NET_TRACK_SAVE_FULL;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_UP);
    CHECK(cursor_to("TRY SAVING AGAIN", 6));
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sSaveBeginCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(has_text("YOUR EDIT CUP IS FULL") && !has_text("TRY SAVING AGAIN"));
    CHECK(sEndSeriesCalls == 0 && sStopCalls == 0);
    /* The host's own row for the series is still there. */
    CHECK(has_text("BACK TO THE LOBBY"));
}

/* ---- the rooms on the internet (O5.12) ---- */

/* The directory answered the Open rooms: count rooms "AAA-000" on, two players each, six seats free but the
 * last, which is full. */
static void fake_open_answer(int count) {
    int i;

    sFake.dirStatus = GDX_NET_ROOM_DIR_READY;
    sFake.dirAsk = GDX_NET_ROOM_ASK_OPEN;
    sFake.dirSerial++;
    sFake.listedCount = count;
    for (i = 0; i < count; i++) {
        snprintf(sFake.listed[i].code, sizeof(sFake.listed[i].code), "AAA-00%d", i);
        sFake.listed[i].type = GDX_NET_ROOM_TYPE_OPEN;
        sFake.listed[i].humans = 2;
        sFake.listed[i].freeSeats = (unsigned char) (i == count - 1 ? 0 : 6);
        sFake.listed[i].reachable = 1;
    }
}

static void fake_quick_answer(int found) {
    sFake.dirStatus = GDX_NET_ROOM_DIR_READY;
    sFake.dirAsk = GDX_NET_ROOM_ASK_QUICK;
    sFake.dirSerial++;
    sFake.quickFound = found;
    snprintf(sFake.quick.code, sizeof(sFake.quick.code), "QQQ-111");
}

/* The first-use notice (ODD 10): before anything can be sent, and again from its row. */
static void test_notice(void) {
    const GdxNetRoomDrawItem* last;

    reset();
    sFakeNotice = 0;
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_NOTICE);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(has_text("WHAT G-NET SENDS"));
    CHECK(has_text("A ROOM GETS YOUR PROFILE NAME AND COLOUR"));
    CHECK(has_text("MESSAGES. ITS HOST CAN BAN YOUR FRIEND"));
    CHECK(has_text("CODE. THE HOST SEES YOUR IP ADDRESS. IF"));
    CHECK(has_text("DIRECTORY WITH THE ADDRESS AND FRIEND"));
    CHECK(has_text("THE PUBLIC HALF OF YOUR PROFILE KEY."));
    last = find_text("SEARCH OR JOIN.");
    CHECK(last != NULL && last->y <= 58 + 13 * 10); /* fourteen lines at most */
    CHECK(find_text("I UNDERSTAND")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(last != NULL && find_text("I UNDERSTAND")->y > last->y);
    CHECK(has_text("BACK"));
    CHECK(!has_text("HOST A ROOM") && !has_text("FIND ROOMS"));
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* B and BACK leave the screen: nothing on it may be used yet. */
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_LEAVE);
    CHECK(sNoticeAccepts == 0);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_NOTICE);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_LEAVE);
    CHECK(sNoticeAccepts == 0);

    /* Read: the front page, for good. */
    gdx_net_room_screen_open();
    CHECK(press(GDX_NET_ROOM_IN_CONFIRM) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(sNoticeAccepts == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 0);
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(sHostCalls == 0 && sBrowseCalls == 0 && sJoinCalls == 0 && sAskCalls == 0);

    /* Its row opens it again, with nothing to accept, and B goes back to that row. */
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(find_text("WHAT G-NET SENDS")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_NOTICE);
    CHECK(!has_text("I UNDERSTAND"));
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(press(GDX_NET_ROOM_IN_BACK) == GDX_NET_ROOM_SCREEN_STAY);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 5);
    CHECK(sNoticeAccepts == 1);

    /* No profile: nothing can be sent, so the page that asks for one comes first. */
    reset();
    sFakeNotice = 0;
    sFakeProfile = 0;
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
}

/* With a directory: the room types, and the rows of the rooms on the internet. */
static void test_front_with_a_directory(void) {
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(find_text("HOST A ROOM ON THIS NETWORK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    CHECK(find_text("QUICK RACE")->y == 70);
    CHECK(find_text("OPEN ROOMS")->y == 82);
    CHECK(find_text("JOIN BY CODE")->y == 94);
    CHECK(find_text("FIND ROOMS")->y == 106);
    CHECK(find_text("FRIENDS")->y == 142);
    CHECK(find_text("BACK")->y == 166);
    CHECK(!has_text("NO ROOM DIRECTORY IS SET. ROOMS STAY ON"));
    CHECK(has_text("LEFT AND RIGHT CHANGE THE KIND OF ROOM."));

    /* Left and right go round the four types. */
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_MOVE);
    CHECK(has_text("HOST A ROOM BY CODE"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(has_text("HOST AN OPEN ROOM"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(has_text("HOST A ROOM FOR FRIENDS"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(has_text("HOST A ROOM ON THIS NETWORK"));
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(has_text("HOST A ROOM FOR FRIENDS"));
    press(GDX_NET_ROOM_IN_LEFT);
    CHECK(has_text("HOST AN OPEN ROOM"));
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(sHostCalls == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sHostCalls == 1 && sHostTypeArg == GDX_NET_ROOM_TYPE_OPEN);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);

    /* The type is kept between visits. The directory goes: the type goes back to this network, and left and
     * right do nothing. */
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    CHECK(has_text("HOST AN OPEN ROOM"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(has_text("HOST A ROOM FOR FRIENDS"));
    sFakeDirectory = 0;
    press(0);
    CHECK(has_text("HOST A ROOM"));
    press(GDX_NET_ROOM_IN_RIGHT);
    CHECK(has_text("HOST A ROOM"));
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_NONE);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sHostCalls == 1 && sHostTypeArg == GDX_NET_ROOM_TYPE_LAN);
}

static void test_join_by_code(void) {
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    CHECK(find_text("JOIN BY CODE")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_CODE);
    CHECK(gdx_net_room_screen_row() == 0);
    CHECK(field_is("CODE", "000-000"));
    CHECK(has_text("JOIN TO RACE") && has_text("JOIN TO WATCH") && has_text("BACK"));
    CHECK(has_text("ASK THE HOST FOR THE CODE OF THEIR ROOM."));

    /* Up and down go round the 32 letters and digits of a code. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 1);
    CHECK(has_text("LEFT AND RIGHT PICK A LETTER. UP AND"));
    press(GDX_NET_ROOM_IN_UP);
    CHECK(field_is("CODE", "100-000"));
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(field_is("CODE", "Z00-000"));
    press(GDX_NET_ROOM_IN_RIGHT);
    press_times(GDX_NET_ROOM_IN_UP, 10);
    CHECK(field_is("CODE", "ZA0-000"));
    press_times(GDX_NET_ROOM_IN_RIGHT, 9);
    CHECK(gdx_net_room_screen_editing() == 6);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 0);
    CHECK(field_is("CODE", "ZA0-001"));
    CHECK(sJoinCodeCalls == 0);

    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCodeCalls == 1 && strcmp(sJoinCodeText, "ZA0-001") == 0 && sJoinCodeWatch == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);
    CHECK(has_text("JOINING ROOM ZA0-001"));
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(sStopCalls == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* A code the session refuses: the page stays and the refusal sounds. The code is kept between visits. */
    sJoinCodeAnswer = 0;
    sFake.joinCode[0] = '\0';
    press_times(GDX_NET_ROOM_IN_DOWN, 3);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(field_is("CODE", "ZA0-001"));
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinCodeCalls == 2 && sJoinCodeWatch == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_CODE);

    /* B in the field gives the change up; B on the page goes back to its row. */
    press_times(GDX_NET_ROOM_IN_UP, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(field_is("CODE", "0A0-001")); /* Z goes round to 0 */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(field_is("CODE", "ZA0-001"));
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 3);
}

static void test_open_rooms(void) {
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskCalls == 1 && sAskLast == GDX_NET_ROOM_ASK_OPEN);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_OPEN);
    CHECK(has_text("OPEN ROOMS"));
    CHECK(has_text("ASKING THE ROOM DIRECTORY."));
    CHECK(find_text("REFRESH")->colour == GDX_NET_ROOM_COLOUR_DIM); /* one request at a time */
    CHECK(find_text("BACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR);

    /* The answer: a row a room, its code, its players and its seats. */
    fake_open_answer(3);
    press(0);
    CHECK(has_text("3 OPEN ROOMS"));
    CHECK(has_text("AAA-000") && has_text("AAA-002"));
    CHECK(count_prefix("2 PLAYERS") == 3);
    CHECK(count_prefix("6 SEATS FREE") == 2);
    CHECK(has_text("FULL - WATCH"));
    CHECK(gdx_net_room_screen_row() == 0); /* the answer takes the cursor to its first room */
    CHECK(find_text("AAA-000")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    CHECK(gdx_net_room_screen_row() == 2);
    CHECK(find_text("AAA-002")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinListedCalls == 1 && sJoinListedIndex == 2 && sJoinListedWatch == 1); /* full: a watch seat */
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);

    /* A room with seats is joined to race. */
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    fake_open_answer(2);
    press(0);
    press_times(GDX_NET_ROOM_IN_UP, 9);
    CHECK(gdx_net_room_screen_row() == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinListedCalls == 1 && sJoinListedIndex == 0 && sJoinListedWatch == 0);

    /* None, then a refresh inside the cooldown: nothing is sent and the wait is said. */
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    fake_open_answer(0);
    press(0);
    CHECK(has_text("NO OPEN ROOM RIGHT NOW."));
    press(GDX_NET_ROOM_IN_UP);
    CHECK(find_text("REFRESH")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    sAskAnswer = 0;
    sAskWait = 12;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskCalls == 2);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(has_text("ASK AGAIN IN 12 SECONDS."));
    sAskAnswer = 1;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskCalls == 3 && !has_text("ASK AGAIN IN 12 SECONDS."));

    /* The directory fails: its reason, in red. */
    sFake.dirStatus = GDX_NET_ROOM_DIR_FAILED;
    sFake.dirSerial++;
    snprintf(sFake.dirText, sizeof(sFake.dirText), "The room directory did not answer");
    press(0);
    CHECK(find_text("THE ROOM DIRECTORY DID NOT ANSWER") != NULL &&
          find_text("THE ROOM DIRECTORY DID NOT ANSWER")->colour == GDX_NET_ROOM_COLOUR_RED);

    /* B goes back to the row. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 2);
}

static void test_quick_race(void) {
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskCalls == 1 && sAskLast == GDX_NET_ROOM_ASK_QUICK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_QUICK);
    CHECK(has_text("QUICK RACE"));
    CHECK(has_text("LOOKING FOR AN OPEN ROOM."));
    CHECK(find_text("HOST AN OPEN ROOM")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(find_text("ASK AGAIN")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(gdx_net_room_screen_row() == 2);

    /* A room: joined at once, to race. */
    press(0);
    CHECK(sJoinListedCalls == 0);
    fake_quick_answer(1);
    press(0);
    CHECK(sJoinListedCalls == 1 && sJoinListedIndex == -1 && sJoinListedWatch == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);

    /* None: the page offers to host one. */
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    fake_quick_answer(0);
    press(0);
    CHECK(sJoinListedCalls == 0);
    CHECK(has_text("NO OPEN ROOM RIGHT NOW. YOU CAN HOST ONE."));
    CHECK(find_text("ASK AGAIN")->colour == GDX_NET_ROOM_COLOUR_WHITE);
    press_times(GDX_NET_ROOM_IN_UP, 2);
    CHECK(find_text("HOST AN OPEN ROOM")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sHostCalls == 1 && sHostTypeArg == GDX_NET_ROOM_TYPE_OPEN);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);

    /* The host is refused: the front page says why. */
    reset();
    sFakeDirectory = 1;
    sHostAnswer = 0;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    fake_quick_answer(0);
    press(0);
    press_times(GDX_NET_ROOM_IN_UP, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(find_text("PORT 24302 IS IN USE BY ANOTHER PROGRAM.")->colour == GDX_NET_ROOM_COLOUR_RED);

    /* B while asking: an answer that comes after it joins nothing. */
    reset();
    sFakeDirectory = 1;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
    CHECK(gdx_net_room_screen_row() == 1);
    fake_quick_answer(1);
    press(0);
    CHECK(sJoinListedCalls == 0);

    /* Inside the cooldown nothing is sent, and the wait is said. */
    reset();
    sFakeDirectory = 1;
    sAskAnswer = 0;
    sAskWait = 30;
    gdx_net_room_screen_open();
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_QUICK);
    CHECK(has_text("ASK AGAIN IN 30 SECONDS."));
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
}

/* The room page of a directory room: its code or how its listing goes, and the upload it needs. */
static void test_room_on_the_internet(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 2, 0);
    gdx_net_room_screen_open();
    CHECK(has_text("YOUR ROOM ON PORT 24302"));
    CHECK(count_prefix("NEEDS") == 0); /* a room on this network */

    sFake.roomType = GDX_NET_ROOM_TYPE_OPEN;
    sFake.listing = GDX_NET_ROOM_LISTING_GATHERING;
    snprintf(sFake.listingText, sizeof(sFake.listingText), "Finding the address of this host");
    CHECK(find_text_at("FINDING THE ADDRESS OF THIS HOST", 58) != NULL);
    CHECK(find_text("FINDING THE ADDRESS OF THIS HOST")->colour == GDX_NET_ROOM_COLOUR_DIM);
    /* Eight seats: the host sends to seven racers and two watchers. */
    CHECK(has_text("NEEDS 4.4 MBIT UPLOAD"));
    sFake.racerCap = 12;
    CHECK(has_text("NEEDS 6.4 MBIT UPLOAD"));
    sFake.racerCap = 2;
    CHECK(has_text("NEEDS 1.5 MBIT UPLOAD"));

    sFake.listing = GDX_NET_ROOM_LISTING_LISTED;
    snprintf(sFake.code, sizeof(sFake.code), "ZAQ-Q9C");
    sFake.reachable = 1;
    CHECK(find_text_at("OPEN ROOM ZAQ-Q9C", 58) != NULL);
    sFake.reachable = 0;
    CHECK(find_text_at("OPEN ROOM ZAQ-Q9C - NOT REACHABLE", 58) != NULL);
    sFake.roomType = GDX_NET_ROOM_TYPE_CODE;
    CHECK(find_text_at("CODE ROOM ZAQ-Q9C", 58) != NULL); /* reachability is the Open list's */
    sFake.roomType = GDX_NET_ROOM_TYPE_FRIENDS;
    CHECK(find_text_at("FRIENDS ROOM ZAQ-Q9C", 58) != NULL);

    sFake.listing = GDX_NET_ROOM_LISTING_FAILED;
    snprintf(sFake.listingText, sizeof(sFake.listingText), "No free room code was found");
    CHECK(find_text_at("NO FREE ROOM CODE WAS FOUND", 58) != NULL);
    CHECK(find_text("NO FREE ROOM CODE WAS FOUND")->colour == GDX_NET_ROOM_COLOUR_RED);

    /* A member sees the code it can pass on. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 1, 2, 0);
    sFake.roomType = GDX_NET_ROOM_TYPE_CODE;
    snprintf(sFake.code, sizeof(sFake.code), "ZAQ-Q9C");
    gdx_net_room_screen_open();
    CHECK(find_text_at("ROOM CODE ZAQ-Q9C", 58) != NULL);
    CHECK(count_prefix("NEEDS") == 0);
    sFake.roomType = GDX_NET_ROOM_TYPE_OPEN;
    CHECK(find_text_at("OPEN ROOM CODE ZAQ-Q9C", 58) != NULL);
    sFake.roomType = GDX_NET_ROOM_TYPE_FRIENDS;
    CHECK(find_text_at("FRIENDS ROOM CODE ZAQ-Q9C", 58) != NULL);

    /* A member whose own check of the draw failed sees it on the vote result, in red, and only there. */
    sFake.lotteryChecked = 1;
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    CHECK(find_text_at("FRIENDS ROOM CODE ZAQ-Q9C", 58) != NULL);
    sFake.lotteryChecked = 0;
    CHECK(find_text_at("THE HOST'S DRAW FAILED THE CHECK.", 58) != NULL);
    CHECK(find_text("THE HOST'S DRAW FAILED THE CHECK.")->colour == GDX_NET_ROOM_COLOUR_RED);
    sFake.phase = GDX_NET_ROOM_PHASE_MACHINE;
    CHECK(!has_text("THE HOST'S DRAW FAILED THE CHECK."));
    /* The host never checks its own draw. */
    sFake.mode = GDX_NET_ROOM_MODE_HOSTING;
    sFake.phase = GDX_NET_ROOM_PHASE_LOTTERY;
    sFake.listing = GDX_NET_ROOM_LISTING_LISTED;
    CHECK(!has_text("THE HOST'S DRAW FAILED THE CHECK."));
}

/* ---- friends (O7.4) and the ban (O7.3) ---- */

static const char kMyCode[] = "ABCD EFGH JKMN PQRS TVWX YZ01 2345 6789 ABCD EFGH JKMN PQRS TVWX YZ01";

/* The front page's FRIENDS row, with a directory: the eighth row. */
static void open_friends(void) {
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    CHECK(find_text("FRIENDS") != NULL && find_text("FRIENDS")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
}

/* The friend code field: its one-letter texts on the field's two lines, in order. */
static void friend_field(char* out, int size) {
    int count;
    const GdxNetRoomDrawItem* items = gdx_net_room_screen_draw(&count);
    int length = 0;
    int line;
    int i;

    for (line = 0; line < 2; line++) {
        for (i = 0; i < count && length < size - 1; i++) {
            if (items[i].kind == GDX_NET_ROOM_DRAW_TEXT && items[i].y == 78 + line * 10 &&
                strlen(items[i].text) == 1) {
                out[length++] = items[i].text[0];
            }
        }
    }
    out[length] = '\0';
}

static void test_friends(void) {
    char field[80];

    reset();
    sFakeDirectory = 1;
    snprintf(sFakeMyCode, sizeof(sFakeMyCode), "%s", kMyCode);
    open_friends();
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS);
    CHECK(has_text("FRIENDS") && has_text("YOUR FRIEND CODE"));
    /* Seven groups a line: 34 columns, inside the margins. */
    CHECK(find_text_at("ABCD EFGH JKMN PQRS TVWX YZ01 2345", 68) != NULL);
    CHECK(find_text_at("6789 ABCD EFGH JKMN PQRS TVWX YZ01", 78) != NULL);
    CHECK(find_text("COPY MY CODE")->colour == GDX_NET_ROOM_COLOUR_CURSOR && find_text("COPY MY CODE")->y == 96);
    CHECK(find_text("ADD BY CODE")->y == 108 && find_text("ADD FROM THE LAST ROOM")->y == 120);
    CHECK(find_text("BACK")->y == 132);
    CHECK(has_text("NO FRIEND YET. ADD ONE BY CODE OR FROM"));
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* Copy: the code goes to the clipboard as it is shown. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sClipboardSets == 1 && strcmp(sClipboard, kMyCode) == 0);
    CHECK(find_text_at("YOUR FRIEND CODE IS ON THE CLIPBOARD.", 212) != NULL);
    sClipboardOk = 0;
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(find_text("THE CLIPBOARD COULD NOT BE SET.")->colour == GDX_NET_ROOM_COLOUR_RED);
    sClipboardOk = 1;

    /* Add by code: the cursor starts on PASTE. */
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND_ADD);
    CHECK(has_text("ADD A FRIEND") && has_text("CODE") && has_text("ADD THIS FRIEND"));
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(!has_text("YOUR FRIEND CODE IS ON THE CLIPBOARD.")); /* a note lasts as long as its page */
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "00000000000000000000000000000000000000000000000000000000") == 0);
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);

    /* Nothing, then not a code: the field stays as it was. */
    sClipboard[0] = '\0';
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(has_text("THE CLIPBOARD HOLDS NO TEXT."));
    snprintf(sClipboard, sizeof(sClipboard), "hello there");
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(find_text("THE CLIPBOARD HOLDS NO FRIEND CODE.")->colour == GDX_NET_ROOM_COLOUR_RED);
    snprintf(sClipboard, sizeof(sClipboard), "%s0", kMyCode); /* one letter too many */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("THE CLIPBOARD HOLDS NO FRIEND CODE."));
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "00000000000000000000000000000000000000000000000000000000") == 0);

    /* A code as a chat program might give it: small letters, O and L, hyphens and a line end. */
    snprintf(sClipboard, sizeof(sClipboard),
             "o123-4567-89ab-cdef-ghjk-mnpq-rstv-wxyz-l123-4567-89ab-cdef-ghjk-mnpq\r\n");
    press(GDX_NET_ROOM_IN_CONFIRM);
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "0123456789ABCDEFGHJKMNPQRSTVWXYZ1123456789ABCDEFGHJKMNPQ") == 0);
    CHECK(has_text("PASTED. CHECK IT AND ADD THIS FRIEND."));
    CHECK(gdx_net_room_screen_row() == 2); /* the cursor goes to ADD THIS FRIEND */
    CHECK(find_text("4")->x == 20 + 5 * 8); /* a space after each group of four */

    /* Refused: the page stays, says why, and the field is kept. */
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_SELF;
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAddCodeCalls == 1 && strcmp(sAddCodeText, "0123456789ABCDEFGHJKMNPQRSTVWXYZ1123456789ABCDEFGHJKMNPQ") == 0);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND_ADD);
    CHECK(find_text("THIS IS YOUR OWN FRIEND CODE.")->colour == GDX_NET_ROOM_COLOUR_RED);
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_LISTED;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("THIS FRIEND IS ON YOUR LIST ALREADY."));
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_BAD_CODE;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("THIS IS NOT A FRIEND CODE. CHECK EVERY"));
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_NOT_SAVED;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("YOUR LIST COULD NOT BE SAVED."));

    /* Added: the list, with the cursor on the new friend, and the field empty for the next one. */
    sFakeAddAnswer = -1;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS);
    CHECK(sFakeFriendCount == 1);
    CHECK(find_text_at("FRIEND 1 IS ON YOUR LIST.", 212) != NULL);
    CHECK(has_text("1 FRIEND"));
    CHECK(gdx_net_room_screen_row() == 4);
    CHECK(find_text("FRIEND 1")->colour == GDX_NET_ROOM_COLOUR_CURSOR && find_text("FRIEND 1")->y == 156);

    /* The field by hand: the 32 letters of a code, and B gives a change up. */
    press_times(GDX_NET_ROOM_IN_UP, 3);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND_ADD);
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "00000000000000000000000000000000000000000000000000000000") == 0);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 1);
    CHECK(has_text("LEFT AND RIGHT PICK A LETTER. UP AND"));
    press(GDX_NET_ROOM_IN_DOWN);
    press_times(GDX_NET_ROOM_IN_RIGHT, 60);
    CHECK(gdx_net_room_screen_editing() == 56);
    press(GDX_NET_ROOM_IN_UP);
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "Z0000000000000000000000000000000000000000000000000000001") == 0);
    press(GDX_NET_ROOM_IN_BACK);
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "00000000000000000000000000000000000000000000000000000000") == 0);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_editing() == 0);
    friend_field(field, (int) sizeof(field));
    CHECK(strcmp(field, "Z0000000000000000000000000000000000000000000000000000000") == 0);

    /* B goes back to the row, then to the front page's. */
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS && gdx_net_room_screen_row() == 1);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT && gdx_net_room_screen_row() == 7);

    /* No code yet: nothing to copy. */
    reset();
    sFakeDirectory = 1;
    open_friends();
    CHECK(find_text("YOUR PROFILE HAS NO FRIEND CODE.")->colour == GDX_NET_ROOM_COLOUR_RED);
    CHECK(find_text("COPY MY CODE")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(gdx_net_room_screen_row() == 1);

    /* No directory: the row is there all the same, as the list and the ban work on this network too. */
    reset();
    gdx_net_room_screen_open();
    CHECK(find_text("FRIENDS")->y == 58 + 4 * 12);
    /* No profile: no friend list. A profile that goes takes the page back to the front. */
    reset();
    sFakeProfile = 0;
    gdx_net_room_screen_open();
    CHECK(!has_text("FRIENDS"));
    reset();
    sFakeDirectory = 1;
    open_friends();
    sFakeProfile = 0;
    press(0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);
}

/* A long list scrolls, and the screen never draws more than it can hold. */
static void test_friends_long_list(void) {
    char name[16];
    int i;

    reset();
    sFakeDirectory = 1;
    snprintf(sFakeMyCode, sizeof(sFakeMyCode), "%s", kMyCode);
    for (i = 0; i < GDX_NET_ROOM_FRIENDS_MAX; i++) {
        snprintf(name, sizeof(name), "F%03d", i);
        fake_friend(name, 'A');
    }
    open_friends();
    CHECK(has_text("200 FRIENDS"));
    CHECK(has_text("F000") && has_text("F005") && !has_text("F006")); /* six lines, 156 to 206 */
    press_times(GDX_NET_ROOM_IN_DOWN, 3 + GDX_NET_ROOM_FRIENDS_MAX + 5);
    CHECK(gdx_net_room_screen_row() == 4 + GDX_NET_ROOM_FRIENDS_MAX - 1);
    CHECK(find_text("F199") != NULL && find_text("F199")->y == 206 && !has_text("F193"));
    CHECK(draw_count() <= GDX_NET_ROOM_DRAW_MAX);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND);
    CHECK(find_text("F199") != NULL && find_text("F199")->font == GDX_NET_ROOM_FONT_TITLE);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_row() == 4 + GDX_NET_ROOM_FRIENDS_MAX - 1);
}

/* The rooms the directory says this friend hosts (decision 43). */
static void fake_friend_answer(int count) {
    int i;

    sFake.dirStatus = GDX_NET_ROOM_DIR_READY;
    sFake.dirAsk = GDX_NET_ROOM_ASK_FRIEND;
    sFake.dirSerial++;
    sFake.listedCount = count;
    for (i = 0; i < count; i++) {
        snprintf(sFake.listed[i].code, sizeof(sFake.listed[i].code), "FFF-00%c", (char) ('0' + i));
        sFake.listed[i].type = GDX_NET_ROOM_TYPE_FRIENDS;
        sFake.listed[i].humans = 3;
        sFake.listed[i].freeSeats = (unsigned char) (i == 0 ? 5 : 0);
        sFake.listed[i].reachable = 1;
    }
}

static void test_friend_page(void) {
    reset();
    sFakeDirectory = 1;
    snprintf(sFakeMyCode, sizeof(sFakeMyCode), "%s", kMyCode);
    fake_friend("Ana", 'A');
    fake_friend("Bo", 'B');
    open_friends();
    CHECK(has_text("2 FRIENDS") && find_text("ANA")->y == 156 && find_text("BO")->y == 166);
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    CHECK(find_text("ANA")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND);
    CHECK(find_text("ANA") != NULL && find_text("ANA")->font == GDX_NET_ROOM_FONT_TITLE);
    CHECK(find_text_at("AAAA AAAA AAAA AAAA AAAA AAAA AAAA", 60) != NULL);
    CHECK(find_text_at("AAAA AAAA AAAA AAAA AAAA AAAA AAAA", 70) != NULL);
    CHECK(find_text("FIND THEIR ROOMS")->colour == GDX_NET_ROOM_COLOUR_CURSOR && find_text("FIND THEIR ROOMS")->y == 90);
    CHECK(find_text("REMOVE FROM FRIENDS")->y == 102 && find_text("BACK")->y == 114);

    /* Find: one request at a time. A press while it is out sends nothing, and the cursor stays. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskFriendCalls == 1 && sAskFriendIndex == 0);
    CHECK(find_text_at("ASKING THE ROOM DIRECTORY.", 132) != NULL);
    gdx_net_room_screen_take_sound();
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskFriendCalls == 1);
    CHECK(gdx_net_room_screen_take_sound() == GDX_NET_ROOM_SOUND_REFUSE);
    CHECK(gdx_net_room_screen_row() == 0);

    /* The answer: their rooms under the rows, and the cursor on the first. */
    fake_friend_answer(2);
    press(0);
    CHECK(find_text_at("2 ROOMS OF THEIRS", 132) != NULL);
    CHECK(find_text("FFF-000")->y == 146 && find_text("FFF-001")->y == 156);
    CHECK(has_text("5 SEATS FREE") && has_text("FULL - WATCH") && count_prefix("3 PLAYERS") == 2);
    CHECK(gdx_net_room_screen_row() == 3);
    CHECK(find_text("FFF-000")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sJoinListedCalls == 1 && sJoinListedIndex == 1 && sJoinListedWatch == 1); /* full: a watch seat */
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_JOINING);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRONT);

    /* Ana's answer is not Bo's. */
    press_times(GDX_NET_ROOM_IN_DOWN, 7);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press_times(GDX_NET_ROOM_IN_DOWN, 5);
    CHECK(find_text("BO")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND);
    CHECK(!has_text("FFF-000") && !has_text("2 ROOMS OF THEIRS"));
    fake_friend_answer(0);
    sAskAnswer = 1;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskFriendCalls == 2 && sAskFriendIndex == 1);
    fake_friend_answer(0);
    press(0);
    CHECK(has_text("NO ROOM OF THEIRS RIGHT NOW."));
    CHECK(gdx_net_room_screen_row() == 0);

    /* Inside the cooldown nothing is sent, and the wait is said. A failure is said in red. */
    sAskAnswer = 0;
    sAskWait = 20;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sAskFriendCalls == 3 && has_text("ASK AGAIN IN 20 SECONDS."));
    sAskAnswer = 1;
    press(GDX_NET_ROOM_IN_CONFIRM);
    sFake.dirStatus = GDX_NET_ROOM_DIR_FAILED;
    sFake.dirSerial++;
    snprintf(sFake.dirText, sizeof(sFake.dirText), "The room directory did not answer");
    press(0);
    CHECK(find_text("THE ROOM DIRECTORY DID NOT ANSWER")->colour == GDX_NET_ROOM_COLOUR_RED);

    /* Remove asks twice. The list follows, with the cursor on the friend that took the place. */
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("PRESS A AGAIN TO REMOVE") && sRemoveCalls == 0);
    press(GDX_NET_ROOM_IN_UP);
    CHECK(has_text("REMOVE FROM FRIENDS"));
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sRemoveCalls == 1 && sRemoveIndex == 1 && sFakeFriendCount == 1);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS);
    CHECK(find_text_at("BO IS OFF YOUR LIST.", 212) != NULL);
    CHECK(gdx_net_room_screen_row() == 4 && find_text("ANA")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS && gdx_net_room_screen_row() == 4);

    /* The last friend goes: the cursor goes to ADD BY CODE. */
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sFakeFriendCount == 0 && gdx_net_room_screen_row() == 1);
    CHECK(has_text("NO FRIEND YET. ADD ONE BY CODE OR FROM"));

    /* With no directory, the row cannot be chosen and the page says why. */
    reset();
    fake_friend("Cy", 'C');
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS);
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIEND);
    CHECK(find_text("FIND THEIR ROOMS")->colour == GDX_NET_ROOM_COLOUR_DIM);
    CHECK(gdx_net_room_screen_row() == 1);
    CHECK(has_text("FINDING ROOMS NEEDS A ROOM DIRECTORY."));
}

static void test_met(void) {
    reset();
    sFakeDirectory = 1;
    snprintf(sFakeMyCode, sizeof(sFakeMyCode), "%s", kMyCode);
    open_friends();
    press_times(GDX_NET_ROOM_IN_DOWN, 2);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_MET);
    CHECK(has_text("LAST ROOM") && has_text("NOBODY YET. THE PLAYERS OF THE NEXT ROOM"));
    CHECK(find_text("BACK")->colour == GDX_NET_ROOM_COLOUR_CURSOR && find_text("BACK")->y == 200);
    press(GDX_NET_ROOM_IN_BACK);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_FRIENDS && gdx_net_room_screen_row() == 2);

    /* Two players: one already on the list. */
    snprintf(sFakeMetNames[0], sizeof(sFakeMetNames[0]), "Cy");
    snprintf(sFakeMetNames[1], sizeof(sFakeMetNames[1]), "Di");
    sFakeMetListed[1] = 1;
    sFakeMetCount = 2;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("PICK A PLAYER TO ADD AS A FRIEND."));
    CHECK(find_text("CY")->colour == GDX_NET_ROOM_COLOUR_CURSOR && find_text("CY")->y == 72);
    CHECK(find_text("DI")->colour == GDX_NET_ROOM_COLOUR_DIM && find_text("DI")->y == 82);
    CHECK(count_prefix("ON YOUR LIST") == 1 && find_text("ON YOUR LIST")->y == 82);
    press(GDX_NET_ROOM_IN_DOWN);
    CHECK(gdx_net_room_screen_row() == 2); /* Di is passed by */
    press(GDX_NET_ROOM_IN_UP);
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_FULL;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMetAddCalls == 1 && sMetAddIndex == 0);
    CHECK(find_text("THE LIST IS FULL. REMOVE A FRIEND FIRST.")->colour == GDX_NET_ROOM_COLOUR_RED);
    sFakeAddAnswer = -1;
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sMetAddCalls == 2 && find_text_at("CY IS ON YOUR LIST.", 212) != NULL);
    CHECK(count_prefix("ON YOUR LIST") == 2);
    CHECK(gdx_net_room_screen_row() == 2); /* nothing left to add: BACK */
    sFakeAddAnswer = GDX_NET_ROOM_FRIEND_RENAMED;
    sFakeMetListed[0] = 0;
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("YOUR LIST NOW CALLS THIS FRIEND CY."));
}

/* The host's ban: asked twice, then the room (O7.3). */
static void test_room_ban(void) {
    reset();
    fake_room(GDX_NET_ROOM_MODE_HOSTING, GDX_NET_ROOM_PHASE_LOBBY, 0, 3, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 9);
    press_times(GDX_NET_ROOM_IN_UP, 1);
    CHECK(find_text("P01")->colour == GDX_NET_ROOM_COLOUR_CURSOR);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(find_text("MUTE THIS PLAYER")->y == 80 && find_text("REMOVE FROM THE ROOM")->y == 92);
    CHECK(find_text("BAN FROM THE ROOM")->y == 104 && find_text("BACK")->y == 116);
    CHECK(find_text_at("A BAN REMOVES THE PLAYER AND REFUSES", 166) != NULL);
    CHECK(find_text_at("THEIR FRIEND CODE AND THEIR ADDRESS FOR", 176) != NULL);
    CHECK(find_text_at("AS LONG AS THIS ROOM LASTS.", 186) != NULL);
    press(GDX_NET_ROOM_IN_UP);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(has_text("PRESS A AGAIN TO BAN") && sBanCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    /* Moving off the row takes the question back. */
    press(GDX_NET_ROOM_IN_UP);
    CHECK(has_text("BAN FROM THE ROOM"));
    press(GDX_NET_ROOM_IN_DOWN);
    press(GDX_NET_ROOM_IN_CONFIRM);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(sBanCalls == 1 && sBanId == 1 && sKickCalls == 0);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_ROOM);

    /* A member cannot ban. */
    reset();
    fake_room(GDX_NET_ROOM_MODE_JOINED, GDX_NET_ROOM_PHASE_LOBBY, 2, 4, 0);
    gdx_net_room_screen_open();
    press_times(GDX_NET_ROOM_IN_DOWN, 4);
    press(GDX_NET_ROOM_IN_CONFIRM);
    CHECK(gdx_net_room_screen_page() == GDX_NET_ROOM_PAGE_PLAYER);
    CHECK(!has_text("BAN FROM THE ROOM") && !has_text("A BAN REMOVES THE PLAYER AND REFUSES"));
}

int main(void) {
    test_filter();
    test_track_name();
    test_wrap();
    test_timer();
    test_wanted();
    test_wanted_for_the_front_page();
    test_front();
    test_front_while_the_room_is_handed_over();
    test_no_profile();
    test_host();
    test_port();
    test_rooms();
    test_found();
    test_address();
    test_joining_and_closed();
    test_joining_ipv6();
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
    test_room_save_track();
    test_notice();
    test_front_with_a_directory();
    test_join_by_code();
    test_open_rooms();
    test_quick_race();
    test_room_on_the_internet();
    test_friends();
    test_friends_long_list();
    test_friend_page();
    test_met();
    test_room_ban();

    if (sFailures != 0) {
        printf("gdx_net_room_screen_tests: %d of %d checks FAILED\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_room_screen_tests: %d checks passed\n", sChecks);
    return 0;
}
