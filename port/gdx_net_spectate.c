/* port/gdx_net_spectate.c -- WATCH in a G-NET room's race. See gdx_net_spectate.h.
 * Copyright (c) Zorkats */

#include "gdx_net_spectate.h"

#include <stdio.h>

static int sOn;
static int sTarget;    /* the watched racer id */
static int sCinematic; /* 1 the trackside cinematic, 0 the race camera */
static unsigned sGeneration;

/* The order L and R go round in: by race position, and by racer id where two positions are alike. */
static int order_key(const GdxNetSpectateHuman* human) {
    return human->position * (GDX_NET_SPECTATE_HUMANS * 8) + human->racer;
}

/* The index of racer in the field, or -1. */
static int index_of(const GdxNetSpectateField* field, int racer) {
    int i;

    for (i = 0; i < field->count; i++) {
        if (field->human[i].racer == racer) {
            return i;
        }
    }
    return -1;
}

/* Whether the human at i can be watched: not this copy's own, and still driving unless any is set. */
static int can_watch(const GdxNetSpectateField* field, int i, int any) {
    return (i != field->self) && (any || field->human[i].driving);
}

/* The human nearest key: the nearest before it (ahead in the race) or, with none, the nearest after it.
   -1 when no human qualifies. */
static int nearest(const GdxNetSpectateField* field, int key, int any) {
    int ahead = -1;
    int behind = -1;
    int i;

    for (i = 0; i < field->count; i++) {
        int k = order_key(&field->human[i]);

        if (!can_watch(field, i, any) || (k == key)) {
            continue;
        }
        if (k < key) {
            if ((ahead < 0) || (k > order_key(&field->human[ahead]))) {
                ahead = i;
            }
        } else if ((behind < 0) || (k < order_key(&field->human[behind]))) {
            behind = i;
        }
    }
    return (ahead >= 0) ? ahead : behind;
}

/* The next driving human round the field from key: after it (next) or before it (!next), wrapping at the
   end. -1 when nobody else drives. */
static int round_from(const GdxNetSpectateField* field, int key, int next) {
    int step = -1;
    int wrap = -1;
    int i;

    for (i = 0; i < field->count; i++) {
        int k = order_key(&field->human[i]);

        if (!can_watch(field, i, 0) || (k == key)) {
            continue;
        }
        if (next ? (k > key) : (k < key)) {
            if ((step < 0) || (next ? (k < order_key(&field->human[step])) : (k > order_key(&field->human[step])))) {
                step = i;
            }
        }
        if ((wrap < 0) || (next ? (k < order_key(&field->human[wrap])) : (k > order_key(&field->human[wrap])))) {
            wrap = i;
        }
    }
    return (step >= 0) ? step : wrap;
}

static void watch_racer(int racer) {
    if (racer != sTarget) {
        sTarget = racer;
        sGeneration++;
    }
}

int gdx_net_spectate_watch(void) {
    GdxNetSpectateField field;
    int key;
    int pick;

    if (sOn) {
        return 1;
    }
    if (!gdx_net_race_spectate_field(&field)) {
        return 0;
    }
    /* Without a machine of its own in the field the copy stands before the leader. */
    key = ((field.self >= 0) && (field.self < field.count)) ? order_key(&field.human[field.self]) : -1;
    pick = nearest(&field, key, 0);
    if (pick < 0) {
        pick = nearest(&field, key, 1);
    }
    if (pick < 0) {
        return 0;
    }
    sOn = 1;
    sTarget = field.human[pick].racer;
    sCinematic = 0;
    sGeneration++;
    return 1;
}

void gdx_net_spectate_frame(int prev, int next, int toggle) {
    GdxNetSpectateField field;
    int at;
    int key;
    int pick;

    if (!sOn) {
        return;
    }
    if (!gdx_net_race_spectate_field(&field)) {
        gdx_net_spectate_reset();
        return;
    }
    at = index_of(&field, sTarget);
    key = (at >= 0) ? order_key(&field.human[at]) : -1;
    /* The watched machine finished or is out: on to the nearest one still driving, if any. */
    if ((at < 0) || !field.human[at].driving) {
        pick = nearest(&field, key, 0);
        if (pick >= 0) {
            watch_racer(field.human[pick].racer);
            at = pick;
            key = order_key(&field.human[at]);
        }
    }
    if (prev != next) {
        pick = round_from(&field, key, next);
        if (pick >= 0) {
            watch_racer(field.human[pick].racer);
        }
    }
    if (toggle) {
        sCinematic = !sCinematic;
        sGeneration++;
    }
}

int gdx_net_spectate_on(void) {
    return sOn;
}

int gdx_net_spectate_racer(int cameraId) {
    return (sOn && (cameraId == 0)) ? sTarget : cameraId;
}

int gdx_net_spectate_cinematic(void) {
    return sOn && sCinematic;
}

unsigned gdx_net_spectate_generation(void) {
    return sGeneration;
}

void gdx_net_spectate_label(char* out, int size) {
    char name[GDX_NET_SPECTATE_NAME_BYTES];

    if (size <= 0) {
        return;
    }
    out[0] = '\0';
    if (!sOn) {
        return;
    }
    name[0] = '\0';
    gdx_net_race_spectate_name(sTarget, name, (int) sizeof(name));
    name[sizeof(name) - 1] = '\0';
    snprintf(out, (size_t) size, "WATCHING %s", name);
}

const char* gdx_net_spectate_hint(void) {
    return sOn ? "L R  PLAYER    Z  CAMERA" : "";
}

void gdx_net_spectate_reset(void) {
    if (sOn) {
        sGeneration++;
    }
    sOn = 0;
    sTarget = 0;
    sCinematic = 0;
}
