/* port/gdx_course_branch_edit.c -- see gdx_course_branch_edit.h. */

#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_format.h"

#include <stdio.h>
#include <string.h>

typedef struct EditState {
    GdxBranchSet set; /* ringCount is the editor ring length in the view showing */
    int viewB;
} EditState;

/* Fails to compile when the undo snapshot size is too small for the state. */
typedef char GdxBranchEditStateFits[(sizeof(EditState) <= GDX_BRANCH_EDIT_STATE_BYTES) ? 1 : -1];

static EditState sLive;
static EditState sBackup;

static void edit_publish(void) {
    gdx_branch_set_active((sLive.set.forkCount > 0) ? &sLive.set : NULL);
}

static void edit_reason(char* reason, int cap, const char* text) {
    if ((reason != NULL) && (cap > 0)) {
        snprintf(reason, (size_t) cap, "%s", text);
    }
}

static int edit_total_nodes(void) {
    int total = 0;
    int f;

    for (f = 0; f < sLive.set.forkCount; f++) {
        total += sLive.set.forks[f].nodeCount;
    }
    return total;
}

static void edit_remove_fork(int f) {
    int i;

    for (i = f; i + 1 < sLive.set.forkCount; i++) {
        sLive.set.forks[i] = sLive.set.forks[i + 1];
    }
    sLive.set.forkCount--;
    memset(&sLive.set.forks[sLive.set.forkCount], 0, sizeof(GdxBranchFork));
}

static void edit_sort(void) {
    int i;
    int j;

    for (i = 1; i < sLive.set.forkCount; i++) {
        GdxBranchFork fork = sLive.set.forks[i];

        for (j = i; (j > 0) && (sLive.set.forks[j - 1].forkPoint > fork.forkPoint); j--) {
            sLive.set.forks[j] = sLive.set.forks[j - 1];
        }
        sLive.set.forks[j] = fork;
    }
}

static void point_to_node(const GdxBranchEditPoint* point, GdxBranchNode* node) {
    memset(node, 0, sizeof(*node));
    node->pos[0] = point->pos[0];
    node->pos[1] = point->pos[1];
    node->pos[2] = point->pos[2];
    node->radiusLeft = point->radiusLeft;
    node->radiusRight = point->radiusRight;
    node->trackSegmentInfo = point->trackSegmentInfo;
    node->bankAngle = point->bankAngle;
    node->pit = point->parts[0];
    node->dash = point->parts[1];
    node->dirt = point->parts[2];
    node->ice = point->parts[3];
    node->jump = point->parts[4];
    node->landmine = point->parts[5];
    node->gate = point->parts[6];
    node->building = point->parts[7];
    node->sign = point->parts[8];
}

static void node_to_point(const GdxBranchNode* node, GdxBranchEditPoint* point) {
    memset(point, 0, sizeof(*point));
    point->pos[0] = node->pos[0];
    point->pos[1] = node->pos[1];
    point->pos[2] = node->pos[2];
    point->radiusLeft = node->radiusLeft;
    point->radiusRight = node->radiusRight;
    point->trackSegmentInfo = node->trackSegmentInfo;
    point->bankAngle = node->bankAngle;
    point->parts[0] = node->pit;
    point->parts[1] = node->dash;
    point->parts[2] = node->dirt;
    point->parts[3] = node->ice;
    point->parts[4] = node->jump;
    point->parts[5] = node->landmine;
    point->parts[6] = node->gate;
    point->parts[7] = node->building;
    point->parts[8] = node->sign;
}

void gdx_branch_edit_clear(void) {
    memset(&sLive, 0, sizeof(sLive));
    edit_publish();
}

int gdx_branch_edit_select(const char* courseId, int ringCount) {
    GdxBranchSet set;

    memset(&sLive, 0, sizeof(sLive));
    if ((courseId != NULL) && gdx_branch_store_get(courseId, &set) && (set.ringCount == ringCount)) {
        sLive.set = set;
    }
    sLive.set.ringCount = (uint8_t) ringCount;
    edit_publish();
    return sLive.set.forkCount;
}

int gdx_branch_edit_commit(const char* courseId, int ringCount, unsigned int courseChecksum, char* reason, int cap) {
    GdxBranchSet set = sLive.set;
    int rc;

    edit_reason(reason, cap, "");
    if (courseId == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    set.ringCount = (uint8_t) ringCount;
    set.courseChecksum = courseChecksum;
    if (sLive.viewB) {
        edit_reason(reason, cap, "path B is showing");
        rc = GDX_BRANCH_ERR_ARGS;
    } else if (set.forkCount == 0) {
        rc = GDX_BRANCH_OK;
    } else {
        rc = gdx_branch_validate(&set, ringCount, reason, (cap > 0) ? (size_t) cap : 0);
    }
    if (rc != GDX_BRANCH_OK) {
        set.forkCount = 0;
    }
    if (gdx_branch_store_set(courseId, &set) != GDX_BRANCH_OK) {
        if (rc == GDX_BRANCH_OK) {
            edit_reason(reason, cap, "the fork store is full");
            rc = GDX_BRANCH_ERR_FULL;
        }
    }
    return rc;
}

int gdx_branch_edit_fork_count(void) {
    return sLive.set.forkCount;
}

int gdx_branch_edit_view_b(void) {
    return sLive.viewB;
}

int gdx_branch_edit_fork_at(int segment) {
    int f;

    for (f = 0; f < sLive.set.forkCount; f++) {
        if ((segment >= sLive.set.forks[f].forkPoint) && (segment < sLive.set.forks[f].mergePoint)) {
            return f;
        }
    }
    return -1;
}

int gdx_branch_edit_swap(GdxBranchEditPoint* ring, int ringCount, int cap, int* dropped) {
    static GdxBranchEditPoint sOut[GDX_BRANCH_EDIT_MAX_POINTS];
    EditState next = sLive;
    int newFork[GDX_BRANCH_MAX_FORKS];
    int newMerge[GDX_BRANCH_MAX_FORKS];
    int outCount = 0;
    int f;
    int i;
    int j;

    *dropped = 0;
    if ((ring == NULL) || (ringCount <= 0) || (ringCount > GDX_BRANCH_EDIT_MAX_POINTS) || (cap < ringCount)) {
        return ringCount;
    }
    /* A fork whose ring interior is empty, or longer than a node list, cannot swap. */
    for (f = next.set.forkCount - 1; f >= 0; f--) {
        int interior = next.set.forks[f].mergePoint - next.set.forks[f].forkPoint - 1;

        if ((interior < 1) || (interior > GDX_BRANCH_MAX_NODES) || (next.set.forks[f].mergePoint >= ringCount)) {
            for (i = f; i + 1 < next.set.forkCount; i++) {
                next.set.forks[i] = next.set.forks[i + 1];
            }
            next.set.forkCount--;
            (*dropped)++;
        }
    }

    /* The new ring: each interior replaced by the fork's nodes. Forks are sorted and may share a point. */
    f = 0;
    for (i = 0; i < ringCount; i++) {
        const GdxBranchFork* fork = (f < next.set.forkCount) ? &next.set.forks[f] : NULL;

        if ((fork != NULL) && (i > fork->forkPoint) && (i < fork->mergePoint)) {
            continue;
        }
        if ((fork != NULL) && (i == fork->mergePoint)) {
            newMerge[f] = outCount;
            f++;
        }
        if (outCount >= GDX_BRANCH_EDIT_MAX_POINTS) {
            return ringCount;
        }
        sOut[outCount++] = ring[i];
        fork = (f < next.set.forkCount) ? &next.set.forks[f] : NULL;
        if ((fork != NULL) && (i == fork->forkPoint)) {
            newFork[f] = outCount - 1;
            if (outCount + fork->nodeCount > GDX_BRANCH_EDIT_MAX_POINTS) {
                return ringCount;
            }
            for (j = 0; j < fork->nodeCount; j++) {
                node_to_point(&fork->nodes[j], &sOut[outCount++]);
            }
        }
    }
    if ((f != next.set.forkCount) || (outCount > cap)) {
        return ringCount;
    }

    /* Each fork now stores the old interior, and its points move to their new indices. */
    for (f = 0; f < next.set.forkCount; f++) {
        GdxBranchFork* fork = &next.set.forks[f];
        int oldFork = fork->forkPoint;
        int interior = fork->mergePoint - oldFork - 1;

        memset(fork->nodes, 0, sizeof(fork->nodes));
        for (j = 0; j < interior; j++) {
            point_to_node(&ring[oldFork + 1 + j], &fork->nodes[j]);
        }
        fork->nodeCount = (uint8_t) interior;
        fork->forkPoint = (uint8_t) newFork[f];
        fork->mergePoint = (uint8_t) newMerge[f];
    }
    for (i = 0; i < outCount; i++) {
        ring[i] = sOut[i];
    }
    next.set.ringCount = (uint8_t) outCount;
    next.viewB = !next.viewB;
    sLive = next;
    edit_publish();
    return outCount;
}

void gdx_branch_edit_on_insert(int newIndex, const GdxBranchEditPoint* ring, int ringCount) {
    int f;

    if (sLive.set.forkCount == 0) {
        sLive.set.ringCount = (uint8_t) ringCount;
        return;
    }
    gdx_branch_on_ring_insert(&sLive.set, newIndex);
    sLive.set.ringCount = (uint8_t) ringCount;
    for (f = 0; f < sLive.set.forkCount; f++) {
        GdxBranchFork* fork = &sLive.set.forks[f];
        GdxBranchNode node;
        const float* prev;
        const float* next;
        int k;

        if ((newIndex <= fork->forkPoint) || (newIndex >= fork->mergePoint) ||
            !(fork->flags & GDX_BRANCH_FLAG_SYMMETRIC)) {
            continue;
        }
        /* A point added to path B is that path's own: path A keeps its points. */
        if (sLive.viewB) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
            continue;
        }
        /* The new interior point gets a partner on the other path, halfway between its neighbors there. */
        k = newIndex - fork->forkPoint - 1;
        if ((ring == NULL) || (fork->mergePoint >= ringCount) || (k > fork->nodeCount) ||
            (ringCount + edit_total_nodes() + 1 > GDX_BRANCH_MAX_TOTAL_POINTS)) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
            continue;
        }
        prev = (k == 0) ? ring[fork->forkPoint].pos : fork->nodes[k - 1].pos;
        next = (k == fork->nodeCount) ? ring[fork->mergePoint].pos : fork->nodes[k].pos;
        point_to_node(&ring[newIndex], &node);
        node.pos[0] = (prev[0] + next[0]) * 0.5f;
        node.pos[1] = (prev[1] + next[1]) * 0.5f;
        node.pos[2] = (prev[2] + next[2]) * 0.5f;
        node.pit = node.dash = node.dirt = node.ice = node.jump = 0;
        node.landmine = node.gate = node.building = node.sign = 0;
        if (gdx_branch_insert_node(fork, k, &node) != GDX_BRANCH_OK) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
        }
    }
    edit_publish();
}

void gdx_branch_edit_on_delete(int deletedIndex) {
    if (sLive.set.ringCount > 0) {
        sLive.set.ringCount--;
    }
    if (sLive.set.forkCount == 0) {
        return;
    }
    gdx_branch_on_ring_delete(&sLive.set, deletedIndex);
    edit_publish();
}

void gdx_branch_edit_on_rotate(int newStart, int ringCount) {
    int f;

    if ((sLive.set.forkCount == 0) || (ringCount <= 0)) {
        return;
    }
    for (f = sLive.set.forkCount - 1; f >= 0; f--) {
        GdxBranchFork* fork = &sLive.set.forks[f];
        int forkPoint = (fork->forkPoint - newStart + ringCount) % ringCount;
        int mergePoint = (fork->mergePoint - newStart + ringCount) % ringCount;

        if ((forkPoint == 0) || (forkPoint >= mergePoint)) {
            edit_remove_fork(f);
            continue;
        }
        fork->forkPoint = (uint8_t) forkPoint;
        fork->mergePoint = (uint8_t) mergePoint;
    }
    edit_sort();
    edit_publish();
}

void gdx_branch_edit_on_reverse(int ringCount) {
    int f;
    int i;

    if ((sLive.set.forkCount == 0) || (ringCount <= 0)) {
        return;
    }
    /* func_xk2_800F0FF4 puts old point k at (1 - k) mod n and mirrors every bank angle. */
    for (f = sLive.set.forkCount - 1; f >= 0; f--) {
        GdxBranchFork* fork = &sLive.set.forks[f];
        int forkPoint = (1 - fork->mergePoint + ringCount) % ringCount;
        int mergePoint = (1 - fork->forkPoint + ringCount) % ringCount;

        if ((forkPoint == 0) || (forkPoint >= mergePoint)) {
            edit_remove_fork(f);
            continue;
        }
        fork->forkPoint = (uint8_t) forkPoint;
        fork->mergePoint = (uint8_t) mergePoint;
        for (i = 0; i < fork->nodeCount / 2; i++) {
            GdxBranchNode node = fork->nodes[i];

            fork->nodes[i] = fork->nodes[fork->nodeCount - 1 - i];
            fork->nodes[fork->nodeCount - 1 - i] = node;
        }
        for (i = 0; i < fork->nodeCount; i++) {
            fork->nodes[i].bankAngle = (int16_t) (360 - fork->nodes[i].bankAngle);
        }
    }
    edit_sort();
    edit_publish();
}

int gdx_branch_edit_sync(int ringCount) {
    int removed = 0;
    int f;

    if (sLive.set.forkCount == 0) {
        sLive.set.ringCount = (uint8_t) ringCount;
        edit_publish();
        return 0;
    }
    if (sLive.set.ringCount != ringCount) {
        /* A ring change no hook reported: the indices are stale. */
        removed = sLive.set.forkCount;
        memset(&sLive.set, 0, sizeof(sLive.set));
        sLive.viewB = 0;
        sLive.set.ringCount = (uint8_t) ringCount;
        edit_publish();
        return removed;
    }
    for (f = sLive.set.forkCount - 1; f >= 0; f--) {
        GdxBranchFork* fork = &sLive.set.forks[f];
        int bad = (fork->forkPoint < 1) || (fork->mergePoint <= fork->forkPoint) ||
                  (fork->mergePoint > ringCount - 1) || (fork->nodeCount < 1) ||
                  (fork->nodeCount > GDX_BRANCH_MAX_NODES) ||
                  ((f + 1 < sLive.set.forkCount) && (fork->mergePoint > sLive.set.forks[f + 1].forkPoint));

        if (bad) {
            edit_remove_fork(f);
            removed++;
            continue;
        }
        if ((fork->flags & GDX_BRANCH_FLAG_SYMMETRIC) && (fork->nodeCount != fork->mergePoint - fork->forkPoint - 1)) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
        }
    }
    while ((sLive.set.forkCount > 0) && (ringCount + edit_total_nodes() > GDX_BRANCH_MAX_TOTAL_POINTS)) {
        edit_remove_fork(sLive.set.forkCount - 1);
        removed++;
    }
    if ((sLive.set.forkCount > 0) && (gdx_branch_validate(&sLive.set, ringCount, NULL, 0) != GDX_BRANCH_OK)) {
        removed += sLive.set.forkCount;
        memset(&sLive.set, 0, sizeof(sLive.set));
        sLive.set.ringCount = (uint8_t) ringCount;
    }
    if (sLive.set.forkCount == 0) {
        sLive.viewB = 0;
    }
    edit_publish();
    return removed;
}

int gdx_branch_edit_split_check(int segment, int ringCount, char* reason, int cap) {
    edit_reason(reason, cap, "");
    if (gdx_branch_edit_fork_at(segment) >= 0) {
        return 1;
    }
    if (sLive.viewB) {
        edit_reason(reason, cap, "Split: show path A first (Split on a fork)");
        return -1;
    }
    if (segment < 1) {
        edit_reason(reason, cap, "Split: not on the start segment");
        return -1;
    }
    if (segment > ringCount - 2) {
        edit_reason(reason, cap, "Split: not on the last segment");
        return -1;
    }
    if (sLive.set.forkCount >= GDX_BRANCH_MAX_FORKS) {
        edit_reason(reason, cap, "Split: a course has at most 4 forks");
        return -1;
    }
    if (ringCount + 1 + edit_total_nodes() + 1 > GDX_BRANCH_MAX_TOTAL_POINTS) {
        edit_reason(reason, cap, "Split: the course and its forks would pass 64 points");
        return -1;
    }
    return 0;
}

int gdx_branch_edit_split_commit(int segment, const GdxBranchEditPoint* nodeB) {
    return gdx_branch_edit_span_commit(segment, segment + 2, nodeB, 1);
}

int gdx_branch_edit_span_check(int forkPoint, int mergePoint, int ringCount, char* reason, int cap) {
    int interior = mergePoint - forkPoint - 1;
    int f;

    edit_reason(reason, cap, "");
    if (sLive.viewB) {
        edit_reason(reason, cap, "Split: show path A first (Split on a fork)");
        return GDX_BRANCH_ERR_ARGS;
    }
    if (forkPoint < 1) {
        edit_reason(reason, cap, "Split: not on the start segment");
        return GDX_BRANCH_ERR_SPAN;
    }
    if (mergePoint > ringCount - 1) {
        edit_reason(reason, cap, "Split: not on the last segment");
        return GDX_BRANCH_ERR_SPAN;
    }
    if (interior < 1) {
        edit_reason(reason, cap, "Split: path A needs a point between the ends");
        return GDX_BRANCH_ERR_SPAN;
    }
    if (interior > GDX_BRANCH_MAX_NODES) {
        edit_reason(reason, cap, "Split: a path holds at most 32 points");
        return GDX_BRANCH_ERR_COUNT;
    }
    /* Two forks may meet at a point, never share a segment. */
    for (f = 0; f < sLive.set.forkCount; f++) {
        if ((forkPoint < sLive.set.forks[f].mergePoint) && (sLive.set.forks[f].forkPoint < mergePoint)) {
            edit_reason(reason, cap, "Split: it would cross another fork");
            return GDX_BRANCH_ERR_OVERLAP;
        }
    }
    if (sLive.set.forkCount >= GDX_BRANCH_MAX_FORKS) {
        edit_reason(reason, cap, "Split: a course has at most 4 forks");
        return GDX_BRANCH_ERR_COUNT;
    }
    if (ringCount + edit_total_nodes() + interior > GDX_BRANCH_MAX_TOTAL_POINTS) {
        edit_reason(reason, cap, "Split: the course and its forks would pass 64 points");
        return GDX_BRANCH_ERR_TOTAL;
    }
    return GDX_BRANCH_OK;
}

int gdx_branch_edit_span_commit(int forkPoint, int mergePoint, const GdxBranchEditPoint* nodesB, int count) {
    GdxBranchSet set;
    GdxBranchFork* fork;
    int i;

    if ((nodesB == NULL) || (count < 1) || (count > GDX_BRANCH_MAX_NODES) ||
        (sLive.set.forkCount >= GDX_BRANCH_MAX_FORKS)) {
        return GDX_BRANCH_ERR_FULL;
    }
    set = sLive.set;
    fork = &set.forks[set.forkCount];
    memset(fork, 0, sizeof(*fork));
    fork->forkPoint = (uint8_t) forkPoint;
    fork->mergePoint = (uint8_t) mergePoint;
    fork->flags = (count == mergePoint - forkPoint - 1) ? GDX_BRANCH_FLAG_SYMMETRIC : 0;
    fork->nodeCount = (uint8_t) count;
    for (i = 0; i < count; i++) {
        point_to_node(&nodesB[i], &fork->nodes[i]);
    }
    set.forkCount++;
    sLive.set = set;
    edit_sort();
    edit_publish();
    return GDX_BRANCH_OK;
}

int gdx_branch_edit_fork_shape(int f, int* forkPoint, int* mergePoint, float (*pos)[3], int cap) {
    const GdxBranchFork* fork;
    int i;

    if ((f < 0) || (f >= sLive.set.forkCount)) {
        return -1;
    }
    fork = &sLive.set.forks[f];
    *forkPoint = fork->forkPoint;
    *mergePoint = fork->mergePoint;
    for (i = 0; (i < fork->nodeCount) && (i < cap); i++) {
        pos[i][0] = fork->nodes[i].pos[0];
        pos[i][1] = fork->nodes[i].pos[1];
        pos[i][2] = fork->nodes[i].pos[2];
    }
    return fork->nodeCount;
}

int gdx_branch_edit_link(int fork, int newMerge, int ringCount) {
    GdxBranchSet set;

    if (sLive.viewB || (fork < 0) || (fork >= sLive.set.forkCount)) {
        return GDX_BRANCH_ERR_ARGS;
    }
    set = sLive.set;
    set.forks[fork].mergePoint = (uint8_t) newMerge;
    set.forks[fork].flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
    set.ringCount = (uint8_t) ringCount;
    /* Path A between the points must stay swappable into the B view. */
    if ((newMerge == sLive.set.forks[fork].mergePoint) || (newMerge - set.forks[fork].forkPoint - 1 < 1) ||
        (newMerge - set.forks[fork].forkPoint - 1 > GDX_BRANCH_MAX_NODES) ||
        (gdx_branch_validate(&set, ringCount, NULL, 0) != GDX_BRANCH_OK)) {
        return GDX_BRANCH_ERR_SPAN;
    }
    sLive.set = set;
    edit_publish();
    return GDX_BRANCH_OK;
}

void gdx_branch_edit_label(int ringPoint, char* out, int cap) {
    int f;

    if ((out == NULL) || (cap <= 0)) {
        return;
    }
    for (f = 0; f < sLive.set.forkCount; f++) {
        if ((ringPoint > sLive.set.forks[f].forkPoint) && (ringPoint < sLive.set.forks[f].mergePoint)) {
            snprintf(out, (size_t) cap, "%d-%c", ringPoint, sLive.viewB ? 'B' : 'A');
            return;
        }
    }
    snprintf(out, (size_t) cap, "%d", ringPoint);
}

void gdx_branch_edit_state_save(unsigned char* out) {
    memset(out, 0, GDX_BRANCH_EDIT_STATE_BYTES);
    memcpy(out, &sLive, sizeof(sLive));
}

void gdx_branch_edit_state_load(const unsigned char* in) {
    memcpy(&sLive, in, sizeof(sLive));
    edit_publish();
}

int gdx_branch_edit_state_equal(const unsigned char* in) {
    return memcmp(in, &sLive, sizeof(sLive)) == 0;
}

void gdx_branch_edit_backup(void) {
    sBackup = sLive;
}

void gdx_branch_edit_restore(void) {
    sLive = sBackup;
    edit_publish();
}

void gdx_branch_edit_backup_save(unsigned char* out) {
    memset(out, 0, GDX_BRANCH_EDIT_STATE_BYTES);
    memcpy(out, &sBackup, sizeof(sBackup));
}

void gdx_branch_edit_backup_load(const unsigned char* in) {
    memcpy(&sBackup, in, sizeof(sBackup));
}
