/* port/gdx_net_room_view.h -- the G-NET room as the game's own screens read it, and what they ask of it.
 * Copyright (c) Zorkats
 *
 * The room session lives in C++ (gdx_lan_lobby.h). The game's screens are C, so once a frame they take one
 * flat copy of everything the session shows, GdxNetRoomView, and they act through the functions below. One
 * copy per frame means every line of a screen agrees with the others: two separate reads could straddle a
 * change of the room.
 *
 * Nothing here opens a socket until the player hosts, searches or joins.
 *
 * Game thread only, which is also the thread the port menu draws on. */

#ifndef GDX_NET_ROOM_VIEW_H
#define GDX_NET_ROOM_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_ROOM_NAME_BYTES 17    /* a profile name, 16 bytes, and its NUL */
#define GDX_NET_ROOM_ADDRESS_BYTES 48 /* "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:65535" */
#define GDX_NET_ROOM_STATUS_BYTES 128
#define GDX_NET_ROOM_LINE_BYTES 64
#define GDX_NET_ROOM_PLAYERS 14 /* 12 race seats and 2 watch seats */
#define GDX_NET_ROOM_RACERS 12
#define GDX_NET_ROOM_FOUND 32
#define GDX_NET_ROOM_LINES 4
#define GDX_NET_ROOM_MESSAGES 8 /* the preset messages a player can send */
#define GDX_NET_ROOM_WATCH_SEATS 2
#define GDX_NET_ROOM_SEATS_MIN 1
#define GDX_NET_ROOM_SEATS_MAX 12
#define GDX_NET_ROOM_DEFAULT_PORT 24302u

#define GDX_NET_ROOM_NOBODY 0xFF       /* no player: the room itself */
#define GDX_NET_ROOM_TRACK_RANDOM 0xFF /* a ballot that lets the room draw */
#define GDX_NET_ROOM_TRACK_NONE 0xFE
#define GDX_NET_ROOM_TRACK_CUSTOM 0xFD /* a racer's own track: its identity and title stand beside it */
#define GDX_NET_ROOM_TRACK_ID_BYTES 32 /* a custom track's identity, the SHA-256 of its content bytes */
#define GDX_NET_ROOM_TITLE_BYTES 21    /* a custom track's title, 1 to 20 printable bytes, and its NUL */

#define GDX_NET_ROOM_CODE_CHARS 6   /* a room code, "ABC-DEF" on screen */
#define GDX_NET_ROOM_CODE_BYTES 8   /* "ABC-DEF" and its NUL */
#define GDX_NET_ROOM_PUBLIC_BYTES 48 /* "[2001:db8::1]:24302" */
#define GDX_NET_ROOM_LISTED 10       /* the Open rooms a page lists (decision 38) */

/* GdxNetRoomView.roomType, GdxNetRoomListed.type, and the type a host picks */
#define GDX_NET_ROOM_TYPE_LAN 0     /* this network only: no directory */
#define GDX_NET_ROOM_TYPE_CODE 1    /* listed by its code, for whoever has it */
#define GDX_NET_ROOM_TYPE_OPEN 2    /* listed for anyone: Open rooms and Quick race */
#define GDX_NET_ROOM_TYPE_FRIENDS 3 /* listed for the host's friends, who prove their key (O7.3) */
#define GDX_NET_ROOM_TYPES 4

/* GdxNetRoomView.listing: a hosted directory room's place in the directory */
#define GDX_NET_ROOM_LISTING_OFF 0       /* a LAN room, or no session */
#define GDX_NET_ROOM_LISTING_GATHERING 1 /* finding this host's address */
#define GDX_NET_ROOM_LISTING_LISTING 2
#define GDX_NET_ROOM_LISTING_LISTED 3
#define GDX_NET_ROOM_LISTING_FAILED 4    /* the room runs on unlisted; listingText says why */

/* GdxNetRoomView.dirStatus and dirAsk: the last directory request of the pages */
#define GDX_NET_ROOM_DIR_IDLE 0
#define GDX_NET_ROOM_DIR_ASKING 1
#define GDX_NET_ROOM_DIR_READY 2
#define GDX_NET_ROOM_DIR_FAILED 3
#define GDX_NET_ROOM_ASK_OPEN 0   /* the list of Open rooms */
#define GDX_NET_ROOM_ASK_QUICK 1  /* the room Quick race picks */
#define GDX_NET_ROOM_ASK_FRIEND 2 /* the rooms a friend hosts */

/* GdxNetRoomView.mode */
#define GDX_NET_ROOM_MODE_IDLE 0     /* no session, no socket */
#define GDX_NET_ROOM_MODE_BROWSING 1 /* searching the network for rooms */
#define GDX_NET_ROOM_MODE_HOSTING 2
#define GDX_NET_ROOM_MODE_JOINING 3 /* waiting for the host to answer */
#define GDX_NET_ROOM_MODE_JOINED 4
#define GDX_NET_ROOM_MODE_CLOSED 5 /* a session ended on its own; status says why */

/* GdxNetRoomView.phase and GdxNetRoomFound.phase */
#define GDX_NET_ROOM_PHASE_LOBBY 0
#define GDX_NET_ROOM_PHASE_VOTING 1
#define GDX_NET_ROOM_PHASE_LOTTERY 2
#define GDX_NET_ROOM_PHASE_PICK 3
#define GDX_NET_ROOM_PHASE_MACHINE 4
#define GDX_NET_ROOM_PHASE_GRID_SYNC 5
#define GDX_NET_ROOM_PHASE_COUNTDOWN 6
#define GDX_NET_ROOM_PHASE_RACING 7
#define GDX_NET_ROOM_PHASE_RESULTS 8
#define GDX_NET_ROOM_PHASE_ASSETS 9 /* racers fetch the chosen custom track before machine select */

/* GdxNetRoomResult.kind */
#define GDX_NET_ROOM_RESULT_FINISHED 0
#define GDX_NET_ROOM_RESULT_UNFINISHED 1
#define GDX_NET_ROOM_RESULT_RETIRED 2

typedef struct GdxNetRoomPlayer {
    unsigned char id;
    unsigned char host;
    unsigned char ready;    /* the lobby's ready mark */
    unsigned char watcher;  /* holds a watch seat */
    unsigned char raceNext; /* a watcher who asked for a race seat at the next grid */
    unsigned char inRace;   /* on the grid of the race being set up or run */
    unsigned char locked;   /* locked a machine in */
    unsigned char voted;    /* has a ballot in the vote being run */
    unsigned char muted;    /* this copy does not show the player's messages */
    unsigned char machine;  /* roster index, or 0xFF for none */
    unsigned colour;        /* 0x00RRGGBB */
    unsigned pingMs;
    unsigned points; /* of the series being run, or of the last one until the next starts */
    char name[GDX_NET_ROOM_NAME_BYTES];
} GdxNetRoomPlayer;

typedef struct GdxNetRoomBallot {
    unsigned char id;
    unsigned char track; /* 0 to 23, GDX_NET_ROOM_TRACK_RANDOM or GDX_NET_ROOM_TRACK_CUSTOM */
    /* When track is GDX_NET_ROOM_TRACK_CUSTOM: the voter's track. Zero and empty otherwise. */
    unsigned char custom[GDX_NET_ROOM_TRACK_ID_BYTES];
    char title[GDX_NET_ROOM_TITLE_BYTES];
} GdxNetRoomBallot;

typedef struct GdxNetRoomResult {
    unsigned char id;
    unsigned char kind;
    unsigned char points; /* what the race is worth to the player */
    unsigned timeMs;      /* 0 unless finished */
} GdxNetRoomResult;

/* A line of the room's log: a preset message of a player, or a notice of the room. */
typedef struct GdxNetRoomLine {
    unsigned char from; /* a player id, or GDX_NET_ROOM_NOBODY for a notice */
    unsigned colour;
    char name[GDX_NET_ROOM_NAME_BYTES];
    char text[GDX_NET_ROOM_LINE_BYTES];
} GdxNetRoomLine;

/* A room the search found. */
typedef struct GdxNetRoomFound {
    unsigned ip; /* a.b.c.d as (a << 24) | (b << 16) | (c << 8) | d */
    unsigned port;
    unsigned char racers;
    unsigned char racerCap;
    unsigned char watchers;
    unsigned char phase;
    char hostName[GDX_NET_ROOM_NAME_BYTES];
} GdxNetRoomFound;

/* A room the directory listed. */
typedef struct GdxNetRoomListed {
    char code[GDX_NET_ROOM_CODE_BYTES]; /* "ABC-DEF" */
    unsigned char type;                 /* GDX_NET_ROOM_TYPE_* */
    unsigned char humans;
    unsigned char freeSeats;            /* race seats */
    unsigned char reachable;            /* passed the reachability test (decision 33) */
} GdxNetRoomListed;

typedef struct GdxNetRoomView {
    int mode;
    char status[GDX_NET_ROOM_STATUS_BYTES];     /* why the last session ended */
    unsigned port;                              /* the host's port while hosting, the room's while joining */
    char hostText[GDX_NET_ROOM_ADDRESS_BYTES];  /* "192.168.1.20:24302" or "[2001:db8::1]:24302" while joining or joined */
    int localId;                                /* the local player's id, or GDX_NET_ROOM_NOBODY */
    char joinCode[GDX_NET_ROOM_CODE_BYTES];     /* the code being joined by, "" when joining by address */

    /* The room's place in the directory. */
    int roomType;                               /* GDX_NET_ROOM_TYPE_* */
    char code[GDX_NET_ROOM_CODE_BYTES];         /* the room's code, "" for none */
    int listing;                                /* GDX_NET_ROOM_LISTING_*, while hosting */
    char listingText[GDX_NET_ROOM_STATUS_BYTES];
    int reachable;                              /* while listed */
    char publicAddress[GDX_NET_ROOM_PUBLIC_BYTES]; /* as a STUN server saw this host, "" when unknown */

    /* The pages' last directory request, which outlives any session. */
    int dirStatus;   /* GDX_NET_ROOM_DIR_* */
    int dirAsk;      /* GDX_NET_ROOM_ASK_* */
    unsigned dirSerial; /* grows with each answer */
    char dirText[GDX_NET_ROOM_STATUS_BYTES]; /* the failure */
    int listedCount; /* Open: the list; Friend: the friend's rooms */
    GdxNetRoomListed listed[GDX_NET_ROOM_LISTED];
    int quickFound;  /* Quick: a room was found, and it is quick */
    GdxNetRoomListed quick;

    /* The room. Meaningful while hosting or joined. */
    int phase;
    unsigned phaseLeftMs;
    unsigned phaseTotalMs; /* 0 when the phase has no timer */
    unsigned raceIndex;
    int racerCap;
    int customTracks; /* the host's switch: racers may vote for and pick custom tracks */
    int lastPlacePicks; /* the host's course choice: 0 the racers vote, 1 last place picks */
    int cpuFill;      /* the host's switch: CPUs take the grid slots the humans leave free */
    int successor;    /* the next host's id, or GDX_NET_ROOM_NOBODY */
    unsigned pool;    /* bit n set: track n can be voted, picked or drawn */
    int track;     /* the track of the race being set up or run, or GDX_NET_ROOM_TRACK_NONE */
    /* When track is GDX_NET_ROOM_TRACK_CUSTOM: its identity and title, and this copy's download of it. */
    unsigned char trackId[GDX_NET_ROOM_TRACK_ID_BYTES];
    char trackTitle[GDX_NET_ROOM_TITLE_BYTES];
    int trackHeld;   /* this copy holds the track, checked */
    int trackFailed; /* its download failed; the room falls back to a stock track */
    unsigned trackChunks;
    unsigned trackChunksOf; /* 0 until the track's owner offers it */
    int lastTrack;
    int picker;      /* who picks in the pick phase, or GDX_NET_ROOM_NOBODY */
    int lockedCount; /* racers who locked a machine in */
    int racers;      /* racers on the grid */

    int playerCount;
    GdxNetRoomPlayer players[GDX_NET_ROOM_PLAYERS];

    /* The vote being run. */
    int ballotCount;
    GdxNetRoomBallot ballots[GDX_NET_ROOM_RACERS];

    /* The last lottery. */
    int lotteryCount;
    GdxNetRoomBallot lottery[GDX_NET_ROOM_RACERS];
    int lotteryWinner;  /* index into lottery, or -1 when nobody voted */
    int lotteryTrack;
    int lotteryChecked; /* 0 when this copy's own draw disagrees with the host's */

    /* The last track chosen without a vote. */
    int pickedTrack;
    int pickedDrawn; /* the room drew it: a first race, a timeout or a Random pick */
    int pickedBy;    /* who picked, or GDX_NET_ROOM_NOBODY */

    /* The table of a race, being run or over. */
    int haveResults;
    unsigned resultsRace;
    int resultsFinal;
    int resultCount;
    GdxNetRoomResult results[GDX_NET_ROOM_RACERS];

    /* The newest lines of the log, oldest first. A muted player's messages are not among them. */
    int lineCount;
    GdxNetRoomLine lines[GDX_NET_ROOM_LINES];

    /* The rooms found while browsing. */
    int foundCount;
    GdxNetRoomFound found[GDX_NET_ROOM_FOUND];
} GdxNetRoomView;

void gdx_lan_room_view(GdxNetRoomView* out);

/* 1 when an active profile with a legal name exists. A room shows that name and colour, so nothing below
 * that starts a session works without one. */
int gdx_lan_room_profile_ok(void);

/* The port this copy hosts on and searches, 1024 to 65535. The setter saves it and ignores anything else. */
unsigned gdx_lan_room_port(void);
void gdx_lan_room_set_port(unsigned port);

/* Host a room of type GDX_NET_ROOM_TYPE_* on gdx_lan_room_port(). 0 with the reason in error when the port is
 * taken, no profile exists, or a directory room is asked for and no directory is set; the session is then idle
 * again. A directory room lists itself once it is up, and the view follows its listing. */
int gdx_lan_room_host(int type, char* error, int size);
/* 1 when a room directory is set, so rooms other than LAN ones can be hosted, found and joined. */
int gdx_lan_room_directory_ok(void);
/* Join the room with this code ("ABC-DEF", any form gdx_net_codes accepts), to race or to watch. 0 when the
 * code does not parse, no profile exists or no directory is set. */
int gdx_lan_room_join_code(const char* code, int watch);
/* Ask the directory for the Open rooms or for Quick race's room (GDX_NET_ROOM_ASK_OPEN or _QUICK). Both share a
 * 30 s cooldown (decision 38): 0 when the press is refused, with the seconds left in waitSeconds, and nothing
 * sent. The answer shows in a later view. */
int gdx_lan_room_ask(int ask, int* waitSeconds);
/* Join a room the directory gave: index into the view's listed rooms, or -1 for Quick race's room. 0 when that
 * room is not in the last answer or no profile exists. */
int gdx_lan_room_join_listed(int index, int watch);
/* The first-use notice (ODD 10): 1 once the player has read the current one. */
int gdx_lan_room_notice_seen(void);
void gdx_lan_room_notice_accept(void);
/* Search the network for rooms. */
void gdx_lan_room_browse(void);
/* Join the room at ip and port, to race or (watch != 0) to watch. 0 when no profile exists or the address is
 * not one a room can have. */
int gdx_lan_room_join(unsigned ip, unsigned port, int watch);
/* Leave or close the room, or stop searching or joining. */
void gdx_lan_room_stop(void);
/* From a closed session back to idle. */
void gdx_lan_room_dismiss(void);
/* 1 while a room this copy's host left still runs here, finishing its race before it goes to the next host
 * (D28.5). next gets that player's name, "" when nobody is left to take it. */
int gdx_lan_room_handing_over(char* next, int size);

/* The player's own actions. The room decides; the answer shows in a later view. */
void gdx_lan_room_ready(int ready);
void gdx_lan_room_race_next(int raceNext);
/* A ballot in the vote, or the pick of the racer who picks. track is 0 to 23 or GDX_NET_ROOM_TRACK_RANDOM;
 * anything else is ignored. */
void gdx_lan_room_vote(int track);
void gdx_lan_room_pick(int track);
/* The same for a track of this copy's own, when the room takes custom tracks. id is its identity,
 * GDX_NET_ROOM_TRACK_ID_BYTES bytes, and title 1 to 20 printable bytes; anything else is ignored. The room
 * fetches the track from this copy if it is chosen, so its bytes must have gone to gdx_lan_room_keep_track. */
void gdx_lan_room_vote_custom(const unsigned char* id, const char* title);
void gdx_lan_room_pick_custom(const unsigned char* id, const char* title);
/* Keeps the content bytes of a track of this copy's, whose identity is id, for the room to send. The last six
 * kept stay. Bytes that are not a track are kept too; the room's racers refuse them. */
void gdx_lan_room_keep_track(const unsigned char* id, const unsigned char* bytes, unsigned long length);

/* Send preset message index, 0 to GDX_NET_ROOM_MESSAGES - 1, to the room. The room refuses a player who sends
 * too many too fast and says so in the log. */
void gdx_lan_room_message(int index);
/* The text of that message as the room logs it, "Good luck!". Empty for an index outside the list. */
void gdx_lan_room_message_text(int index, char* out, int size);
/* Hide or show a player's messages on this copy only. Nothing is sent, and the mark ends when the player
 * leaves the room. */
void gdx_lan_room_set_muted(int id, int muted);

/* The host's controls. They do nothing on a client. */
void gdx_lan_room_start(void);
void gdx_lan_room_racer_cap(int cap);
void gdx_lan_room_custom_tracks(int on);
void gdx_lan_room_last_place_picks(int on);
void gdx_lan_room_cpu_fill(int on);
void gdx_lan_room_kick(int id);
/* Removes the player and refuses their friend key for the rest of the room (O7.3). */
void gdx_lan_room_ban(int id);
/* Draw the lottery's track now and not when its timer ends. */
void gdx_lan_room_skip_lottery(void);
/* From the results back to the lobby, which ends the series. Without it the next track choice starts when
 * the results' timer ends. */
void gdx_lan_room_end_series(void);

/* A track's name as the room screens show it, "Mute City". track is 0 to 23 or one of the two special values. */
void gdx_lan_room_track_name(int track, char* out, int size);

/* ---- Friends (O7.4) ----
 *
 * A friend is a friend key this install keeps, with the name last seen. The list lives on this computer only and
 * is written on every change. Indexes count from the top of the list, in the order friends were added. */

#define GDX_NET_ROOM_FRIEND_CODE_CHARS 56 /* the key in base32 and 4 check characters */
#define GDX_NET_ROOM_FRIEND_CODE_BYTES 72 /* 14 groups of 4 with a space between groups, and the NUL */
#define GDX_NET_ROOM_FRIENDS_MAX 200
#define GDX_NET_ROOM_MET_MAX 32           /* the players of the last room a page lists */

/* gdx_lan_room_friend_add_code and gdx_lan_room_met_add */
#define GDX_NET_ROOM_FRIEND_ADDED 0
#define GDX_NET_ROOM_FRIEND_RENAMED 1   /* on the list already; the name is now the one the room showed */
#define GDX_NET_ROOM_FRIEND_LISTED 2    /* on the list already: nothing changed */
#define GDX_NET_ROOM_FRIEND_BAD_CODE 3  /* not a friend code: a wrong length, letter or check */
#define GDX_NET_ROOM_FRIEND_SELF 4      /* the code of the active profile */
#define GDX_NET_ROOM_FRIEND_FULL 5
#define GDX_NET_ROOM_FRIEND_BAD_NAME 6
#define GDX_NET_ROOM_FRIEND_NOT_SAVED 7 /* the list could not be written; it is as it was */
#define GDX_NET_ROOM_FRIEND_GONE 8      /* no such entry */

/* The active profile's friend code, "ABCD EFGH ...". Makes the profile's key on first use. 0 when no profile is
 * active. */
int gdx_lan_room_my_friend_code(char* out, int size);
int gdx_lan_room_friend_count(void);
/* 0 for an index outside the list. */
int gdx_lan_room_friend_name(int index, char* out, int size);
int gdx_lan_room_friend_code(int index, char* out, int size);
/* Adds the friend whose code this is, under a name of its own until a room shows theirs. Any form the code
 * parser takes: spaces, hyphens, lower case and the confusable letters. */
int gdx_lan_room_friend_add_code(const char* code);
/* 1 when the friend was removed. */
int gdx_lan_room_friend_remove(int index);
/* Ask the directory for the rooms this friend hosts (decision 43). The answer shows in a later view, with dirAsk
 * GDX_NET_ROOM_ASK_FRIEND, and gdx_lan_room_join_listed joins one. 0 when nothing was sent: a request is under
 * way, no directory is set or the index is outside the list. A press inside the directory's cooldown sets
 * waitSeconds to what is left of it, as gdx_lan_room_ask does. */
int gdx_lan_room_ask_friend(int index, int* waitSeconds);

/* The players with a friend key in the last room this copy was in, or the one it is in (gdx::lan::LastRoom). */
int gdx_lan_room_met_count(void);
int gdx_lan_room_met_name(int index, char* out, int size);
/* 1 when that player is on the friend list under that name. */
int gdx_lan_room_met_listed(int index);
int gdx_lan_room_met_add(int index);

/* The system clipboard, for a friend code. 0 when it could not be set, or holds no text. */
int gdx_lan_room_clipboard_set(const char* text);
int gdx_lan_room_clipboard_get(char* out, int size);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_ROOM_VIEW_H */
