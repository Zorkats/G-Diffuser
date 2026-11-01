/* port/gdx_course_branch.c -- race runtime of Course Edit forks. See gdx_course_branch.h. */

#include "global.h"
#include "fzx_course.h"
#include "fzx_racer.h"
#include "gdx_course_branch.h"
#include "gdx_course_branch_view.h"
#include "gdx_course_settings_view.h"

extern CourseInfo* gCurrentCourseInfo;
extern bool gInCourseEditor;
extern bool gInCourseEditTestRun;
extern s32 gNumPlayers;
extern s32 gSegmentChunkCount;
extern SegmentChunk gSegmentChunks[];

/* course.c: points gSegmentChunkCount and sLastSegmentChunk at a chunk ring of count chunks. */
extern void Course_GdxSetChunkRing(s32 count);
/* racer.c: the course file id ("NAME.EXT") of the course about to start; 0 when it has no file. */
extern int gdx_racer_course_file_id(char* out, int outCap);
extern void gdx_dbg_logf(const char* fmt, ...);
extern f32 Course_SplineGetTangent(CourseSegment* segment, f32 t, Vec3f* tangentVec);
extern void Course_SplineGetPosition(CourseSegment* segment, f32 t, Vec3f* pos);

#define BRANCH_SEGMENTS 64
/* The larger of the two stock gSegmentChunks sizes (game_context.c), so a copy never runs past a buffer. */
#define BRANCH_CHUNKS 1025
/* Steps for a walk onto the other ring. The walk starts from a point of the old spline, a few steps settle
 * it, and it stops early once it converges. */
#define BRANCH_REWALK_STEPS 12
/* A human racer in a fork's first segment moves to the other path only when it is this much nearer. */
#define BRANCH_SWITCH_MARGIN 20.0f
/* Part of a lap a lane crossing may move a racer by. */
#define BRANCH_LANE_MAX_LAP_JUMP 0.25f

#define SIDE_A 0
#define SIDE_B 1

typedef struct {
    s32 ring1Fork;  /* stock ring index of F */
    s32 ring1Merge; /* stock ring index of M */
    s32 ring2Fork;  /* ring 2 index of F */
    s32 ring2Merge; /* ring 2 index of M */
    s32 setFork;    /* index of the fork in the active set */
    f32 lapScale;   /* stock span length / ring 2 span length */
} BranchFork;

typedef struct {
    CourseSegment* lastSegment;
    u8 side[GDX_BRANCH_VIEW_MAX_FORKS]; /* the path the racer last took through each fork */
} BranchRacer;

static struct {
    s32 live;
    CourseInfo* courseInfo;
    CourseSegment* ring1;
    s32 ring1Count;
    s32 ring1ChunkCount;
    s8 ring1ForkOf[BRANCH_SEGMENTS]; /* fork whose A span (F..M-1) holds the stock segment, else -1 */
    s32 forkCount;
    BranchFork forks[GDX_BRANCH_VIEW_MAX_FORKS];
    CourseSegment ring2[BRANCH_SEGMENTS];
    s32 ring2Count;
    s16 ring2Source[BRANCH_SEGMENTS]; /* stock index of a copied point, -1 for a B node */
    s8 ring2ForkOf[BRANCH_SEGMENTS];  /* fork whose B span (F..last B node) holds the segment, else -1 */
    s32 ring2ChunkCount;
    s32 ring2SpanChunks;
} sBranch;

static SegmentChunk sRing2Chunks[BRANCH_CHUNKS];
static SegmentChunk sRing1ChunkSave[BRANCH_CHUNKS];
static u8 sRing2ChunkMask[BRANCH_CHUNKS];
static BranchRacer sRacers[TOTAL_RACER_COUNT];

const u8* gGdxCourseChunkDrawMask = NULL;

/* Control point of race segment 0, -1 when no rotation matched. */
static s32 sSegmentRotation;
static s32 sSegmentCount;

static f32 branch_abs(f32 v) {
    return (v < 0.0f) ? -v : v;
}

void gdx_course_branch_reset_racers(void) {
    static const BranchRacer sFreshRacer;
    s32 i;

    for (i = 0; i < TOTAL_RACER_COUNT; i++) {
        sRacers[i] = sFreshRacer;
    }
}

void gdx_course_branch_clear(void) {
    sBranch.live = 0;
    sBranch.courseInfo = NULL;
    sBranch.ring1 = NULL;
    sBranch.ring1Count = 0;
    sBranch.forkCount = 0;
    sBranch.ring2Count = 0;
    sBranch.ring2ChunkCount = 0;
    sBranch.ring2SpanChunks = 0;
    gGdxCourseChunkDrawMask = NULL;
    gdx_course_branch_reset_racers();
}

s32 gdx_course_branch_live(void) {
    return sBranch.live && (gCurrentCourseInfo == sBranch.courseInfo) &&
           (sBranch.courseInfo->courseSegments == sBranch.ring1) &&
           (sBranch.courseInfo->segmentCount == sBranch.ring1Count);
}

/* The stock ring is the course's control points rotated so the start segment is index 0
 * (Course_SegmentsInit) and centered on x and z (func_80074634). Finds the rotation and the centering
 * offset by matching positions, which also proves the fork indices (control point indices) still describe
 * this ring. Returns the rotation, or -1 when no rotation matches. */
static s32 branch_find_rotation(const CourseData* courseData, const CourseSegment* ring, s32 count, f32* offsetX,
                                f32* offsetZ) {
    s32 rotation;
    s32 i;

    for (rotation = 0; rotation < count; rotation++) {
        f32 dx = ring[0].pos.x - courseData->controlPoint[rotation].pos.x;
        f32 dz = ring[0].pos.z - courseData->controlPoint[rotation].pos.z;

        for (i = 0; i < count; i++) {
            const ControlPoint* point = &courseData->controlPoint[(rotation + i) % count];

            if ((branch_abs(ring[i].pos.y - point->pos.y) > 0.05f) ||
                (branch_abs(ring[i].pos.x - point->pos.x - dx) > 0.05f) ||
                (branch_abs(ring[i].pos.z - point->pos.z - dz) > 0.05f)) {
                break;
            }
        }
        if (i == count) {
            *offsetX = dx;
            *offsetZ = dz;
            return rotation;
        }
    }
    return -1;
}

/* Is the point on the other road of fork f? chunks and forkOf are that road's ring. */
static s32 branch_edge_on_other_road(const s16 point[3], const SegmentChunk* chunks, s32 count, const s8* forkOf, s32 f) {
    const SegmentChunk* nearest = NULL;
    f32 nearestSq = 0.0f;
    s32 i;

    for (i = 0; i < count; i++) {
        const SegmentChunk* chunk = &chunks[i];
        s32 shape = chunk->trackSegmentInfo & TRACK_SHAPE_MASK;
        f32 dx;
        f32 dy;
        f32 dz;
        f32 distanceSq;

        if ((chunk->segmentIndex < 0) || (chunk->segmentIndex >= BRANCH_SEGMENTS) || (forkOf[chunk->segmentIndex] != f) ||
            ((shape != TRACK_SHAPE_ROAD) && (shape != TRACK_SHAPE_BORDERLESS_ROAD))) {
            continue;
        }
        dx = (f32) point[0] - chunk->referencePos2[0];
        dy = (f32) point[1] - chunk->referencePos2[1];
        dz = (f32) point[2] - chunk->referencePos2[2];
        distanceSq = (dx * dx) + (dy * dy) + (dz * dz);
        if ((nearest == NULL) || (distanceSq < nearestSq)) {
            nearest = chunk;
            nearestSq = distanceSq;
        }
    }
    return (nearest != NULL) &&
           gdx_branch_view_chunk_covers_point(point, nearest->referencePos2, nearest->referencePos5,
                                              nearest->referencePos8, nearest->referencePos1);
}

/* Where a road's edge lies on the other road, ships cross it, so it draws no rail. Moving the rail top onto the
 * road edge flattens the rail, and the shared vertices make it taper from the chunk where the roads part, the V of
 * a GX lane split. Only the plain road has a rail to lose: a walled road's walls stay and stop the ship. */
static void branch_flatten_rails(SegmentChunk* chunks, s32 count, const s8* forkOf, const SegmentChunk* otherChunks,
                                 s32 otherCount, const s8* otherForkOf) {
    s32 i;

    for (i = 0; i < count; i++) {
        SegmentChunk* chunk = &chunks[i];
        s32 f;

        if ((chunk->segmentIndex < 0) || (chunk->segmentIndex >= BRANCH_SEGMENTS) ||
            ((chunk->trackSegmentInfo & TRACK_SHAPE_MASK) != TRACK_SHAPE_ROAD)) {
            continue;
        }
        f = forkOf[chunk->segmentIndex];
        if (f < 0) {
            continue;
        }
        if (branch_edge_on_other_road(chunk->referencePos5, otherChunks, otherCount, otherForkOf, f)) {
            memcpy(chunk->referencePos4, chunk->referencePos5, sizeof(chunk->referencePos4));
        }
        if (branch_edge_on_other_road(chunk->referencePos8, otherChunks, otherCount, otherForkOf, f)) {
            memcpy(chunk->referencePos7, chunk->referencePos8, sizeof(chunk->referencePos7));
        }
    }
}

/* editor: the ring is the Course Edit ring (control point order, uncentered, its own trackSegmentInfo),
 * which the course data's control points do not describe until the course is saved. */
static void branch_build(s32 editor) {
    static const CourseSegment sFreshSegment;
    CourseInfo* info = gCurrentCourseInfo;
    CourseData* courseData = &COURSE_CONTEXT()->courseData;
    CourseSegment* ring1;
    CourseInfo ring2Info;
    s16 savedBankAngle[BRANCH_SEGMENTS];
    s16 ring2BankAngle[BRANCH_SEGMENTS];
    char reason[96];
    f32 offsetX;
    f32 offsetZ;
    s32 rotation;
    s32 count;
    s32 ring2Count;
    s32 ring1ChunkCount;
    s32 buildResult;
    s32 savedInCourseEditor;
    s32 setForkCount;
    s32 f;
    s32 i;
    s32 j;

    gdx_course_branch_clear();
    setForkCount = gdx_branch_view_fork_count();
    if ((setForkCount == 0) || (info == NULL) || (info->courseSegments == NULL)) {
        return;
    }
    count = info->segmentCount;
    ring1 = info->courseSegments;
    if ((count < 4) || (count > BRANCH_SEGMENTS) || (!editor && (courseData->controlPointCount != count)) ||
        (gdx_branch_view_ring_count() != count)) {
        gdx_dbg_logf("[fork] off: the fork set is for %d points, the course has %d\n", gdx_branch_view_ring_count(),
                     (int) count);
        return;
    }
    if (gdx_branch_view_validate(count, reason, sizeof(reason)) != 0) {
        gdx_dbg_logf("[fork] off: %s\n", reason);
        return;
    }
    if (editor) {
        rotation = 0;
        offsetX = offsetZ = 0.0f;
    } else {
        rotation = branch_find_rotation(courseData, ring1, count, &offsetX, &offsetZ);
    }
    if (rotation < 0) {
        gdx_dbg_logf("[fork] off: the course ring does not match its control points\n");
        return;
    }

    /* Forks in stock ring order. A fork the rotation carries across the start segment is left out. */
    for (f = 0; (f < setForkCount) && (f < GDX_BRANCH_VIEW_MAX_FORKS); f++) {
        BranchFork fork;
        int forkPoint;
        int mergePoint;
        int nodeCount;

        if (!gdx_branch_view_fork(f, &forkPoint, &mergePoint, &nodeCount)) {
            return;
        }
        fork.setFork = f;
        fork.ring1Fork = (forkPoint - rotation + count) % count;
        fork.ring1Merge = (mergePoint - rotation + count) % count;
        if (fork.ring1Fork >= fork.ring1Merge) {
            gdx_dbg_logf("[fork] fork %d left out: it crosses the start segment\n", (int) f);
            continue;
        }
        fork.ring2Fork = fork.ring2Merge = -1;
        fork.lapScale = 1.0f;
        for (i = sBranch.forkCount; (i > 0) && (sBranch.forks[i - 1].ring1Fork > fork.ring1Fork); i--) {
            sBranch.forks[i] = sBranch.forks[i - 1];
        }
        sBranch.forks[i] = fork;
        sBranch.forkCount++;
    }
    if (sBranch.forkCount == 0) {
        return;
    }

    for (i = 0; i < BRANCH_SEGMENTS; i++) {
        sBranch.ring1ForkOf[i] = -1;
        sBranch.ring2ForkOf[i] = -1;
        sBranch.ring2[i] = sFreshSegment;
    }
    for (f = 0; f < sBranch.forkCount; f++) {
        for (i = sBranch.forks[f].ring1Fork; i < sBranch.forks[f].ring1Merge; i++) {
            sBranch.ring1ForkOf[i] = (s8) f;
        }
    }

    /* Ring 2's points: the stock ring with each A interior replaced by the B nodes. */
    ring2Count = 0;
    for (i = 0; i < count; i++) {
        int forkPoint;
        int mergePoint;
        int nodeCount;

        f = sBranch.ring1ForkOf[i];
        if ((f >= 0) && (i != sBranch.forks[f].ring1Fork)) {
            continue; /* A interior */
        }
        if (ring2Count >= BRANCH_SEGMENTS) {
            gdx_dbg_logf("[fork] off: the second ring needs more than %d points\n", BRANCH_SEGMENTS);
            return;
        }
        sBranch.ring2[ring2Count].pos = ring1[i].pos;
        sBranch.ring2[ring2Count].radiusLeft = ring1[i].radiusLeft;
        sBranch.ring2[ring2Count].radiusRight = ring1[i].radiusRight;
        sBranch.ring2[ring2Count].trackSegmentInfo =
            editor ? ring1[i].trackSegmentInfo : courseData->controlPoint[(rotation + i) % count].trackSegmentInfo;
        sBranch.ring2Source[ring2Count] = (s16) i;
        ring2BankAngle[ring2Count] = courseData->bankAngle[i];
        ring2Count++;
        if (f < 0) {
            continue;
        }

        /* i is F: the fork's B nodes follow it. */
        if (!gdx_branch_view_fork(sBranch.forks[f].setFork, &forkPoint, &mergePoint, &nodeCount) ||
            (ring2Count + nodeCount > BRANCH_SEGMENTS)) {
            gdx_dbg_logf("[fork] off: the second ring cannot hold fork %d\n", (int) f);
            return;
        }
        sBranch.forks[f].ring2Fork = ring2Count - 1;
        /* F is shared, but the segment it starts is this path's own. */
        {
            int firstInfo;

            if (gdx_branch_view_fork_first_info(sBranch.forks[f].setFork, &firstInfo)) {
                sBranch.ring2[ring2Count - 1].trackSegmentInfo = firstInfo;
            }
        }
        for (j = 0; j < nodeCount; j++) {
            CourseSegment* segment = &sBranch.ring2[ring2Count];
            float pos[3];
            short radiusLeft;
            short radiusRight;
            int trackSegmentInfo;
            short bankAngle;

            if (!gdx_branch_view_node(sBranch.forks[f].setFork, j, pos, &radiusLeft, &radiusRight, &trackSegmentInfo,
                                      &bankAngle)) {
                return;
            }
            segment->pos.x = pos[0] + offsetX;
            segment->pos.y = pos[1];
            segment->pos.z = pos[2] + offsetZ;
            segment->radiusLeft = radiusLeft;
            segment->radiusRight = radiusRight;
            segment->trackSegmentInfo = trackSegmentInfo;
            sBranch.ring2Source[ring2Count] = -1;
            ring2BankAngle[ring2Count] = bankAngle;
            ring2Count++;
        }
    }
    for (f = 0; f < sBranch.forkCount; f++) {
        for (j = 0; j < ring2Count; j++) {
            if (sBranch.ring2Source[j] == sBranch.forks[f].ring1Merge) {
                sBranch.forks[f].ring2Merge = j;
            }
        }
        if ((sBranch.forks[f].ring2Fork < 0) || (sBranch.forks[f].ring2Merge <= sBranch.forks[f].ring2Fork)) {
            gdx_dbg_logf("[fork] off: fork %d has no span on the second ring\n", f);
            return;
        }
    }

    for (j = 0; j < ring2Count; j++) {
        sBranch.ring2[j].segmentIndex = j;
        sBranch.ring2[j].next = &sBranch.ring2[(j + 1) % ring2Count];
        sBranch.ring2[j].prev = &sBranch.ring2[(j + ring2Count - 1) % ring2Count];
    }
    ring2Info = *info;
    ring2Info.courseSegments = sBranch.ring2;
    ring2Info.segmentCount = ring2Count;

    /* The stock setup, in the stock order, on ring 2. func_80074CE4 reads the bank angle of segment i from
     * the course data, which Course_SegmentsInit left in stock ring order, so ring 2's angles stand in for
     * the call. Every up vector is computed again: at F and M the tangent differs from the stock ring's. */
    if (Course_SplineCalculateTensions(&ring2Info) != 0) {
        gdx_dbg_logf("[fork] off: a path B bends too sharply for the track spline\n");
        return;
    }
    for (j = 0; j < ring2Count; j++) {
        savedBankAngle[j] = courseData->bankAngle[j];
        courseData->bankAngle[j] = ring2BankAngle[j];
    }
    func_80074CE4(&ring2Info);
    for (j = 0; j < ring2Count; j++) {
        courseData->bankAngle[j] = savedBankAngle[j];
    }
    Course_SegmentLengthsInit(&ring2Info);

    /* The editor flags too-short joins on its own ring and keeps one basis per chunk; neither may see ring 2. */
    savedInCourseEditor = gInCourseEditor;
    gInCourseEditor = false;
    Course_SegmentJoinsInit(&ring2Info);
    Course_SegmentContinuousFlagInit(&ring2Info);
    Course_SegmentFormsInit(&ring2Info);

    ring1ChunkCount = gSegmentChunkCount;
    if ((ring1ChunkCount < 3) || (ring1ChunkCount + 1 > BRANCH_CHUNKS)) {
        gInCourseEditor = savedInCourseEditor;
        gdx_dbg_logf("[fork] off: the stock chunk ring is not built\n");
        return;
    }
    memcpy(sRing1ChunkSave, gSegmentChunks, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    buildResult = func_800A1954(&ring2Info);
    sBranch.ring2ChunkCount = gSegmentChunkCount;
    if (!(buildResult & 0x20000) && (sBranch.ring2ChunkCount >= 3) && (sBranch.ring2ChunkCount + 1 <= BRANCH_CHUNKS)) {
        memcpy(sRing2Chunks, gSegmentChunks, (sBranch.ring2ChunkCount + 1) * sizeof(SegmentChunk));
    } else {
        sBranch.ring2ChunkCount = 0;
    }
    memcpy(gSegmentChunks, sRing1ChunkSave, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(ring1ChunkCount);
    gInCourseEditor = savedInCourseEditor;
    if (sBranch.ring2ChunkCount == 0) {
        gdx_dbg_logf("[fork] off: the second ring has too many chunks\n");
        return;
    }

    /* Lap distance: each B span is scaled into the stock span F..M. */
    for (f = 0; f < sBranch.forkCount; f++) {
        BranchFork* fork = &sBranch.forks[f];
        f32 ring1Start = ring1[fork->ring1Fork].lengthFromStart;
        f32 ring1Span = ring1[fork->ring1Merge].lengthFromStart - ring1Start;
        f32 ring2Start = sBranch.ring2[fork->ring2Fork].lengthFromStart;
        f32 ring2Span = sBranch.ring2[fork->ring2Merge].lengthFromStart - ring2Start;

        if ((ring1Span <= 0.0f) || (ring2Span <= 0.0f)) {
            gdx_dbg_logf("[fork] off: fork %d has an empty span\n", f);
            return;
        }
        fork->lapScale = ring1Span / ring2Span;
        for (j = fork->ring2Fork; j < fork->ring2Merge; j++) {
            sBranch.ring2ForkOf[j] = (s8) f;
            sBranch.ring2[j].lengthFromStart = ring1Start + (sBranch.ring2[j].lengthFromStart - ring2Start) * fork->lapScale;
        }
    }

    /* Outside the B spans ring 2 mirrors the stock ring, so it shares the stock gadgets and lengths; a racer
     * that lands there moves back to the stock ring on its next walk. A B span has no gadgets yet. Every
     * ring 2 segment reports a stock segment index, so the per-segment tables (CPU, hazards) stay in range;
     * a B segment reports its fork point. */
    for (j = 0; j < ring2Count; j++) {
        CourseSegment* segment = &sBranch.ring2[j];
        s32 source = sBranch.ring2Source[j];

        f = sBranch.ring2ForkOf[j];
        if (f < 0) {
            segment->lengthFromStart = ring1[source].lengthFromStart;
            segment->jumpsStart = ring1[source].jumpsStart;
            segment->jumpsEnd = ring1[source].jumpsEnd;
            segment->landminesStart = ring1[source].landminesStart;
            segment->landminesEnd = ring1[source].landminesEnd;
            segment->effectsStart = ring1[source].effectsStart;
            segment->effectsEnd = ring1[source].effectsEnd;
            memcpy(segment->unk_5C, ring1[source].unk_5C, sizeof(segment->unk_5C));
            segment->unk_64 = ring1[source].unk_64;
            segment->segmentIndex = source;
        } else {
            segment->jumpsStart = segment->jumpsEnd = NULL;
            segment->landminesStart = segment->landminesEnd = NULL;
            segment->effectsStart = segment->effectsEnd = NULL;
            segment->segmentIndex = sBranch.forks[f].ring1Fork;
        }
    }

    /* Only the B spans are drawn from ring 2. */
    sBranch.ring2SpanChunks = 0;
    for (i = 0; i <= sBranch.ring2ChunkCount; i++) {
        s32 segmentIndex = sRing2Chunks[i].segmentIndex;

        sRing2ChunkMask[i] = (i < sBranch.ring2ChunkCount) && (segmentIndex >= 0) && (segmentIndex < ring2Count) &&
                             (sBranch.ring2ForkOf[segmentIndex] >= 0);
        sBranch.ring2SpanChunks += sRing2ChunkMask[i];
    }

    /* The two passes read each other's edges, and a flattened rail keeps its road edge, so the order is free. */
    branch_flatten_rails(gSegmentChunks, ring1ChunkCount, sBranch.ring1ForkOf, sRing2Chunks, sBranch.ring2ChunkCount,
                         sBranch.ring2ForkOf);
    branch_flatten_rails(sRing2Chunks, sBranch.ring2ChunkCount, sBranch.ring2ForkOf, gSegmentChunks, ring1ChunkCount,
                         sBranch.ring1ForkOf);

    sBranch.courseInfo = info;
    sBranch.ring1 = ring1;
    sBranch.ring1Count = count;
    sBranch.ring1ChunkCount = ring1ChunkCount;
    sBranch.ring2Count = ring2Count;
    sBranch.live = 1;
    gdx_dbg_logf("[fork] %d fork(s) live: %d stock points, %d on the second ring, %d chunks (%d drawn)\n",
                 sBranch.forkCount, count, ring2Count, sBranch.ring2ChunkCount, sBranch.ring2SpanChunks);
}

void gdx_course_branch_build(void) {
    branch_build(0);
}

void gdx_course_branch_build_editor(void) {
    branch_build(1);
}

void gdx_course_branch_on_segments_init(const CourseSegment* ring, s32 count) {
    f32 offsetX;
    f32 offsetZ;

    /* The editor and its test run read the values the edit library publishes. */
    if (!gInCourseEditor && !gInCourseEditTestRun) {
        char courseId[26]; /* GDX_BRANCH_COURSE_ID_MAX + 1 */

        gdx_settings_view_select_course(gdx_racer_course_file_id(courseId, sizeof(courseId)) ? courseId : NULL, count);
    }
    /* Matched by position, not from the rotation Course_SegmentsInit just did: a test run restart rotates a ring
     * that is already rotated. */
    sSegmentRotation = branch_find_rotation(&COURSE_CONTEXT()->courseData, ring, count, &offsetX, &offsetZ);
    sSegmentCount = count;
}

s32 gdx_course_branch_control_point(s32 segmentIndex) {
    if (gInCourseEditor && !gInCourseEditTestRun) {
        return segmentIndex;
    }
    if ((sSegmentRotation < 0) || (sSegmentCount <= 0)) {
        return -1;
    }
    return (sSegmentRotation + segmentIndex) % sSegmentCount;
}

void gdx_course_branch_on_course_init(void) {
    if (!gInCourseEditor && !gInCourseEditTestRun) {
        char courseId[26]; /* GDX_BRANCH_COURSE_ID_MAX + 1 */

        gdx_branch_view_select_course(gdx_racer_course_file_id(courseId, sizeof(courseId)) ? courseId : NULL);
    }
    /* The editor builds ring 2 itself once its course is set up, and again after each edit. A test run races
     * the editor's fork set. */
    if (gInCourseEditor && !gInCourseEditTestRun) {
        gdx_course_branch_clear();
        return;
    }
    gdx_course_branch_build();
}

/* Walks a copy of info on another segment from the point info last stood on. Returns 0 when it settled. */
static s32 branch_walk_copy(RacerSegmentPositionInfo* out, const RacerSegmentPositionInfo* info, CourseSegment* segment) {
    *out = *info;
    out->courseSegment = segment;
    out->lastGroundedPos = out->segmentPos;
    /* A negative threshold makes the walker step until it converges or runs out of steps. */
    return func_8009EBEC(out, info->pos.x, info->pos.y, info->pos.z, BRANCH_REWALK_STEPS, -1.0f);
}

/* Moves info onto segment. Returns 1 when it moved. */
static s32 branch_move(RacerSegmentPositionInfo* info, CourseSegment* segment) {
    RacerSegmentPositionInfo moved;

    if (branch_walk_copy(&moved, info, segment) != 0) {
        return 0;
    }
    *info = moved;
    return 1;
}

/* In fork f's first segment: CPUs take a fixed path, humans the path they are nearer to. */
static void branch_choose(BranchRacer* racer, s32 racerId, s32 f, RacerSegmentPositionInfo* info, s32 onB) {
    CourseSegment* other = onB ? &sBranch.ring1[sBranch.forks[f].ring1Fork] : &sBranch.ring2[sBranch.forks[f].ring2Fork];

    if (racerId >= gNumPlayers) {
        s32 wantB = (racerId + f) & 1;

        if ((wantB != onB) && branch_move(info, other)) {
            onB = wantB;
        }
    } else {
        RacerSegmentPositionInfo alternative;

        if ((branch_walk_copy(&alternative, info, other) == 0) &&
            ((alternative.distanceFromSegment + BRANCH_SWITCH_MARGIN) < info->distanceFromSegment)) {
            *info = alternative;
            onB = !onB;
        }
    }
    racer->side[f] = onB ? SIDE_B : SIDE_A;
}

static s32 branch_flat_road(const CourseSegment* segment) {
    s32 shape = segment->trackSegmentInfo & TRACK_SHAPE_MASK;

    return (shape == TRACK_SHAPE_ROAD) || (shape == TRACK_SHAPE_BORDERLESS_ROAD);
}

/* The same basis the racer computes after its walk (racer.c). segmentLengthProportion must be current. */
static s32 branch_lane_sample(const RacerSegmentPositionInfo* info, GdxBranchLaneSample* out) {
    const CourseSegment* segment = info->courseSegment;
    const CourseSegment* next = segment->next;
    const f32 along = info->segmentLengthProportion;
    Vec3f x;
    Vec3f y;
    Vec3f z;
    f32 scale;

    if (info->segmentForwardMagnitude <= 0.0f) {
        return 0;
    }
    x.x = info->segmentForward.x / info->segmentForwardMagnitude;
    x.y = info->segmentForward.y / info->segmentForwardMagnitude;
    x.z = info->segmentForward.z / info->segmentForwardMagnitude;
    y.x = segment->up.x + along * (next->up.x - segment->up.x);
    y.y = segment->up.y + along * (next->up.y - segment->up.y);
    y.z = segment->up.z + along * (next->up.z - segment->up.z);
    z.x = (y.y * x.z) - (y.z * x.y);
    z.y = (y.z * x.x) - (y.x * x.z);
    z.z = (y.x * x.y) - (y.y * x.x);
    scale = sqrtf((z.x * z.x) + (z.y * z.y) + (z.z * z.z));
    if (scale <= 0.0f) {
        return 0;
    }
    z.x /= scale;
    z.y /= scale;
    z.z /= scale;
    y.x = (x.y * z.z) - (x.z * z.y);
    y.y = (x.z * z.x) - (x.x * z.z);
    y.z = (x.x * z.y) - (x.y * z.x);
    out->lateral = (z.x * info->segmentDisplacement.x) + (z.y * info->segmentDisplacement.y) +
                   (z.z * info->segmentDisplacement.z);
    out->height = (y.x * info->segmentDisplacement.x) + (y.y * info->segmentDisplacement.y) +
                  (y.z * info->segmentDisplacement.z);
    out->radiusLeft = segment->radiusLeft + along * (next->radiusLeft - segment->radiusLeft);
    out->radiusRight = segment->radiusRight + along * (next->radiusRight - segment->radiusRight);
    return 1;
}

/* Puts out on the span point nearest to info's position, ready for a walk. The other road can be anywhere along its
 * span, so a walk from a fixed start could run out of steps. */
static void branch_lane_start(RacerSegmentPositionInfo* out, const RacerSegmentPositionInfo* info, CourseSegment* first,
                              s32 count) {
    CourseSegment* bestSegment = first;
    f32 bestT = 0.0f;
    f32 bestSq = -1.0f;
    s32 i;
    s32 mark;

    for (i = 0; i < count; i++) {
        CourseSegment* segment = &first[i];
        const Vec3f* marks[4];

        marks[0] = &segment->pos;
        marks[1] = &segment->quarterMarkPos;
        marks[2] = &segment->halfMarkPos;
        marks[3] = &segment->threeQuarterMarkPos;
        for (mark = 0; mark < 4; mark++) {
            f32 dx = info->pos.x - marks[mark]->x;
            f32 dy = info->pos.y - marks[mark]->y;
            f32 dz = info->pos.z - marks[mark]->z;
            f32 distanceSq = (dx * dx) + (dy * dy) + (dz * dz);

            if ((bestSq < 0.0f) || (distanceSq < bestSq)) {
                bestSq = distanceSq;
                bestSegment = segment;
                bestT = mark * 0.25f;
            }
        }
    }
    *out = *info;
    out->courseSegment = bestSegment;
    out->segmentTValue = bestT;
    Course_SplineGetPosition(bestSegment, bestT, &out->segmentPos);
    out->segmentForwardMagnitude = Course_SplineGetTangent(bestSegment, bestT, &out->segmentForward);
    out->lastGroundedPos = out->segmentPos;
}

/* A human racer who left the road's edge inside a fork, and is on the other path's road, crosses to it. CPUs keep
 * the path they took: they follow one ring's racing line. */
static void branch_lane_cross(BranchRacer* racer, s32 racerId, RacerSegmentPositionInfo* info) {
    CourseSegment* segment = info->courseSegment;
    RacerSegmentPositionInfo moved;
    GdxBranchLaneSample current;
    GdxBranchLaneSample other;
    CourseSegment* first;
    const BranchFork* fork;
    s32 count;
    s32 onB;
    s32 f;
    s32 index;
    f32 lapBefore;
    f32 lapAfter;

    if (racerId >= gNumPlayers) {
        return;
    }
    onB = (segment >= sBranch.ring2) && (segment < sBranch.ring2 + sBranch.ring2Count);
    if (onB) {
        f = sBranch.ring2ForkOf[segment - sBranch.ring2];
    } else if ((segment >= sBranch.ring1) && (segment < sBranch.ring1 + sBranch.ring1Count)) {
        f = sBranch.ring1ForkOf[segment - sBranch.ring1];
    } else {
        return;
    }
    if ((f < 0) || !branch_flat_road(segment)) {
        return;
    }
    lapBefore = func_8009DFA0(info);
    if (!branch_lane_sample(info, &current) ||
        ((current.lateral <= current.radiusLeft) && (current.lateral >= -current.radiusRight))) {
        return;
    }

    fork = &sBranch.forks[f];
    first = onB ? &sBranch.ring1[fork->ring1Fork] : &sBranch.ring2[fork->ring2Fork];
    count = onB ? (fork->ring1Merge - fork->ring1Fork) : (fork->ring2Merge - fork->ring2Fork);
    branch_lane_start(&moved, info, first, count);
    if (func_8009EBEC(&moved, info->pos.x, info->pos.y, info->pos.z, BRANCH_REWALK_STEPS, -1.0f) != 0) {
        return;
    }
    index = moved.courseSegment - first;
    if ((index < 0) || (index >= count) || !branch_flat_road(moved.courseSegment)) {
        return;
    }
    lapAfter = func_8009DFA0(&moved);
    if (!branch_lane_sample(&moved, &other) || !gdx_branch_view_lane_crossing(&current, &other)) {
        return;
    }
    /* The two paths share lap distance in proportion. A racer far along one and early on the other must not gain
     * or lose a large part of a lap, or a lap: this edge stays a wall. */
    if (branch_abs(lapAfter - lapBefore) > (gCurrentCourseInfo->length * BRANCH_LANE_MAX_LAP_JUMP)) {
        return;
    }
    *info = moved;
    racer->side[f] = onB ? SIDE_A : SIDE_B;
}

void gdx_course_branch_racer_walked(s32 racerId, RacerSegmentPositionInfo* info) {
    BranchRacer* racer;
    CourseSegment* segment;
    s32 f;
    s32 i;

    if ((racerId < 0) || (racerId >= TOTAL_RACER_COUNT) || !gdx_course_branch_live()) {
        return;
    }
    racer = &sRacers[racerId];
    segment = info->courseSegment;

    if ((segment >= sBranch.ring2) && (segment < sBranch.ring2 + sBranch.ring2Count)) {
        i = segment - sBranch.ring2;
        f = sBranch.ring2ForkOf[i];
        if (f < 0) {
            /* Past the merge, or back before the fork: the stock ring again. */
            branch_move(info, &sBranch.ring1[sBranch.ring2Source[i]]);
        } else if (i == sBranch.forks[f].ring2Fork) {
            branch_choose(racer, racerId, f, info, 1);
        } else {
            racer->side[f] = SIDE_B;
        }
    } else if ((segment >= sBranch.ring1) && (segment < sBranch.ring1 + sBranch.ring1Count)) {
        i = segment - sBranch.ring1;
        f = sBranch.ring1ForkOf[i];
        if (f >= 0) {
            const BranchFork* fork = &sBranch.forks[f];
            CourseSegment* last = racer->lastSegment;

            if (i == fork->ring1Fork) {
                branch_choose(racer, racerId, f, info, 0);
            } else if ((racer->side[f] == SIDE_B) && (last >= sBranch.ring1) &&
                       (last < sBranch.ring1 + sBranch.ring1Count) && ((last - sBranch.ring1) >= fork->ring1Merge)) {
                /* Driving backwards over the merge after taking path B: back onto B. */
                if (!branch_move(info, &sBranch.ring2[fork->ring2Merge - 1])) {
                    racer->side[f] = SIDE_A;
                }
            } else {
                racer->side[f] = SIDE_A;
            }
        }
    }
    branch_lane_cross(racer, racerId, info);
    racer->lastSegment = info->courseSegment;
}

f32 gdx_course_branch_lap_partial(const CourseSegment* segment, f32 partial) {
    s32 f;

    if (!sBranch.live || (segment < sBranch.ring2) || (segment >= sBranch.ring2 + sBranch.ring2Count)) {
        return partial;
    }
    f = sBranch.ring2ForkOf[segment - sBranch.ring2];
    return (f >= 0) ? partial * sBranch.forks[f].lapScale : partial;
}

Gfx* gdx_course_branch_draw(Gfx* gfx, s32 cameraIndex, Gfx* (*drawRing)(Gfx*, s32)) {
    s32 ring1ChunkCount = gSegmentChunkCount;

    /* The stock chunk draw stops itself near the end of the course vertex buffer (EXPANSION_KIT guard), so
     * this pass needs no budget of its own. */
    if (!gdx_course_branch_live() || (sBranch.ring2SpanChunks == 0) || (ring1ChunkCount != sBranch.ring1ChunkCount)) {
        return gfx;
    }

    memcpy(sRing1ChunkSave, gSegmentChunks, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    memcpy(gSegmentChunks, sRing2Chunks, (sBranch.ring2ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(sBranch.ring2ChunkCount);
    gGdxCourseChunkDrawMask = sRing2ChunkMask;
    gfx = drawRing(gfx, cameraIndex);
    gGdxCourseChunkDrawMask = NULL;
    memcpy(gSegmentChunks, sRing1ChunkSave, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(ring1ChunkCount);
    return gfx;
}

/* Course Edit draws its surface with func_800A95B4 (no draw state), which skips chunks the mask leaves out. */
Gfx* gdx_course_branch_draw_editor(Gfx* gfx, Gfx* (*drawSurface)(Gfx*)) {
    s32 ring1ChunkCount = gSegmentChunkCount;

    if (!gdx_course_branch_live() || (sBranch.ring2SpanChunks == 0) || (ring1ChunkCount != sBranch.ring1ChunkCount)) {
        return gfx;
    }

    memcpy(sRing1ChunkSave, gSegmentChunks, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    memcpy(gSegmentChunks, sRing2Chunks, (sBranch.ring2ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(sBranch.ring2ChunkCount);
    gGdxCourseChunkDrawMask = sRing2ChunkMask;
    gfx = drawSurface(gfx);
    gGdxCourseChunkDrawMask = NULL;
    memcpy(gSegmentChunks, sRing1ChunkSave, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(ring1ChunkCount);
    return gfx;
}
