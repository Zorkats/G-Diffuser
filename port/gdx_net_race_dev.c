// Copyright (c) Zorkats
//
// port/gdx_net_race_dev.c: developer switches of the online race. They live here because the bridge
// (gdx_net_race_bridge.c) is built against the decomp headers, whose stdlib.h has no getenv.
//
//   GDX_NET_PUPPET_ECHO=<ticks>   In a one-player Grand Prix or Practice race, the racer in slot 1 is
//                                 not driven by the CPU: it shows your own machine as another copy of
//                                 the game would, from poses that took <ticks> to arrive (1 to 600, 60
//                                 ticks a second), one lane to the side. Checks remote racers without a
//                                 second copy. A copy guesses a remote machine ahead to where it is now,
//                                 so the echo rides beside you; where it swings out or falls back is
//                                 where the guess is wrong. Past 40 ticks it is no longer guessed.
//   GDX_NET_DEV_AUTOPILOT=1       In an online race the game drives your machine, the way it does once
//                                 you have crossed the line. A scripted copy can then finish a race,
//                                 which checks finish reports and the results without a driver.
//   GDX_NET_DEV_CONTACTS=1        In an online race, one line goes to the port log each time a machine
//                                 this copy drives first touches another one, unless both are CPUs. The
//                                 logs of two copies then say whether both saw the same contact.
//   GDX_NET_DEV_TRACE=<file>      In an online race, every tick this copy writes where it has every
//                                 machine: the host clock (ms), the race tick, the race slot, 1 when this
//                                 copy drives it, and x y z. Two copies' files then say how far a machine
//                                 is shown from where its owner had it at the same moment.
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>

static FILE* sTrace;
static int sTraceTicks;

int gdx_net_race_dev_trace_on(void) {
    static int sRead;

    if (!sRead) {
        const char* env = getenv("GDX_NET_DEV_TRACE");

        sRead = 1;
        if (env != NULL && env[0] != '\0') {
            sTrace = fopen(env, "w");
            if (sTrace != NULL) {
                setvbuf(sTrace, NULL, _IOFBF, 1 << 16);
            }
        }
    }
    return sTrace != NULL;
}

void gdx_net_race_dev_trace(unsigned hostMs, int raceTicks, int slot, int driven, float x, float y, float z) {
    if (sTrace != NULL) {
        fprintf(sTrace, "%u %d %d %d %.2f %.2f %.2f\n", hostMs, raceTicks, slot, driven, x, y, z);
    }
}

// A tick's lines are written. A copy stopped from outside loses at most half a second of them.
void gdx_net_race_dev_trace_tick(void) {
    if (sTrace != NULL && ++sTraceTicks >= 30) {
        sTraceTicks = 0;
        fflush(sTrace);
    }
}

int gdx_net_race_dev_echo_ticks(void) {
    static int sRead;
    static int sTicks;

    if (!sRead) {
        const char* env = getenv("GDX_NET_PUPPET_ECHO");

        sRead = 1;
        if (env != NULL && env[0] != '\0') {
            long ticks = strtol(env, NULL, 10);

            if (ticks > 600) {
                ticks = 600;
            }
            sTicks = ticks > 0 ? (int)ticks : 0;
        }
    }
    return sTicks;
}

int gdx_net_race_dev_contacts(void) {
    static int sRead;
    static int sOn;

    if (!sRead) {
        const char* env = getenv("GDX_NET_DEV_CONTACTS");

        sRead = 1;
        sOn = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return sOn;
}

int gdx_net_race_dev_autopilot(void) {
    static int sRead;
    static int sOn;

    if (!sRead) {
        const char* env = getenv("GDX_NET_DEV_AUTOPILOT");

        sRead = 1;
        sOn = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return sOn;
}
