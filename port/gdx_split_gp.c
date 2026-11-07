/* port/gdx_split_gp.c -- split-screen Grand Prix rules. See gdx_split_gp.h.
 * Copyright (c) Zorkats
 */

#include "gdx_split_gp.h"

static unsigned players_mask(int numPlayers) {
    return (1u << (unsigned) numPlayers) - 1u;
}

static int players_ok(int numPlayers) {
    return numPlayers >= 1 && numPlayers <= GDX_SPLIT_GP_MAX_PLAYERS;
}

int gdx_split_gp_field(int numPlayers, int requested) {
    if (numPlayers < 1) {
        numPlayers = 1;
    } else if (numPlayers > GDX_SPLIT_GP_MAX_PLAYERS) {
        numPlayers = GDX_SPLIT_GP_MAX_PLAYERS;
    }
    if (requested < numPlayers) {
        return numPlayers;
    }
    if (requested > GDX_SPLIT_GP_MAX_RACERS) {
        return GDX_SPLIT_GP_MAX_RACERS;
    }
    return requested;
}

int gdx_split_gp_race_over(int numPlayers, unsigned finishedMask, unsigned retiredMask, unsigned outMask) {
    unsigned in;

    if (!players_ok(numPlayers)) {
        return 0;
    }
    in = players_mask(numPlayers) & ~outMask;
    return (in & ~(finishedMask | retiredMask)) == 0;
}

int gdx_split_gp_resolve(int numPlayers, unsigned finishedMask, unsigned retiredMask, unsigned* outMask,
                         short* lives, unsigned* lostMask) {
    unsigned in;
    int p;

    if (!players_ok(numPlayers) || outMask == 0 || lives == 0 || lostMask == 0) {
        return -1;
    }
    *lostMask = 0;
    in = players_mask(numPlayers) & ~*outMask;
    for (p = 0; p < numPlayers; p++) {
        const unsigned bit = 1u << (unsigned) p;

        if (!(in & bit) || (finishedMask & bit) || !(retiredMask & bit)) {
            continue;
        }
        if (lives[p] > 0) {
            lives[p]--;
            *lostMask |= bit;
        } else {
            *outMask |= bit;
        }
    }
    if (in & finishedMask) {
        return GDX_SPLIT_GP_COUNTS;
    }
    if (players_mask(numPlayers) & ~*outMask) {
        return GDX_SPLIT_GP_RETRY;
    }
    return GDX_SPLIT_GP_GAME_OVER;
}

int gdx_split_gp_retry_allowed(int numPlayers, unsigned finishedMask, unsigned outMask, const short* lives) {
    int p;

    if (!players_ok(numPlayers) || lives == 0) {
        return 0;
    }
    for (p = 0; p < numPlayers; p++) {
        const unsigned bit = 1u << (unsigned) p;

        if (!(outMask & bit) && !(finishedMask & bit) && (lives[p] <= 0)) {
            return 0;
        }
    }
    return 1;
}

void gdx_split_gp_retry_cost(int numPlayers, unsigned finishedMask, unsigned outMask, short* lives,
                             unsigned* lostMask) {
    int p;

    if (!players_ok(numPlayers) || lives == 0 || lostMask == 0) {
        return;
    }
    *lostMask = 0;
    for (p = 0; p < numPlayers; p++) {
        const unsigned bit = 1u << (unsigned) p;

        if ((outMask & bit) || (finishedMask & bit) || (lives[p] <= 0)) {
            continue;
        }
        lives[p]--;
        *lostMask |= bit;
    }
}

int gdx_split_gp_back_grid(int* positions, int total, int numPlayers) {
    int seen[GDX_SPLIT_GP_MAX_RACERS + 1];
    int i;
    int j;
    int ahead;

    if (positions == 0 || !players_ok(numPlayers) || total < numPlayers || total > GDX_SPLIT_GP_MAX_RACERS) {
        return 0;
    }
    for (i = 0; i <= total; i++) {
        seen[i] = 0;
    }
    for (i = 0; i < total; i++) {
        if (positions[i] < 1 || positions[i] > total || seen[positions[i]]) {
            return 0;
        }
        seen[positions[i]] = 1;
    }
    /* Each CPU's new place is its rank among the CPUs; seen[] is reused to hold the new grid. */
    for (i = numPlayers; i < total; i++) {
        ahead = 0;
        for (j = numPlayers; j < total; j++) {
            if (positions[j] < positions[i]) {
                ahead++;
            }
        }
        seen[i] = ahead + 1;
    }
    for (i = numPlayers; i < total; i++) {
        positions[i] = seen[i];
    }
    for (i = 0; i < numPlayers; i++) {
        positions[i] = total - i;
    }
    return 1;
}
