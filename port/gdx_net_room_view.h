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
#define GDX_NET_ROOM_ADDRESS_BYTES 24 /* "255.255.255.255:65535" */
#define GDX_NET_ROOM_STATUS_BYTES 128
#define GDX_NET_ROOM_LINE_BYTES 64
#define GDX_NET_ROOM_PLAYERS 14 /* 12 race seats and 2 watch seats */
#define GDX_NET_ROOM_RACERS 12
#define GDX_NET_ROOM_FOUND 32
#define GDX_NET_ROOM_LINES 4
#define GDX_NET_ROOM_MESSAGES 8 /* the preset messages a player can send */
#define GDX_NET_ROOM_WATCH_SEATS 2
#define GDX_NET_ROOM_SEATS_MIN 8
#define GDX_NET_ROOM_SEATS_MAX 12
#define GDX_NET_ROOM_DEFAULT_PORT 24302u

#define GDX_NET_ROOM_NOBODY 0xFF       /* no player: the room itself */
#define GDX_NET_ROOM_TRACK_RANDOM 0xFF /* a ballot that lets the room draw */
#define GDX_NET_ROOM_TRACK_NONE 0xFE

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
    unsigned char track; /* 0 to 23, or GDX_NET_ROOM_TRACK_RANDOM */
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
    unsigned char sameBuild; /* 0: the room runs another build and cannot be joined */
    char hostName[GDX_NET_ROOM_NAME_BYTES];
} GdxNetRoomFound;

typedef struct GdxNetRoomView {
    int mode;
    char status[GDX_NET_ROOM_STATUS_BYTES];     /* why the last session ended */
    unsigned port;                              /* the host's port while hosting, the room's while joining */
    char hostText[GDX_NET_ROOM_ADDRESS_BYTES];  /* "192.168.1.20:24302" while joining or joined */
    int localId;                                /* the local player's id, or GDX_NET_ROOM_NOBODY */

    /* The room. Meaningful while hosting or joined. */
    int phase;
    unsigned phaseLeftMs;
    unsigned phaseTotalMs; /* 0 when the phase has no timer */
    unsigned raceIndex;
    int racerCap;
    unsigned pool; /* bit n set: track n can be voted, picked or drawn */
    int track;     /* the track of the race being set up or run, or GDX_NET_ROOM_TRACK_NONE */
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

/* Host a room on gdx_lan_room_port(). 0 with the reason in error when the port is taken or no profile
 * exists; the session is then idle again. */
int gdx_lan_room_host(char* error, int size);
/* Search the network for rooms. */
void gdx_lan_room_browse(void);
/* Join the room at ip and port, to race or (watch != 0) to watch. 0 when no profile exists or the address is
 * not one a room can have. */
int gdx_lan_room_join(unsigned ip, unsigned port, int watch);
/* Leave or close the room, or stop searching or joining. */
void gdx_lan_room_stop(void);
/* From a closed session back to idle. */
void gdx_lan_room_dismiss(void);

/* The player's own actions. The room decides; the answer shows in a later view. */
void gdx_lan_room_ready(int ready);
void gdx_lan_room_race_next(int raceNext);
/* A ballot in the vote, or the pick of the racer who picks. track is 0 to 23 or GDX_NET_ROOM_TRACK_RANDOM;
 * anything else is ignored. */
void gdx_lan_room_vote(int track);
void gdx_lan_room_pick(int track);

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
void gdx_lan_room_kick(int id);
/* Draw the lottery's track now and not when its timer ends. */
void gdx_lan_room_skip_lottery(void);
/* From the results back to the lobby, which ends the series. Without it the next track choice starts when
 * the results' timer ends. */
void gdx_lan_room_end_series(void);

/* A track's name as the room screens show it, "Mute City". track is 0 to 23 or one of the two special values. */
void gdx_lan_room_track_name(int track, char* out, int size);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_ROOM_VIEW_H */
