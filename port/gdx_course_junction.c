#include "gdx_course_junction.h"
#include <math.h>
#include <string.h>

#define POLYGON_CAPACITY 10

short gdx_junction_quantize(double value) {
    if (!isfinite(value)) {
        return 0;
    }
    return (short)fmax(-32768.0, fmin(32767.0, floor(value + 0.5)));
}

static double dot(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const double* a, const double* b, double* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int valid_vertex(const GdxJunctionVertex* v) {
    int i;
    for (i = 0; i < 3; i++) {
        if (!isfinite(v->pos[i]) || fabs(v->pos[i]) > 32768.0) {
            return 0;
        }
    }
    for (i = 0; i < 2; i++) {
        if (!isfinite(v->uv[i]) || fabs(v->uv[i]) > 32768.0) {
            return 0;
        }
    }
    return 1;
}

static double triangle_area_squared(const GdxJunctionTriangle* t) {
    double a[3], b[3], n[3];
    int i;
    for (i = 0; i < 3; i++) {
        a[i] = t->v[1].pos[i] - t->v[0].pos[i];
        b[i] = t->v[2].pos[i] - t->v[0].pos[i];
    }
    cross(a, b, n);
    return dot(n, n);
}

int gdx_junction_cutter(const GdxJunctionTriangle* owner, GdxJunctionCutter* out) {
    GdxJunctionCutter c;
    double a[3], b[3], normal[3], length;
    int i, j;
    if (!owner || !out) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (!valid_vertex(&owner->v[i])) {
            return 0;
        }
        a[i] = owner->v[1].pos[i] - owner->v[0].pos[i];
        b[i] = owner->v[2].pos[i] - owner->v[0].pos[i];
    }
    cross(a, b, normal);
    length = sqrt(dot(normal, normal));
    if (length < 1e-6) {
        return 0;
    }
    c.railVolume = 0;
    for (i = 0; i < 3; i++) {
        normal[i] /= length;
        c.min[i] = c.max[i] = owner->v[0].pos[i];
        for (j = 1; j < 3; j++) {
            c.min[i] = fmin(c.min[i], owner->v[j].pos[i]);
            c.max[i] = fmax(c.max[i], owner->v[j].pos[i]);
        }
        c.min[i] -= GDX_JUNCTION_DISTANCE;
        c.max[i] += GDX_JUNCTION_DISTANCE;
        c.plane[3][i] = normal[i];
        c.plane[4][i] = -normal[i];
    }
    c.plane[3][3] = GDX_JUNCTION_DISTANCE - dot(normal, owner->v[0].pos);
    c.plane[4][3] = GDX_JUNCTION_DISTANCE + dot(normal, owner->v[0].pos);
    for (i = 0; i < 3; i++) {
        double inward[3];
        for (j = 0; j < 3; j++) {
            a[j] = owner->v[(i + 1) % 3].pos[j] - owner->v[i].pos[j];
        }
        cross(normal, a, inward);
        length = sqrt(dot(inward, inward));
        for (j = 0; j < 3; j++) {
            c.plane[i][j] = inward[j] / length;
        }
        c.plane[i][3] = -dot(c.plane[i], owner->v[i].pos);
    }
    *out = c;
    return 1;
}

int gdx_junction_rail_cutter(const GdxJunctionTriangle* road, double depth, GdxJunctionCutter* out) {
    GdxJunctionCutter c;
    int i;
    if (!isfinite(depth) || depth <= 0 || depth > 100 || !gdx_junction_cutter(road, &c)) {
        return 0;
    }
    c.railVolume = 1;
    for (i = 0; i < 3; i++) {
        double extension = fabs(c.plane[3][i]) * depth;
        c.min[i] -= extension;
        c.max[i] += extension;
    }
    c.plane[3][3] += depth;
    c.plane[4][3] += depth;
    *out = c;
    return 1;
}

static int clip_polygon(const GdxJunctionVertex* in, int count, const double* plane,
                         int inside, GdxJunctionVertex* out) {
    int i, j, n = 0;
    for (i = 0; i < count; i++) {
        const GdxJunctionVertex* a = &in[i];
        const GdxJunctionVertex* b = &in[(i + 1) % count];
        double da = dot(plane, a->pos) + plane[3];
        double db = dot(plane, b->pos) + plane[3];
        int keepA = inside ? da >= 0 : da < 0;
        int keepB = inside ? db >= 0 : db < 0;
        if (keepA) {
            if (n == POLYGON_CAPACITY) {
                return -1;
            }
            out[n++] = *a;
        }
        if (keepA != keepB) {
            double t = da / (da - db);
            if (n == POLYGON_CAPACITY) {
                return -1;
            }
            for (j = 0; j < 3; j++) {
                out[n].pos[j] = a->pos[j] + (b->pos[j] - a->pos[j]) * t;
            }
            for (j = 0; j < 2; j++) {
                out[n].uv[j] = a->uv[j] + (b->uv[j] - a->uv[j]) * t;
            }
            n++;
        }
    }
    return n;
}

static int append_polygon(const GdxJunctionVertex* polygon, int count,
                           GdxJunctionTriangle* out, int* outCount) {
    int i;
    for (i = 1; i + 1 < count; i++) {
        GdxJunctionTriangle t;
        t.v[0] = polygon[0];
        t.v[1] = polygon[i];
        t.v[2] = polygon[i + 1];
        if (triangle_area_squared(&t) <= 1e-12) {
            continue;
        }
        if (*outCount == GDX_JUNCTION_MAX_PIECES) {
            return 0;
        }
        out[(*outCount)++] = t;
    }
    return 1;
}

static int subtract_one(const GdxJunctionTriangle* input, const GdxJunctionCutter* c,
                         GdxJunctionTriangle* out, int* outCount) {
    GdxJunctionVertex polygon[2][POLYGON_CAPACITY];
    GdxJunctionVertex outside[POLYGON_CAPACITY];
    int i, j, count = 3, side = 0;
    double removed = 0;
    double a[3], b[3], normal[3], alignment;
    for (i = 0; i < 3; i++) {
        a[i] = input->v[1].pos[i] - input->v[0].pos[i];
        b[i] = input->v[2].pos[i] - input->v[0].pos[i];
    }
    cross(a, b, normal);
    alignment = dot(normal, c->plane[3]);
    // Rail volumes are used only after the owning road surfaces overlap.
    if (!c->railVolume && alignment * alignment <
        GDX_JUNCTION_PARALLEL_COS * GDX_JUNCTION_PARALLEL_COS * dot(normal, normal)) {
        if (*outCount == GDX_JUNCTION_MAX_PIECES) {
            return 0;
        }
        out[(*outCount)++] = *input;
        return 1;
    }
    for (i = 0; i < 3; i++) {
        double min = input->v[0].pos[i], max = min;
        for (j = 1; j < 3; j++) {
            min = fmin(min, input->v[j].pos[i]);
            max = fmax(max, input->v[j].pos[i]);
        }
        if (min > c->max[i] || max < c->min[i]) {
            break;
        }
    }
    if (i == 3) {
        memcpy(polygon[0], input->v, sizeof(input->v));
        for (i = 0; i < 5 && count > 0; i++) {
            count = clip_polygon(polygon[side], count, c->plane[i], 1, polygon[side ^ 1]);
            if (count < 0) {
                return 0;
            }
            side ^= 1;
        }
        for (i = 1; i + 1 < count; i++) {
            GdxJunctionTriangle t;
            t.v[0] = polygon[side][0];
            t.v[1] = polygon[side][i];
            t.v[2] = polygon[side][i + 1];
            removed += sqrt(triangle_area_squared(&t));
        }
    }
    if (removed <= 1e-6) {
        if (*outCount == GDX_JUNCTION_MAX_PIECES) {
            return 0;
        }
        out[(*outCount)++] = *input;
        return 1;
    }
    memcpy(polygon[0], input->v, sizeof(input->v));
    side = 0;
    count = 3;
    for (i = 0; i < 5 && count > 0; i++) {
        int outsideCount = clip_polygon(polygon[side], count, c->plane[i], 0, outside);
        if (outsideCount < 0 || !append_polygon(outside, outsideCount, out, outCount)) {
            return 0;
        }
        count = clip_polygon(polygon[side], count, c->plane[i], 1, polygon[side ^ 1]);
        if (count < 0) {
            return 0;
        }
        side ^= 1;
    }
    return 1;
}

int gdx_junction_subtract(const GdxJunctionTriangle* input, const GdxJunctionCutter* cutters,
                          int cutterCount, GdxJunctionWorkspace* workspace,
                          GdxJunctionTriangle* output, int capacity) {
    int i, j, k, side = 0, count = 1;
    if (!input || !workspace || !output || capacity < 0 || cutterCount < 0 ||
        cutterCount > GDX_JUNCTION_MAX_CUTTERS ||
        (cutterCount > 0 && !cutters)) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (!valid_vertex(&input->v[i])) {
            return -1;
        }
    }
    for (i = 0; i < cutterCount; i++) {
        for (j = 0; j < 3; j++) {
            if (!isfinite(cutters[i].min[j]) || !isfinite(cutters[i].max[j]) ||
                cutters[i].min[j] > cutters[i].max[j]) {
                return -1;
            }
        }
        for (j = 0; j < 5; j++) {
            for (k = 0; k < 4; k++) {
                if (!isfinite(cutters[i].plane[j][k])) {
                    return -1;
                }
            }
        }
    }
    workspace->pieces[0][0] = *input;
    for (i = 0; i < cutterCount && count > 0; i++) {
        int nextCount = 0;
        for (j = 0; j < count; j++) {
            if (!subtract_one(&workspace->pieces[side][j], &cutters[i],
                               workspace->pieces[side ^ 1], &nextCount)) {
                return -1;
            }
        }
        count = nextCount;
        side ^= 1;
    }
    if (count > capacity) {
        return -1;
    }
    memcpy(output, workspace->pieces[side], (size_t)count * sizeof(*output));
    return count;
}
