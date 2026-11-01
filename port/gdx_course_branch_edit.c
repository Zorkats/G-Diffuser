/* port/gdx_course_branch_edit.c -- see gdx_course_branch_edit.h. */

#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_format.h"
#include "gdx_course_settings.h"

#include <stdio.h>
#include <string.h>

typedef struct EditState {
    GdxBranchSet set; /* ringCount is the editor ring length in the view showing */
    int viewB;
    GdxPointExtras ringExtras[GDX_BRANCH_EDIT_MAX_POINTS]; /* The ring in the view showing, by ring index */
    uint8_t lavaMode;
    uint8_t magnetStrength;
    uint8_t magnetRange;
} EditState;

/* Fails to compile when the undo snapshot size is too small for the state. */
typedef char GdxBranchEditStateFits[(sizeof(EditState) <= GDX_BRANCH_EDIT_STATE_BYTES) ? 1 : -1];
typedef char GdxBranchEditExtrasFit[(GDX_BRANCH_EDIT_MAX_POINTS <= GDX_SETTINGS_MAX_POINTS) ? 1 : -1];

static EditState sLive;
static EditState sBackup;

/* The values as GdxCourseSettings. skipNodes leaves the stored path's values out. */
static void edit_build_settings(GdxCourseSettings* out, int ringCount, int skipNodes) {
    int f;
    int i;

    memset(out, 0, sizeof(*out));
    out->ringCount = (uint8_t) ringCount;
    out->lavaMode = sLive.lavaMode;
    out->magnetStrength = sLive.magnetStrength;
    out->magnetRange = sLive.magnetRange;
    for (i = 0; (i < ringCount) && (i < GDX_BRANCH_EDIT_MAX_POINTS); i++) {
        out->ring[i] = sLive.ringExtras[i];
    }
    for (f = 0; !skipNodes && (f < sLive.set.forkCount); f++) {
        for (i = 0; i < sLive.set.forks[f].nodeCount; i++) {
            out->nodes[f][i] = sLive.set.forks[f].nodes[i].extras;
        }
    }
}

static void edit_publish(void) {
    static GdxCourseSettings sPublished;

    gdx_branch_set_active((sLive.set.forkCount > 0) ? &sLive.set : NULL);
    edit_build_settings(&sPublished, sLive.set.ringCount, 0);
    gdx_settings_set_active(gdx_settings_is_empty(&sPublished) ? NULL : &sPublished);
}

static void extras_clear_ring(void) {
    memset(sLive.ringExtras, 0, sizeof(sLive.ringExtras));
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

static void node_clear_parts(GdxBranchNode* node) {
    node->pit = node->dash = node->dirt = node->ice = node->jump = GDX_BRANCH_EDIT_PART_NONE;
    node->landmine = node->gate = node->building = node->sign = GDX_BRANCH_EDIT_PART_NONE;
}

void gdx_branch_edit_point_clear_parts(GdxBranchEditPoint* point) {
    memset(point->parts, GDX_BRANCH_EDIT_PART_NONE, sizeof(point->parts));
}

/* Older builds gave every new path B node 0 for all nine parts, the first type of each. */
static void edit_repair_parts(GdxBranchSet* set) {
    int f;
    int i;

    for (f = 0; f < set->forkCount; f++) {
        for (i = 0; i < set->forks[f].nodeCount; i++) {
            GdxBranchNode* node = &set->forks[f].nodes[i];

            if ((node->pit == 0) && (node->dash == 0) && (node->dirt == 0) && (node->ice == 0) && (node->jump == 0) &&
                (node->landmine == 0) && (node->gate == 0) && (node->building == 0) && (node->sign == 0)) {
                node_clear_parts(node);
            }
        }
    }
}

void gdx_branch_edit_clear(void) {
    memset(&sLive, 0, sizeof(sLive));
    edit_publish();
}

int gdx_branch_edit_select(const char* courseId, int ringCount) {
    static GdxCourseSettings sLoaded;
    GdxBranchSet set;

    memset(&sLive, 0, sizeof(sLive));
    if ((courseId != NULL) && gdx_branch_store_get(courseId, &set) && (set.ringCount == ringCount)) {
        edit_repair_parts(&set);
        sLive.set = set;
    }
    sLive.set.ringCount = (uint8_t) ringCount;
    if ((courseId != NULL) && gdx_settings_store_get(courseId, &sLoaded)) {
        sLive.lavaMode = sLoaded.lavaMode;
        sLive.magnetStrength = sLoaded.magnetStrength;
        sLive.magnetRange = sLoaded.magnetRange;
        if ((sLoaded.ringCount == ringCount) && (ringCount <= GDX_BRANCH_EDIT_MAX_POINTS)) {
            int f;
            int i;

            memcpy(sLive.ringExtras, sLoaded.ring, (size_t) ringCount * sizeof(GdxPointExtras));
            for (f = 0; f < sLive.set.forkCount; f++) {
                for (i = 0; i < sLive.set.forks[f].nodeCount; i++) {
                    sLive.set.forks[f].nodes[i].extras = sLoaded.nodes[f][i];
                }
            }
        }
    }
    edit_publish();
    return sLive.set.forkCount;
}

void gdx_branch_edit_first_fill(const GdxBranchEditPoint* ring, int ringCount) {
    int f;

    for (f = 0; f < sLive.set.forkCount; f++) {
        GdxBranchFork* fork = &sLive.set.forks[f];

        if (!(fork->flags & GDX_BRANCH_FLAG_FIRST_INFO) && (fork->forkPoint < ringCount)) {
            fork->firstInfo = ring[fork->forkPoint].trackSegmentInfo;
            fork->flags |= GDX_BRANCH_FLAG_FIRST_INFO;
        }
    }
    edit_publish();
}

int gdx_branch_edit_commit(const char* courseId, const GdxBranchEditPoint* ring, int ringCount,
                           unsigned int courseChecksum, char* reason, int cap) {
    static GdxCourseSettings sSaved;
    GdxBranchSet set = sLive.set;
    int rc;
    int f;
    int i;

    edit_reason(reason, cap, "");
    if (courseId == NULL) {
        return GDX_BRANCH_ERR_ARGS;
    }
    set.ringCount = (uint8_t) ringCount;
    set.courseChecksum = courseChecksum;
    /* Point values go in the settings store, not the fork block. */
    for (f = 0; f < set.forkCount; f++) {
        for (i = 0; i < set.forks[f].nodeCount; i++) {
            memset(&set.forks[f].nodes[i].extras, 0, sizeof(GdxPointExtras));
        }
    }
    /* Paths that agree store no value of their own, so the set stays readable by older builds. */
    for (f = 0; (ring != NULL) && (f < set.forkCount); f++) {
        GdxBranchFork* fork = &set.forks[f];

        if ((fork->forkPoint < ringCount) && (fork->firstInfo == ring[fork->forkPoint].trackSegmentInfo)) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_FIRST_INFO;
            fork->firstInfo = 0;
        }
    }
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
    /* With path B showing the ring table holds path B, so the stored values stay as they are. */
    if (!sLive.viewB) {
        edit_build_settings(&sSaved, ringCount, rc != GDX_BRANCH_OK);
        if ((gdx_settings_store_set(courseId, &sSaved) != GDX_SETTINGS_OK) && (rc == GDX_BRANCH_OK)) {
            edit_reason(reason, cap, "the course settings store is full");
            rc = GDX_BRANCH_ERR_FULL;
        }
    }
    return rc;
}

/* The value's range: 0 is always "not set". */
static int extra_clamp(int which, int value) {
    int min = 1;
    int max = GDX_SETTINGS_WIDTH_MAX;

    if (which == GDX_BRANCH_EDIT_EXTRA_MAGNET_STRENGTH) {
        min = GDX_SETTINGS_STRENGTH_MIN;
        max = GDX_SETTINGS_STRENGTH_MAX;
    } else if (which == GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE) {
        min = GDX_SETTINGS_RANGE_MIN;
        max = GDX_SETTINGS_RANGE_MAX;
    }
    if (value <= 0) {
        return 0;
    }
    return (value < min) ? min : (value > max) ? max : value;
}

static uint8_t* extra_field(GdxPointExtras* extras, int which) {
    switch (which) {
        case GDX_BRANCH_EDIT_EXTRA_PIT_WIDTH:
            return &extras->pitWidth;
        case GDX_BRANCH_EDIT_EXTRA_DIRT_WIDTH:
            return &extras->dirtWidth;
        case GDX_BRANCH_EDIT_EXTRA_ICE_WIDTH:
            return &extras->iceWidth;
        case GDX_BRANCH_EDIT_EXTRA_MAGNET_STRENGTH:
            return &extras->magnetStrength;
        case GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE:
            return &extras->magnetRange;
        default:
            return NULL;
    }
}

int gdx_branch_edit_point_extra(int point, int which) {
    uint8_t* field;

    if ((point < 0) || (point >= sLive.set.ringCount) || (point >= GDX_BRANCH_EDIT_MAX_POINTS)) {
        return 0;
    }
    field = extra_field(&sLive.ringExtras[point], which);
    return (field != NULL) ? *field : 0;
}

void gdx_branch_edit_set_point_extra(int point, int which, int value) {
    uint8_t* field;

    if ((point < 0) || (point >= sLive.set.ringCount) || (point >= GDX_BRANCH_EDIT_MAX_POINTS)) {
        return;
    }
    field = extra_field(&sLive.ringExtras[point], which);
    if (field != NULL) {
        *field = (uint8_t) extra_clamp(which, value);
        edit_publish();
    }
}

int gdx_branch_edit_track_setting(int which) {
    switch (which) {
        case GDX_BRANCH_EDIT_TRACK_LAVA:
            return sLive.lavaMode;
        case GDX_BRANCH_EDIT_TRACK_STRENGTH:
            return sLive.magnetStrength;
        case GDX_BRANCH_EDIT_TRACK_RANGE:
            return sLive.magnetRange;
        default:
            return 0;
    }
}

void gdx_branch_edit_set_track_setting(int which, int value) {
    switch (which) {
        case GDX_BRANCH_EDIT_TRACK_LAVA:
            sLive.lavaMode =
                (uint8_t) ((value < GDX_SETTINGS_LAVA_FOLLOW_MENU)   ? GDX_SETTINGS_LAVA_FOLLOW_MENU
                           : (value > GDX_SETTINGS_LAVA_NEVER_KILLS) ? GDX_SETTINGS_LAVA_NEVER_KILLS
                                                                     : value);
            break;
        case GDX_BRANCH_EDIT_TRACK_STRENGTH:
            sLive.magnetStrength = (uint8_t) extra_clamp(GDX_BRANCH_EDIT_EXTRA_MAGNET_STRENGTH, value);
            break;
        case GDX_BRANCH_EDIT_TRACK_RANGE:
            sLive.magnetRange = (uint8_t) extra_clamp(GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE, value);
            break;
        default:
            return;
    }
    edit_publish();
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
    static GdxPointExtras sOutExtras[GDX_BRANCH_EDIT_MAX_POINTS];
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
        sOutExtras[outCount] = sLive.ringExtras[i];
        sOut[outCount++] = ring[i];
        fork = (f < next.set.forkCount) ? &next.set.forks[f] : NULL;
        if ((fork != NULL) && (i == fork->forkPoint)) {
            newFork[f] = outCount - 1;
            if (fork->flags & GDX_BRANCH_FLAG_FIRST_INFO) {
                sOut[outCount - 1].trackSegmentInfo = fork->firstInfo;
            }
            if (outCount + fork->nodeCount > GDX_BRANCH_EDIT_MAX_POINTS) {
                return ringCount;
            }
            for (j = 0; j < fork->nodeCount; j++) {
                sOutExtras[outCount] = fork->nodes[j].extras;
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
            fork->nodes[j].extras = sLive.ringExtras[oldFork + 1 + j];
        }
        fork->nodeCount = (uint8_t) interior;
        fork->firstInfo = ring[oldFork].trackSegmentInfo;
        fork->flags |= GDX_BRANCH_FLAG_FIRST_INFO;
        fork->forkPoint = (uint8_t) newFork[f];
        fork->mergePoint = (uint8_t) newMerge[f];
    }
    for (i = 0; i < outCount; i++) {
        ring[i] = sOut[i];
    }
    memset(next.ringExtras, 0, sizeof(next.ringExtras));
    memcpy(next.ringExtras, sOutExtras, (size_t) outCount * sizeof(GdxPointExtras));
    next.set.ringCount = (uint8_t) outCount;
    next.viewB = !next.viewB;
    sLive = next;
    edit_publish();
    return outCount;
}

int gdx_branch_edit_swap_to_node(GdxBranchEditPoint* ring, int ringCount, int cap, int f, int node, int* point) {
    int viewBefore = sLive.viewB;
    int dropped;
    int count;
    int i;

    if ((f < 0) || (f >= sLive.set.forkCount) || (node < 0) || (node >= sLive.set.forks[f].nodeCount)) {
        return -1;
    }
    for (i = 0; i < sLive.set.forkCount; i++) {
        int interior = sLive.set.forks[i].mergePoint - sLive.set.forks[i].forkPoint - 1;

        if ((interior < 1) || (interior > GDX_BRANCH_MAX_NODES) || (sLive.set.forks[i].mergePoint >= ringCount)) {
            return -1;
        }
    }
    count = gdx_branch_edit_swap(ring, ringCount, cap, &dropped);
    if (sLive.viewB == viewBefore) {
        return -1;
    }
    *point = sLive.set.forks[f].forkPoint + 1 + node;
    return count;
}

/* The new point's segment was cut out of its neighbor's, so a part the insert kept keeps its values. */
static void extras_on_insert(int newIndex, const GdxBranchEditPoint* ring, int ringCount) {
    GdxPointExtras fresh;
    int i;

    if ((newIndex < 0) || (newIndex >= GDX_BRANCH_EDIT_MAX_POINTS)) {
        return;
    }
    memset(&fresh, 0, sizeof(fresh));
    if ((ring != NULL) && (ringCount > 1) && (newIndex < ringCount)) {
        const int before = (newIndex - 1 + ringCount) % ringCount;
        const GdxPointExtras* old = &sLive.ringExtras[before];

        if ((ring[newIndex].parts[0] != GDX_BRANCH_EDIT_PART_NONE) &&
            (ring[newIndex].parts[0] == ring[before].parts[0])) {
            fresh.pitWidth = old->pitWidth;
            fresh.magnetStrength = old->magnetStrength;
            fresh.magnetRange = old->magnetRange;
        }
        if ((ring[newIndex].parts[2] != GDX_BRANCH_EDIT_PART_NONE) &&
            (ring[newIndex].parts[2] == ring[before].parts[2])) {
            fresh.dirtWidth = old->dirtWidth;
        }
        if ((ring[newIndex].parts[3] != GDX_BRANCH_EDIT_PART_NONE) &&
            (ring[newIndex].parts[3] == ring[before].parts[3])) {
            fresh.iceWidth = old->iceWidth;
        }
    }
    for (i = GDX_BRANCH_EDIT_MAX_POINTS - 1; i > newIndex; i--) {
        sLive.ringExtras[i] = sLive.ringExtras[i - 1];
    }
    sLive.ringExtras[newIndex] = fresh;
}

void gdx_branch_edit_on_insert(int newIndex, const GdxBranchEditPoint* ring, int ringCount) {
    int f;

    extras_on_insert(newIndex, ring, ringCount);
    if (sLive.set.forkCount == 0) {
        sLive.set.ringCount = (uint8_t) ringCount;
        edit_publish();
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
        node_clear_parts(&node);
        if (gdx_branch_insert_node(fork, k, &node) != GDX_BRANCH_OK) {
            fork->flags &= (uint8_t) ~GDX_BRANCH_FLAG_SYMMETRIC;
        }
    }
    edit_publish();
}

void gdx_branch_edit_on_delete(int deletedIndex) {
    int i;

    if (sLive.set.ringCount > 0) {
        sLive.set.ringCount--;
    }
    if ((deletedIndex >= 0) && (deletedIndex < GDX_BRANCH_EDIT_MAX_POINTS)) {
        for (i = deletedIndex; i + 1 < GDX_BRANCH_EDIT_MAX_POINTS; i++) {
            sLive.ringExtras[i] = sLive.ringExtras[i + 1];
        }
        memset(&sLive.ringExtras[GDX_BRANCH_EDIT_MAX_POINTS - 1], 0, sizeof(GdxPointExtras));
    }
    if (sLive.set.forkCount == 0) {
        edit_publish();
        return;
    }
    gdx_branch_on_ring_delete(&sLive.set, deletedIndex);
    edit_publish();
}

void gdx_branch_edit_on_rotate(int newStart, int ringCount) {
    GdxPointExtras old[GDX_BRANCH_EDIT_MAX_POINTS];
    int f;
    int i;

    if ((ringCount <= 0) || (ringCount > GDX_BRANCH_EDIT_MAX_POINTS)) {
        return;
    }
    memcpy(old, sLive.ringExtras, sizeof(old));
    for (i = 0; i < ringCount; i++) {
        sLive.ringExtras[(i - newStart + ringCount) % ringCount] = old[i];
    }
    if (sLive.set.forkCount == 0) {
        edit_publish();
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
    GdxPointExtras old[GDX_BRANCH_EDIT_MAX_POINTS];
    int f;
    int i;

    if ((ringCount <= 0) || (ringCount > GDX_BRANCH_EDIT_MAX_POINTS)) {
        return;
    }
    /* The values belong to a segment, the same as the part they size: see func_xk2_800F0FF4. */
    memcpy(old, sLive.ringExtras, sizeof(old));
    for (i = 0; i < ringCount; i++) {
        sLive.ringExtras[i] = old[(ringCount - i) % ringCount];
    }
    if (sLive.set.forkCount == 0) {
        edit_publish();
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
        /* A style belongs to the segment after its point, and the segments now run the other way: the first
         * segment takes the old last one's style, and each node the style of the segment that now follows it. */
        if (fork->flags & GDX_BRANCH_FLAG_FIRST_INFO) {
            int32_t last = fork->nodes[0].trackSegmentInfo;

            for (i = 0; i + 1 < fork->nodeCount; i++) {
                fork->nodes[i].trackSegmentInfo = fork->nodes[i + 1].trackSegmentInfo;
            }
            fork->nodes[fork->nodeCount - 1].trackSegmentInfo = fork->firstInfo;
            fork->firstInfo = last;
        }
    }
    edit_sort();
    edit_publish();
}

void gdx_branch_edit_on_translate(float dx, float dz) {
    int f;
    int i;

    for (f = 0; f < sLive.set.forkCount; f++) {
        for (i = 0; i < sLive.set.forks[f].nodeCount; i++) {
            sLive.set.forks[f].nodes[i].pos[0] += dx;
            sLive.set.forks[f].nodes[i].pos[2] += dz;
        }
    }
    edit_publish();
}

int gdx_branch_edit_sync(int ringCount) {
    int removed = 0;
    int f;

    /* A ring change no hook reported: the indices are stale. */
    if (sLive.set.ringCount != ringCount) {
        extras_clear_ring();
    }
    if (sLive.set.forkCount == 0) {
        sLive.set.ringCount = (uint8_t) ringCount;
        edit_publish();
        return 0;
    }
    if (sLive.set.ringCount != ringCount) {
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
