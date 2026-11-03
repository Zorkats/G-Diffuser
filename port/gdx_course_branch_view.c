/* port/gdx_course_branch_view.c -- see gdx_course_branch_view.h. */

#include "gdx_course_branch_view.h"
#include "gdx_course_branch_format.h"

#include <math.h>

/* Fails to compile when the two limits drift apart. */
typedef char GdxBranchViewMaxForksMatch[(GDX_BRANCH_VIEW_MAX_FORKS == GDX_BRANCH_MAX_FORKS) ? 1 : -1];

/* A racer further above the other road than this is passing over it, not onto it. */
#define LANE_MAX_HEIGHT 60.0f
/* How far inside a chunk's edge reference (radius + 11.5) a point must be to count as on the road. */
#define CHUNK_EDGE_INSET 20.0f
#define CHUNK_MAX_HEIGHT 25.0f
/* A point further along the road than this is past the chunk, whatever its side. */
#define CHUNK_MAX_ALONG 300.0f

int gdx_branch_view_lane_crossing(const GdxBranchLaneSample* current, const GdxBranchLaneSample* other) {
    const int leftCurrent = (current->lateral > current->radiusLeft) || (current->lateral < -current->radiusRight);
    const int onOther = (other->lateral <= other->radiusLeft) && (other->lateral >= -other->radiusRight);
    const float height = (other->height < 0.0f) ? -other->height : other->height;

    return leftCurrent && onOther && (height <= LANE_MAX_HEIGHT);
}

int gdx_branch_view_chunk_covers_point(const short point[3], const short center[3], const short left[3],
                                       const short right[3], const short below[3]) {
    float rel[3];
    float axis[3];
    float up[3];
    float width = 0.0f;
    float upLength = 0.0f;
    float lateral = 0.0f;
    float height = 0.0f;
    float distanceSq = 0.0f;
    float alongSq;
    int i;

    for (i = 0; i < 3; i++) {
        rel[i] = (float) point[i] - (float) center[i];
        axis[i] = (float) left[i] - (float) right[i];
        up[i] = (float) center[i] - (float) below[i];
        width += axis[i] * axis[i];
        upLength += up[i] * up[i];
        distanceSq += rel[i] * rel[i];
    }
    width = sqrtf(width);
    upLength = sqrtf(upLength);
    if ((width < 1.0f) || (upLength < 1.0f)) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        lateral += rel[i] * axis[i] / width;
        height += rel[i] * up[i] / upLength;
    }
    alongSq = distanceSq - (lateral * lateral) - (height * height);
    return (fabsf(lateral) <= (width / 2.0f) - CHUNK_EDGE_INSET) && (fabsf(height) <= CHUNK_MAX_HEIGHT) &&
           (alongSq <= CHUNK_MAX_ALONG * CHUNK_MAX_ALONG);
}

int gdx_branch_view_select_course(const char* courseId) {
    GdxBranchSet set;

    if ((courseId != NULL) && gdx_branch_store_get(courseId, &set)) {
        gdx_branch_set_active(&set);
    } else {
        gdx_branch_set_active(NULL);
    }
    return gdx_branch_view_fork_count() > 0;
}

int gdx_branch_view_fork_count(void) {
    const GdxBranchSet* set = gdx_branch_active_set();

    return (set != NULL) ? set->forkCount : 0;
}

int gdx_branch_view_ring_count(void) {
    const GdxBranchSet* set = gdx_branch_active_set();

    return (set != NULL) ? set->ringCount : 0;
}

int gdx_branch_view_validate(int ringCount, char* reason, int cap) {
    const GdxBranchSet* set = gdx_branch_active_set();

    if (set == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    return gdx_branch_validate(set, ringCount, reason, (cap > 0) ? (size_t) cap : 0);
}

static const GdxBranchFork* view_fork(int fork) {
    const GdxBranchSet* set = gdx_branch_active_set();

    if ((set == NULL) || (fork < 0) || (fork >= set->forkCount) || (fork >= GDX_BRANCH_MAX_FORKS)) {
        return NULL;
    }
    return &set->forks[fork];
}

int gdx_branch_view_fork(int fork, int* forkPoint, int* mergePoint, int* nodeCount) {
    const GdxBranchFork* data = view_fork(fork);

    if (data == NULL) {
        return 0;
    }
    *forkPoint = data->forkPoint;
    *mergePoint = data->mergePoint;
    *nodeCount = data->nodeCount;
    return 1;
}

int gdx_branch_view_fork_first_info(int fork, int* trackSegmentInfo) {
    const GdxBranchFork* data = view_fork(fork);

    if ((data == NULL) || ((data->flags & GDX_BRANCH_FLAG_FIRST_INFO) == 0)) {
        return 0;
    }
    *trackSegmentInfo = data->firstInfo;
    return 1;
}

int gdx_branch_view_node(int fork, int node, float pos[3], short* radiusLeft, short* radiusRight,
                         int* trackSegmentInfo, short* bankAngle) {
    const GdxBranchFork* data = view_fork(fork);
    const GdxBranchNode* point;

    if ((data == NULL) || (node < 0) || (node >= data->nodeCount) || (node >= GDX_BRANCH_MAX_NODES)) {
        return 0;
    }
    point = &data->nodes[node];
    pos[0] = point->pos[0];
    pos[1] = point->pos[1];
    pos[2] = point->pos[2];
    *radiusLeft = point->radiusLeft;
    *radiusRight = point->radiusRight;
    *trackSegmentInfo = point->trackSegmentInfo;
    *bankAngle = point->bankAngle;
    return 1;
}

int gdx_branch_view_part(int fork, int node, int part, int fallback) {
    const GdxBranchFork* data = view_fork(fork);
    const GdxBranchNode* value;

    if (data == NULL || part < 0 || part >= 9) {
        return fallback;
    }
    if (node == -1) {
        if (part >= 6) {
            return -1;
        }
        return (data->flags & GDX_BRANCH_FLAG_FIRST_VALUES) ? data->firstParts[part] : fallback;
    }
    if (node < 0 || node >= data->nodeCount) {
        return fallback;
    }
    value = &data->nodes[node];
    switch (part) {
        case 0: return value->pit;
        case 1: return value->dash;
        case 2: return value->dirt;
        case 3: return value->ice;
        case 4: return value->jump;
        case 5: return value->landmine;
        case 6: return value->gate;
        case 7: return value->building;
        default: return value->sign;
    }
}

int gdx_branch_view_settings_point(int fork, int node) {
    const GdxBranchFork* data = view_fork(fork);

    if (data == NULL || node < -1 || node >= data->nodeCount) {
        return -1;
    }
    if (node == -1 && !(data->flags & GDX_BRANCH_FLAG_FIRST_VALUES)) {
        return data->forkPoint;
    }
    return GDX_BRANCH_SETTINGS_BASE + fork * GDX_BRANCH_SETTINGS_STRIDE + node + 1;
}
