/* port/gdx_net_machine_select.c -- the G-NET lock-in screen, game side. See gdx_net_machine_select.h.
 * Copyright (c) Zorkats
 */

#include "gdx_net_machine_select.h"

#include <stdio.h>

#define ROSTER_MACHINES 30 /* net::lobby::kRosterMachines */
#define STOCK_TRACKS 24    /* net::lobby::kStockTracks */
#define MACHINE_SKINS 4    /* net::lobby::kMachineSkins */

static int sOpen;           /* the lock-in screen is up */
static int sSent;           /* this copy sent its lock for sRaceIndex */
static unsigned sRaceIndex; /* the race the screen was opened for */
static GdxNetLockInRoom sRoom;
static char sTimerText[8];
static char sStatusText[24];

static int gdx_net_lock_in_wanted(const GdxNetLockInRoom* room) {
    return room->inRoom && room->phaseMachine && room->entrant;
}

int gdx_net_machine_select_poll_open(void) {
    sOpen = 0;
    gdx_lan_lock_in_room(&sRoom);
    if (!gdx_net_lock_in_wanted(&sRoom) || sRoom.locked) {
        return 0;
    }
    /* The lock for this race is on its way to the host. */
    if (sSent && (sRoom.raceIndex == sRaceIndex)) {
        return 0;
    }
    if ((sRoom.track < 0) || (sRoom.track >= STOCK_TRACKS)) {
        return 0;
    }
    sOpen = 1;
    sSent = 0;
    sRaceIndex = sRoom.raceIndex;
    gdx_lan_race_capture_rules();
    gdx_menu_request_close();
    return 1;
}

int gdx_net_machine_select_track(void) {
    return sRoom.track;
}

int gdx_net_machine_select_active(void) {
    return sOpen;
}

int gdx_net_machine_select_should_leave(void) {
    int race;

    if (!sOpen) {
        return 0;
    }
    gdx_lan_lock_in_room(&sRoom);
    if (gdx_net_lock_in_wanted(&sRoom) && (sRoom.raceIndex == sRaceIndex)) {
        return 0;
    }
    race = gdx_net_race_arm(sRaceIndex);
    if (race == 1) {
        return 0; /* the grid follows the room state by a message at most */
    }
    sOpen = 0;
    if (race == 2) {
        return 2;
    }
    /* Back to the main menu, whose room screen also says why when the room closed. */
    return 1;
}

int gdx_net_machine_select_waiting(void) {
    return sOpen && (sSent || sRoom.locked);
}

void gdx_net_machine_select_confirm(int character, int skin, float engine) {
    if (!sOpen || gdx_net_machine_select_waiting()) {
        return;
    }
    if ((character < 0) || (character >= ROSTER_MACHINES)) {
        character = 0;
    }
    if ((skin < 0) || (skin >= MACHINE_SKINS)) {
        skin = 0;
    }
    gdx_lan_lock_in_send(character, skin, gdx_net_lock_in_engine_percent(engine));
    sSent = 1;
}

int gdx_net_lock_in_engine_percent(float engine) {
    int percent;

    /* The comparison is written so a NaN lands on the middle of the slider. */
    if (!(engine >= 0.0f)) {
        return (engine < 0.0f) ? 0 : 50;
    }
    if (engine >= 1.0f) {
        return 100;
    }
    percent = (int) (engine * 100.0f + 0.5f);
    return (percent > 100) ? 100 : percent;
}

void gdx_net_lock_in_format_timer(unsigned leftMs, char* out, int size) {
    /* Rounded up, so the clock reads 0:00 only when the time is gone. */
    unsigned seconds = (leftMs / 1000u) + ((leftMs % 1000u) != 0u ? 1u : 0u);

    if (seconds > 599u) {
        seconds = 599u; /* 9:59, the most four glyphs hold */
    }
    snprintf(out, (size_t) size, "%u:%02u", seconds / 60u, seconds % 60u);
}

void gdx_net_lock_in_format_count(int locked, int racers, char* out, int size) {
    if (racers < 0) {
        racers = 0;
    }
    if (racers > 99) {
        racers = 99;
    }
    if (locked < 0) {
        locked = 0;
    }
    if (locked > racers) {
        locked = racers;
    }
    /* The menu fonts have no slash. */
    snprintf(out, (size_t) size, "%d OF %d", locked, racers);
}

const char* gdx_net_machine_select_timer_text(void) {
    gdx_net_lock_in_format_timer(sRoom.leftMs, sTimerText, (int) sizeof(sTimerText));
    return sTimerText;
}

const char* gdx_net_machine_select_status_text(void) {
    char count[12];

    /* A player who is still choosing reads the clock and nothing else. */
    if (!gdx_net_machine_select_waiting()) {
        return gdx_net_machine_select_timer_text();
    }
    gdx_net_lock_in_format_count(sRoom.lockedCount, sRoom.racers, count, (int) sizeof(count));
    snprintf(sStatusText, sizeof(sStatusText), "%s  %s", gdx_net_machine_select_timer_text(), count);
    return sStatusText;
}

const char* gdx_net_machine_select_wait_text(void) {
    return "PLEASE WAIT";
}
