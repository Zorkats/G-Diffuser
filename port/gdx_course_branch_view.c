/* port/gdx_course_branch_view.c -- see gdx_course_branch_view.h. */

#include "gdx_course_branch_view.h"
#include "gdx_course_branch_format.h"

/* Fails to compile when the two limits drift apart. */
typedef char GdxBranchViewMaxForksMatch[(GDX_BRANCH_VIEW_MAX_FORKS == GDX_BRANCH_MAX_FORKS) ? 1 : -1];

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
