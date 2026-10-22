/* port/gdx_course_branch.c -- race runtime of Course Edit forks. See gdx_course_branch.h. */

#include "global.h"
#include "fzx_course.h"
#include "fzx_racer.h"
#include "gdx_course_branch.h"
#include "gdx_course_branch_view.h"

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

#define BRANCH_SEGMENTS 64
/* The larger of the two stock gSegmentChunks sizes (game_context.c), so a copy never runs past a buffer. */
#define BRANCH_CHUNKS 1025
/* Steps for a walk onto the other ring. The walk starts from a point of the old spline, a few steps settle
 * it, and it stops early once it converges. */
#define BRANCH_REWALK_STEPS 12
/* A human racer in a fork's first segment moves to the other path only when it is this much nearer. */
#define BRANCH_SWITCH_MARGIN 20.0f

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
