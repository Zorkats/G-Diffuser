/* port/gdx_net_race_room.h -- the room, as an online race sees it (O3 slice S3).
 * Copyright (c) Zorkats
 *
 * The race bridge (gdx_net_race_bridge.c) is built against the decomp headers and runs on the game thread.
 * The room lives on the lobby worker thread (gdx_lan_lobby.cpp). These calls are all the bridge knows of
 * it: each one copies a few numbers under the worker's lock, or queues one message for the host.
 *
 * Plain C, and no <stdint.h>: the decomp's libc headers do not have it. */

#ifndef GDX_NET_RACE_ROOM_H
#define GDX_NET_RACE_ROOM_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_NET_RACE_HUMANS 12       /* net::lobby::kMaxRacers */
#define GDX_NET_RACE_RECORD_BYTES 32 /* net::lobby::kMachineRecordBytes, the game's sizeof(CustomMachine) */

enum {
    GDX_NET_RACE_PHASE_NONE = 0,      /* no race: the lobby, a vote, the lock-in screen, the results */
    GDX_NET_RACE_PHASE_LOADING = 1,   /* every racer loads the track */
    GDX_NET_RACE_PHASE_COUNTDOWN = 2, /* the race intro */
    GDX_NET_RACE_PHASE_RACING = 3
};

typedef struct GdxNetRaceRoom {
    int inRoom; /* hosting a room or joined to one */
    int host;   /* this copy hosts it, so it runs the CPUs */
    int phase;  /* GDX_NET_RACE_PHASE_* */
    unsigned raceIndex;
    int track;
    int cpusOut;  /* the host's game left this race: the CPU machines are out of it on every copy */
    int onGrid;   /* the local player has a place in this race */
    int haveGrid; /* the grid of this race arrived and the local player is on it; the rest is valid */
    unsigned seed;
    int humans;
    int cpus;
    int self; /* the local player's place among the humans, 0 at the front of their rows */
    unsigned char machine[GDX_NET_RACE_HUMANS];
    unsigned char skin[GDX_NET_RACE_HUMANS];
    unsigned char hasRecord[GDX_NET_RACE_HUMANS];                      /* the human brought a machine record */
    unsigned char record[GDX_NET_RACE_HUMANS][GDX_NET_RACE_RECORD_BYTES]; /* its CustomMachine bytes */
    int laps;
    int ignoreTrackSettings;
    int lavaNoKill;
    float magnetRange;
    float magnetStrength;
    int boostFrames; /* 0 is the stock boost length */
} GdxNetRaceRoom;

void gdx_lan_race_room(GdxNetRaceRoom* out);

// The bridge: the Create Machine record of the local player's machine for character, as the game holds it.
// 1 and the 32 bytes in out when that machine was edited, else 0 (a stock or super machine).
int gdx_net_race_local_record(int character, unsigned char* out);

/* Milliseconds since the start time of that race on the host clock, negative before it. 0 while the room
 * has sent no start for that race or the host clock is not known yet; elapsedMs is then left alone. */
int gdx_lan_race_elapsed(unsigned raceIndex, int* elapsedMs);

/* The host clock now, in milliseconds: the one clock every copy in the room can read. It wraps. 0 while
 * this copy is in no room or has not measured the host clock yet; hostMs is then left alone. */
int gdx_lan_race_clock(unsigned* hostMs);

/* This copy has loaded the track: digest is 32 bytes over its control points. */
void gdx_lan_race_ready(unsigned raceIndex, const unsigned char* digest, unsigned loadMs);

/* The host's game leaves the race raceIndex while the others drive on. The room tells every copy that
 * the CPU machines, which only the host's game drives, are out of that race. Does nothing on a client. */
void gdx_lan_race_host_left(unsigned raceIndex);

/* The host player leaves the room and the room goes on (D28.5). The room runs a race in progress to its
 * results in the background, then goes to the next host; outside a race it goes at once. This copy is out
 * of the room from this call on: the view is idle. Call it after the race reported this copy's machine and
 * its CPUs, so the room hears both first. Does nothing on a client or in no room. */
void gdx_lan_room_hand_over(void);

/* The custom track of the room's next race, verified, from memory: its 32-byte identity in id and its content
 * bytes in out. 1 when the room's track is a custom track, this copy holds all of it, its identity is the
 * room's and it fits capacity. 0 otherwise; nothing is written then. */
int gdx_lan_race_track(unsigned char* id, unsigned char* out, unsigned long capacity, unsigned long* length);

/* The clock the room runs on, in milliseconds. It wraps. */
unsigned gdx_lan_now_ms(void);

/* The host's own driving options become the rules of the next race, and its CPU options (AI aggression,
 * track-hazard avoidance) the room's. Does nothing on a client. */
void gdx_lan_race_capture_rules(void);

/* How a race ended for one player, as the host's table has it (net::lobby::ResultKind). */
enum {
    GDX_NET_RACE_RESULT_FINISHED = 0,
    GDX_NET_RACE_RESULT_UNFINISHED = 1, /* still driving */
    GDX_NET_RACE_RESULT_RETIRED = 2,
    GDX_NET_RACE_RESULT_NONE = 3 /* the table does not list that player */
};

typedef struct GdxNetRaceResults {
    int humans;
    unsigned char kind[GDX_NET_RACE_HUMANS]; /* GDX_NET_RACE_RESULT_*, by place among the humans */
    unsigned timeMs[GDX_NET_RACE_HUMANS];    /* the host's time for a finisher, else 0 */
} GdxNetRaceResults;

/* Our own race ended: over the line after timeMs of racing, or out of it (timeMs is then not read). The
 * host decides whether it counts; its answer is the next table. */
void gdx_lan_race_finish(unsigned raceIndex, int retired, unsigned timeMs);

/* The host game's order of the humans, first to last, each named by its place among the humans. It places
 * whoever is still driving when the race is called. Does nothing on a client. */
void gdx_lan_race_standing(unsigned raceIndex, const unsigned char* humans, int count);

/* The host's table of that race so far. 0 while there is none; out is then left alone. */
int gdx_lan_race_results(unsigned raceIndex, GdxNetRaceResults* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_RACE_ROOM_H */
