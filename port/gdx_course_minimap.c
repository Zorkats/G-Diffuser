#include "gdx_course_minimap.h"
#include <math.h>

static float preview_spline(float a, float b, float c, float d, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * (2.0f * b + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                   (-a + 3.0f * b - 3.0f * c + d) * t3);
}

static float preview_branch_point(const float* ring, int count, const GdxBranchFork* fork, int node, int axis) {
    if (node < 1) {
        return ring[(fork->forkPoint + node + count) % count];
    }
    if (node <= fork->nodeCount) {
        return fork->nodes[node - 1].pos[axis];
    }
    return ring[(fork->mergePoint + node - fork->nodeCount - 1) % count];
}

int gdx_course_preview_build(const float* x, const float* z, int count, const GdxBranchSet* forks,
                             int includeForks, float* outX, float* outZ, int* next, int capacity) {
    int required;
    int out = 0;
    int i;
    int f;
    int s;

    if (!x || !z || !outX || !outZ || !next || count < 2 || count > GDX_BRANCH_MAX_TOTAL_POINTS) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!isfinite(x[i]) || !isfinite(z[i])) {
            return 0;
        }
    }
    required = count * GDX_PREVIEW_STEPS;
    if (includeForks && forks) {
        if (gdx_branch_validate(forks, count, 0, 0) != GDX_BRANCH_OK) {
            return 0;
        }
        for (f = 0; f < forks->forkCount; f++) {
            required += (forks->forks[f].nodeCount + 1) * GDX_PREVIEW_STEPS + 1;
        }
    }
    if (required > capacity) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        for (s = 0; s < GDX_PREVIEW_STEPS; s++, out++) {
            float t = (float)s / GDX_PREVIEW_STEPS;
            outX[out] = preview_spline(x[(i + count - 1) % count], x[i], x[(i + 1) % count], x[(i + 2) % count], t);
            outZ[out] = preview_spline(z[(i + count - 1) % count], z[i], z[(i + 1) % count], z[(i + 2) % count], t);
            next[out] = out + 1;
        }
    }
    next[out - 1] = 0;
    for (f = 0; includeForks && forks && f < forks->forkCount; f++) {
        const GdxBranchFork* fork = &forks->forks[f];
        for (i = 0; i <= fork->nodeCount; i++) {
            float px[4];
            float pz[4];
            int k;
            for (k = 0; k < 4; k++) {
                px[k] = preview_branch_point(x, count, fork, i + k - 1, 0);
                pz[k] = preview_branch_point(z, count, fork, i + k - 1, 2);
            }
            for (s = 0; s < GDX_PREVIEW_STEPS; s++, out++) {
                float t = (float)s / GDX_PREVIEW_STEPS;
                outX[out] = preview_spline(px[0], px[1], px[2], px[3], t);
                outZ[out] = preview_spline(pz[0], pz[1], pz[2], pz[3], t);
                next[out] = out + 1;
            }
        }
        outX[out] = x[fork->mergePoint];
        outZ[out] = z[fork->mergePoint];
        next[out++] = -1;
    }
    for (i = 0; i < out; i++) {
        if (!isfinite(outX[i]) || !isfinite(outZ[i])) {
            return 0;
        }
    }
    return out;
}

void gdx_course_minimap_plot(unsigned char* texture, float x, float z, float scale, int fill) {
    int column;
    int row;
    float px = x * 64.0f * scale / 16000.0f + 64.0f * scale;
    float pz = z * 64.0f * scale / 16000.0f + 64.0f * scale;

    if (!texture || !isfinite(px) || !isfinite(pz) || px < 0 || px >= 128 || pz < 0 || pz >= 128) {
        return;
    }
    column = (int)floorf(px + 0.5f) / 2;
    row = (int)floorf(pz + 0.5f) / 2;
    if (column >= 64 || row >= 64) {
        return;
    }
    if (fill) {
        if (column > 0 && row > 0) {
            texture[row * 64 + column] = 2;
        }
    } else {
        if (column > 0) {
            texture[row * 64 + column - 1] = 1;
        }
        if (column < 63) {
            texture[row * 64 + column + 1] = 1;
        }
        if (row > 0) {
            texture[(row - 1) * 64 + column] = 1;
        }
        if (row < 63) {
            texture[(row + 1) * 64 + column] = 1;
        }
    }
}
