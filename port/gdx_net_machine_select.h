/* port/gdx_net_machine_select.h -- the G-NET lock-in screen, game side (O3 slice S1b).
 * Copyright (c) Zorkats
 *
 * When the room reaches its lock-in state, a racer waiting on the main menu is taken through the stock
 * machine select and machine settings screens. The second OK on the settings screen sends the machine, its
 * colour and the engine setting to the room, and the screen then waits: the machine name reads PLEASE WAIT.
 * The room's timer stands in the lower left corner of both screens the whole time, and once the player has
 * locked in the count of racers who have stands beside it. The screen leaves when the room moves on, by
 * everyone locking in or by the timer.
 *
 * The room decides everything (net/gdx_net_room.h). This module only tracks whether the screen is up and
 * turns the room's numbers into text. It never sees a socket: the five functions below are its whole view of
 * the room, the menu and the race, so the tests replace them.
 *
 * Game thread only. */

#ifndef GDX_NET_MACHINE_SELECT_H
#define GDX_NET_MACHINE_SELECT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxNetLockInRoom {
    int inRoom;         /* hosting a room or joined to one */
    int phaseMachine;   /* the room is in its lock-in state */
    unsigned raceIndex; /* which race of the room this lock-in belongs to */
    int entrant;        /* the local player is on this race's grid */
    int locked;         /* the room has the local player's lock */
    unsigned leftMs;    /* time left on the room's timer */
    int lockedCount;    /* racers who have locked in */
    int racers;         /* racers on the grid */
    int track;          /* the stock course of this race */
} GdxNetLockInRoom;

/* The room and the menu, as this module sees them (gdx_lan_lobby.cpp, gdx_menu.cpp). */
void gdx_lan_lock_in_room(GdxNetLockInRoom* out);
void gdx_lan_lock_in_send(int machine, int skin, int enginePercent);
/* The port menu steps aside for the lock-in screen. */
void gdx_menu_request_close(void);
/* The host's driving options become the rules of the race this lock-in is for (gdx_lan_lobby.cpp). */
void gdx_lan_race_capture_rules(void);
/* The race after the lock-in (gdx_net_race_bridge.c): 0 when this player has none, 1 while its grid is on
 * the way, 2 when the next Practice race is the room's. */
int gdx_net_race_arm(unsigned raceIndex);

/* Main menu, on its mode grid: 1 when the lock-in screen opens now. The caller then enters machine select on
 * course gdx_net_machine_select_track(). Calling it also records that the screen is not up, which is true
 * whenever the main menu runs. */
int gdx_net_machine_select_poll_open(void);
int gdx_net_machine_select_track(void);

/* 1 while machine select or machine settings is the room's lock-in screen and not the stock one. */
int gdx_net_machine_select_active(void);

/* Machine select and machine settings, once per tick: 0 while the screen stays. 1 when the room has moved
 * on without this player and the screen must go back to the main menu, which shows the room screen again.
 * 2 when the room's race is next and the screen must start a Practice race. Either way it stops being
 * active. */
int gdx_net_machine_select_should_leave(void);

/* 1 once the local player has locked in. The screen takes no more input. */
int gdx_net_machine_select_waiting(void);

/* The second OK on machine settings. character is a roster index, skin 0..3, engine 0..1. A lock is final,
 * so a second call for the same race does nothing. */
void gdx_net_machine_select_confirm(int character, int skin, float engine);

/* Text in glyphs the menu fonts have: capitals, digits and a colon. Valid until the next call.
 * The status line is the timer, "0:42", while the player chooses, and the timer and the count of locked
 * racers, "0:42  3 OF 8", once the player has locked in. */
const char* gdx_net_machine_select_timer_text(void);
const char* gdx_net_machine_select_status_text(void);
const char* gdx_net_machine_select_wait_text(void);

/* The pieces, for the tests. */
int gdx_net_lock_in_engine_percent(float engine);
void gdx_net_lock_in_format_timer(unsigned leftMs, char* out, int size);
void gdx_net_lock_in_format_count(int locked, int racers, char* out, int size);

#ifdef __cplusplus
}
#endif

#endif /* GDX_NET_MACHINE_SELECT_H */
