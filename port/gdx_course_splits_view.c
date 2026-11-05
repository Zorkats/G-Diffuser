/* port/gdx_course_splits_view.c -- see gdx_course_splits_view.h. */

#include "gdx_course_splits_view.h"

#include "gdx_course_meta.h"
#include "gdx_course_splits.h"
#include "port_log.h"

#include <string.h>

/* decomp/src/game/racer.c: the course file id of the course about to start, 1 when out holds one. */
extern int gdx_racer_course_file_id(char* out, int outCap);

/* cpu.c compares the pace now with the pace 0x11 ms earlier. */
#define CPU_STEP_MS 17
/* EXPERT in decomp/include/fzx_game.h. */
#define DIFFICULTY_EXPERT 2
/* The most race laps gdx_splits_pace_laps takes. */
#define PACE_RACE_LAPS_MAX 255

/* The player's run in progress. */
typedef struct SplitsRun {
    int active;           /* recording this race, and nothing has broken the lap count */
    int started;          /* the first frame was seen */
    int nodesOk;          /* the checkpoint recorder can still finish */
    int lap;              /* the racer's lap and lapsCompleted on the last frame */
    int lapsCompleted;
    double prevAt;        /* lap share and race time on the last frame */
    uint32_t prevTime;
    float nodeAt[GDX_SPLITS_NODES_MAX];
    GdxSplitsRecorder nodes; /* lap and checkpoint times */
    GdxSplitsRecorder laps;  /* lap times only, the fallback */
} SplitsRun;

/* The staff run the CPUs follow in this race. */
typedef struct SplitsPace {
    int active;
    int raceLaps;
    float nodeAt[GDX_SPLITS_NODES_MAX];
    GdxCourseSplits splits;
} SplitsPace;

static SplitsRun sRun;
static SplitsPace sPace;

void gdx_splits_view_race_init(void) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    int nodes;

    memset(&sRun, 0, sizeof(sRun));
    if (!gdx_splits_course_recordable() || !gdx_racer_course_file_id(courseId, sizeof(courseId))) {
        return;
    }
    nodes = gdx_splits_course_nodes(sRun.nodeAt, GDX_SPLITS_NODES_MAX);
    gdx_splits_rec_begin(&sRun.nodes, nodes);
    gdx_splits_rec_begin(&sRun.laps, 0);
    sRun.nodesOk = nodes > 0;
    sRun.active = 1;
}

static void track_nodes(double prevAt, uint32_t prevTime, double at, uint32_t time) {
    if (sRun.nodesOk) {
        gdx_splits_rec_track(&sRun.nodes, sRun.nodeAt, prevAt, prevTime, at, time);
    }
}

void gdx_splits_view_player_moved(int lap, int lapsCompleted, float lapShare, int raceTime, int completedLapsTime) {
    const double at = (double) lapShare;
    const uint32_t time = (raceTime > 0) ? (uint32_t) raceTime : 0u;
    const int crossed = lap == sRun.lap + 1;

    if (!sRun.active) {
        return;
    }
    if (!sRun.started) {
        sRun.started = 1;
        sRun.lap = lap;
        sRun.lapsCompleted = lapsCompleted;
        sRun.prevAt = at;
        sRun.prevTime = time;
        return;
    }
    /* Lap 0 is the run-up behind the line: its checkpoints belong to no lap. */
    if (lap == sRun.lap) {
        if (lap >= 1) {
            track_nodes(sRun.prevAt, sRun.prevTime, at, time);
        }
    } else if (crossed) {
        /* The old lap's part of the frame runs on past 1. */
        if (sRun.lap >= 1) {
            track_nodes(sRun.prevAt, sRun.prevTime, at + 1.0, time);
        }
    } else {
        /* Back over the line, so the checkpoints of the lap are passed twice. Lap times still hold. */
        sRun.nodesOk = 0;
    }
    if (lapsCompleted != sRun.lapsCompleted) {
        if ((lapsCompleted != sRun.lapsCompleted + 1) || (completedLapsTime <= 0)) {
            sRun.active = 0;
            return;
        }
        gdx_splits_rec_lap(&sRun.laps, (uint32_t) completedLapsTime);
        if (sRun.nodesOk) {
            gdx_splits_rec_lap(&sRun.nodes, (uint32_t) completedLapsTime);
        }
    }
    /* The new lap's part of the frame starts below 0. */
    if (crossed && (lap >= 1)) {
        track_nodes(sRun.prevAt - 1.0, sRun.prevTime, at, time);
    }
    sRun.lap = lap;
    sRun.lapsCompleted = lapsCompleted;
    sRun.prevAt = at;
    sRun.prevTime = time;
}

static int finish_matches(const GdxSplitsRecorder* r, int raceLaps, uint32_t raceTime, GdxCourseSplits* out) {
    unsigned char bytes[GDX_SPLITS_VALUE_MAX_BYTES];

    return (gdx_splits_rec_finish(r, out) == 0) && (out->laps == raceLaps) &&
           (out->lapEnd[out->laps - 1] == raceTime) && (gdx_splits_encode(out, bytes, sizeof(bytes)) > 0);
}

int gdx_splits_view_finished(int raceLaps, int raceTime, struct GdxCourseSplits* out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!sRun.active || !sRun.started || (raceTime <= 0)) {
        return 0;
    }
    if (sRun.nodesOk && finish_matches(&sRun.nodes, raceLaps, (uint32_t) raceTime, out)) {
        return 1;
    }
    if (finish_matches(&sRun.laps, raceLaps, (uint32_t) raceTime, out)) {
        return 1;
    }
    memset(out, 0, sizeof(*out));
    return 0;
}

/* Keeps the lap times of a record and drops its checkpoints. */
static void drop_nodes(GdxCourseSplits* splits) {
    splits->nodes = 0;
    memset(splits->node, 0, sizeof(splits->node));
}

void gdx_splits_view_pace_begin(int creatorPaced, int difficulty) {
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;

    memset(&sPace, 0, sizeof(sPace));
    if (!creatorPaced || (difficulty < DIFFICULTY_EXPERT) || !gdx_racer_course_file_id(courseId, sizeof(courseId)) ||
        !gdx_meta_store_get(courseId, &meta) || !gdx_meta_get_splits(&meta, &sPace.splits)) {
        return;
    }
    sPace.raceLaps = gdx_splits_course_laps();
    if ((sPace.raceLaps < 2) || (sPace.raceLaps > PACE_RACE_LAPS_MAX)) {
        return;
    }
    if ((sPace.splits.nodes > 0) &&
        (gdx_splits_course_nodes(sPace.nodeAt, GDX_SPLITS_NODES_MAX) != sPace.splits.nodes)) {
        gdx_port_logf("[splits] %s: checkpoints changed since the staff run, CPUs follow its lap times\n",
                      courseId);
        drop_nodes(&sPace.splits);
    }
    sPace.active = 1;
    gdx_port_logf("[splits] %s: CPUs follow the staff run, %d laps, %d checkpoints\n", courseId, sPace.splits.laps,
                  sPace.splits.nodes);
}

int gdx_splits_view_cpu_pace(int elapsed, int window, float lapLength, float raceDistance, float* outBehind,
                             float* outBehindBefore) {
    const float* nodeAt;
    double now;
    double before;

    if (!sPace.active || (window <= 0) || !(lapLength > 0.0f) || (outBehind == NULL) || (outBehindBefore == NULL)) {
        return 0;
    }
    nodeAt = (sPace.splits.nodes > 0) ? sPace.nodeAt : NULL;
    if ((gdx_splits_pace_laps(&sPace.splits, nodeAt, sPace.raceLaps, 1, (double) elapsed / (double) window, &now) !=
         0) ||
        (gdx_splits_pace_laps(&sPace.splits, nodeAt, sPace.raceLaps, 1,
                              (double) (elapsed - CPU_STEP_MS) / (double) window, &before) != 0)) {
        return 0;
    }
    *outBehind = (float) (now * (double) lapLength) - raceDistance;
    *outBehindBefore = (float) (before * (double) lapLength) - raceDistance;
    return 1;
}
