// port/gdx_course_splits.c: split-time model, codec, run recorder and target query. Pure data: no file IO, no RNG,
// no networking. The byte layout and the rules are described in gdx_course_splits.h.

#include "gdx_course_splits.h"

#include <string.h>

// Largest lap count gdx_splits_pace_laps accepts.
#define PACE_LAPS_MAX 255

static void put_u16le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void put_u32le(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static uint32_t get_u16le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Start of lap `lap`: the previous lap end, or time 0 for lap 0.
static uint32_t lap_start(const GdxCourseSplits* s, int lap) {
    return lap == 0 ? 0u : s->lapEnd[lap - 1];
}

// Time of the last accepted point in the run: the last node of the lap in progress, or the lap start before any node.
static uint32_t rec_last_time(const GdxSplitsRecorder* r) {
    if (r->nextNode > 0) {
        return r->s.node[r->lap][r->nextNode - 1];
    }
    return lap_start(&r->s, r->lap);
}

// Staff time at the end of race lap k - 1, so k = 0 is the start. A lap past the recorded laps repeats the last one.
static double staff_lap_end(const GdxCourseSplits* s, int k) {
    const uint32_t finish = s->lapEnd[s->laps - 1];
    if (k <= 0) {
        return 0.0;
    }
    if (k <= s->laps) {
        return (double)s->lapEnd[k - 1];
    }
    return (double)finish + (double)(k - s->laps) * (double)(finish - lap_start(s, s->laps - 1));
}

// 1 when nodeAt holds `nodes` finite positions in (0, 1), strictly increasing.
static int node_positions_ok(const float* nodeAt, int nodes) {
    double last = 0.0;
    int n;
    if (nodes == 0) {
        return 1;
    }
    if (!nodeAt) {
        return 0;
    }
    for (n = 0; n < nodes; n++) {
        const double at = nodeAt[n];
        if (!(at > last) || !(at < 1.0)) {
            return 0;
        }
        last = at;
    }
    return 1;
}

int gdx_splits_validate(const GdxCourseSplits* s) {
    int l, n;
    if (!s) {
        return GDX_SPLITS_ERR_ARGS;
    }
    if (s->laps < 0 || s->laps > GDX_SPLITS_LAPS_MAX || s->nodes < 0 || s->nodes > GDX_SPLITS_NODES_MAX) {
        return GDX_SPLITS_ERR_VALUE;
    }
    if (s->laps == 0 && s->nodes != 0) {
        return GDX_SPLITS_ERR_VALUE;
    }
    for (l = 0; l < GDX_SPLITS_LAPS_MAX; l++) {
        if (l >= s->laps && s->lapEnd[l] != 0) {
            return GDX_SPLITS_ERR_VALUE;
        }
        for (n = 0; n < GDX_SPLITS_NODES_MAX; n++) {
            if ((l >= s->laps || n >= s->nodes) && s->node[l][n] != 0) {
                return GDX_SPLITS_ERR_VALUE;
            }
        }
    }
    for (l = 0; l < s->laps; l++) {
        uint32_t prev = lap_start(s, l);
        uint32_t end = s->lapEnd[l];
        if (end <= prev || end > GDX_SPLITS_TIME_MAX) {
            return GDX_SPLITS_ERR_VALUE;
        }
        for (n = 0; n < s->nodes; n++) {
            uint32_t t = s->node[l][n];
            if (t <= prev || t >= end) {
                return GDX_SPLITS_ERR_VALUE;
            }
            prev = t;
        }
    }
    return 0;
}

int gdx_splits_encode(const GdxCourseSplits* s, unsigned char* out, size_t cap) {
    int l, n;
    size_t len, at;
    if (!s || !out) {
        return GDX_SPLITS_ERR_ARGS;
    }
    if (s->laps < 0 || s->laps > GDX_SPLITS_LAPS_MAX || s->nodes < 0 || s->nodes > GDX_SPLITS_NODES_MAX) {
        return GDX_SPLITS_ERR_VALUE;
    }
    for (l = 0; l < s->laps; l++) {
        uint32_t prev = lap_start(s, l);
        for (n = 0; n < s->nodes; n++) {
            int64_t delta = (int64_t)s->node[l][n] - (int64_t)prev;
            if (delta < 1 || delta > 65535) {
                return GDX_SPLITS_ERR_RANGE;
            }
            prev = s->node[l][n];
        }
    }
    if (gdx_splits_validate(s) != 0) {
        return GDX_SPLITS_ERR_VALUE;
    }
    len = 2 + 4 * (size_t)s->laps + 2 * (size_t)s->laps * (size_t)s->nodes;
    if (cap < len) {
        return GDX_SPLITS_ERR_FULL;
    }
    out[0] = (unsigned char)s->laps;
    out[1] = (unsigned char)s->nodes;
    at = 2;
    for (l = 0; l < s->laps; l++) {
        put_u32le(out + at, s->lapEnd[l]);
        at += 4;
    }
    for (l = 0; l < s->laps; l++) {
        uint32_t prev = lap_start(s, l);
        for (n = 0; n < s->nodes; n++) {
            put_u16le(out + at, s->node[l][n] - prev);
            at += 2;
            prev = s->node[l][n];
        }
    }
    return (int)len;
}

int gdx_splits_decode(const unsigned char* in, size_t length, GdxCourseSplits* out) {
    int laps, nodes, l, n;
    size_t at;
    if (!out) {
        return GDX_SPLITS_ERR_ARGS;
    }
    memset(out, 0, sizeof *out);
    if (!in) {
        return GDX_SPLITS_ERR_ARGS;
    }
    if (length < 2) {
        return GDX_SPLITS_ERR_TRUNCATED;
    }
    laps = in[0];
    nodes = in[1];
    if (laps > GDX_SPLITS_LAPS_MAX || nodes > GDX_SPLITS_NODES_MAX || (laps == 0 && nodes != 0)) {
        return GDX_SPLITS_ERR_LAYOUT;
    }
    if (length != 2 + 4 * (size_t)laps + 2 * (size_t)laps * (size_t)nodes) {
        return GDX_SPLITS_ERR_LAYOUT;
    }
    out->laps = laps;
    out->nodes = nodes;
    at = 2;
    for (l = 0; l < laps; l++) {
        out->lapEnd[l] = get_u32le(in + at);
        at += 4;
    }
    for (l = 0; l < laps; l++) {
        uint64_t last = lap_start(out, l);
        for (n = 0; n < nodes; n++) {
            last += get_u16le(in + at);
            at += 2;
            if (last > GDX_SPLITS_TIME_MAX) {
                memset(out, 0, sizeof *out);
                return GDX_SPLITS_ERR_VALUE;
            }
            out->node[l][n] = (uint32_t)last;
        }
    }
    if (gdx_splits_validate(out) != 0) {
        memset(out, 0, sizeof *out);
        return GDX_SPLITS_ERR_VALUE;
    }
    return 0;
}

void gdx_splits_rec_begin(GdxSplitsRecorder* r, int nodes) {
    if (!r) {
        return;
    }
    memset(r, 0, sizeof *r);
    if (nodes < 0 || nodes > GDX_SPLITS_NODES_MAX) {
        r->failed = 1;
        return;
    }
    r->s.nodes = nodes;
}

void gdx_splits_rec_node(GdxSplitsRecorder* r, int node, uint32_t raceTime) {
    if (!r || r->failed) {
        return;
    }
    if (r->lap >= GDX_SPLITS_LAPS_MAX || node < 0 || node >= r->s.nodes) {
        r->failed = 1;
        return;
    }
    if (node < r->nextNode) {
        return;
    }
    if (node > r->nextNode || raceTime <= rec_last_time(r) || raceTime > GDX_SPLITS_TIME_MAX) {
        r->failed = 1;
        return;
    }
    r->s.node[r->lap][node] = raceTime;
    r->nextNode++;
}

void gdx_splits_rec_track(GdxSplitsRecorder* r, const float* nodeAt, double prevAt, uint32_t prevTime, double at,
                          uint32_t raceTime) {
    if (!r || !nodeAt || r->failed || !(at > prevAt) || (raceTime < prevTime)) {
        return;
    }
    while (!r->failed && (r->nextNode < r->s.nodes) && (nodeAt[r->nextNode] <= at)) {
        double share = ((double)nodeAt[r->nextNode] - prevAt) / (at - prevAt);
        if (share < 0.0) {
            share = 0.0;
        }
        gdx_splits_rec_node(r, r->nextNode, prevTime + (uint32_t)(share * (double)(raceTime - prevTime) + 0.5));
    }
}

void gdx_splits_rec_lap(GdxSplitsRecorder* r, uint32_t raceTime) {
    if (!r || r->failed) {
        return;
    }
    if (r->lap >= GDX_SPLITS_LAPS_MAX || r->nextNode != r->s.nodes || raceTime <= rec_last_time(r) ||
        raceTime > GDX_SPLITS_TIME_MAX) {
        r->failed = 1;
        return;
    }
    r->s.lapEnd[r->lap] = raceTime;
    r->lap++;
    r->s.laps = r->lap;
    r->nextNode = 0;
}

int gdx_splits_rec_finish(const GdxSplitsRecorder* r, GdxCourseSplits* out) {
    int l, n;
    if (!out) {
        return GDX_SPLITS_ERR_ARGS;
    }
    memset(out, 0, sizeof *out);
    if (!r) {
        return GDX_SPLITS_ERR_ARGS;
    }
    if (r->failed) {
        return GDX_SPLITS_ERR_VALUE;
    }
    if (r->lap == 0) {
        return GDX_SPLITS_ERR_LAYOUT;
    }
    out->laps = r->lap;
    out->nodes = r->s.nodes;
    for (l = 0; l < r->lap; l++) {
        out->lapEnd[l] = r->s.lapEnd[l];
        for (n = 0; n < r->s.nodes; n++) {
            out->node[l][n] = r->s.node[l][n];
        }
    }
    return 0;
}

int gdx_splits_pace_laps(const GdxCourseSplits* s, const float* nodeAt, int raceLaps, int fromLap, double share,
                         double* outLaps) {
    double from, to, local, lapTime, prevTime, prevAt;
    int k, src, n;
    if (!s || !outLaps) {
        return GDX_SPLITS_ERR_ARGS;
    }
    *outLaps = 0.0;
    if ((s->laps == 0) || (gdx_splits_validate(s) != 0) || !node_positions_ok(nodeAt, s->nodes) || (raceLaps < 1) ||
        (raceLaps > PACE_LAPS_MAX) || (fromLap < 0) || (fromLap >= raceLaps) || !(share > -1e9 && share < 1e9)) {
        return GDX_SPLITS_ERR_VALUE;
    }
    if ((share <= 0.0) || (share >= 1.0)) {
        *outLaps = (double)fromLap + share * (double)(raceLaps - fromLap);
        return 0;
    }
    from = staff_lap_end(s, fromLap);
    to = staff_lap_end(s, raceLaps);
    local = from + share * (to - from);
    for (k = fromLap; (k < raceLaps - 1) && (local >= staff_lap_end(s, k + 1)); k++) {
    }
    lapTime = staff_lap_end(s, k + 1) - staff_lap_end(s, k);
    local -= staff_lap_end(s, k);
    src = (k < s->laps) ? k : s->laps - 1;
    // Steady between the points of the lap: its start, each node, its end.
    prevTime = 0.0;
    prevAt = 0.0;
    for (n = 0; n < s->nodes; n++) {
        const double nodeTime = (double)(s->node[src][n] - lap_start(s, src));
        if (local < nodeTime) {
            break;
        }
        prevTime = nodeTime;
        prevAt = nodeAt[n];
    }
    if (n < s->nodes) {
        *outLaps = (double)k + prevAt + (local - prevTime) / ((double)(s->node[src][n] - lap_start(s, src)) - prevTime) *
                                            ((double)nodeAt[n] - prevAt);
    } else {
        *outLaps = (double)k + prevAt + (local - prevTime) / (lapTime - prevTime) * (1.0 - prevAt);
    }
    return 0;
}
