/* port/gdx_course_branch.c -- race runtime of Course Edit forks. See gdx_course_branch.h. */

#include "global.h"
#include "fzx_course.h"
#include "fzx_racer.h"
#include "gdx_course_branch.h"
#include "gdx_course_contact.h"
#include "gdx_course_branch_view.h"
#include "gdx_course_settings_view.h"
#include "gdx_course_junction.h"
#include "gdx_net_room_course.h"

extern CourseInfo* gCurrentCourseInfo;
extern bool gInCourseEditor;
extern bool gInCourseEditTestRun;
extern s32 gNumPlayers;
extern s32 gSegmentChunkCount;
extern SegmentChunk gSegmentChunks[];

/* course.c: points gSegmentChunkCount and sLastSegmentChunk at a chunk ring of count chunks. */
extern void Course_GdxSetChunkRing(s32 count);
extern s32 Course_GdxRoadTextureCoords(s32 trackInfo, s16* coords);
extern void gdx_register_host_wide_command_range(void* ptr, size_t size);
/* racer.c: the course file id ("NAME.EXT") of the course about to start; 0 when it has no file. */
extern int gdx_racer_course_file_id(char* out, int outCap);
extern void gdx_dbg_logf(const char* fmt, ...);
extern f32 Course_SplineGetTangent(CourseSegment* segment, f32 t, Vec3f* tangentVec);
extern void Course_SplineGetPosition(CourseSegment* segment, f32 t, Vec3f* pos);

#define BRANCH_SEGMENTS GDX_COURSE_POINT_CAPACITY
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
    s16 gadgetRing2[BRANCH_SEGMENTS];
    s32 gadgetCount;
} sBranch;

static SegmentChunk sRing2Chunks[BRANCH_CHUNKS];
static SegmentChunk sRing1ChunkSave[BRANCH_CHUNKS];
static u8 sRing2ChunkMask[BRANCH_CHUNKS];
static BranchRacer sRacers[TOTAL_RACER_COUNT];

#define BRANCH_JUNCTION_VERTICES 32760
#define BRANCH_JUNCTION_LISTS 7600
#define BRANCH_JUNCTION_CUTTERS GDX_JUNCTION_MAX_CUTTERS

static Vtx sJunctionVertices[BRANCH_JUNCTION_VERTICES];
static Gfx sJunctionLists[BRANCH_JUNCTION_LISTS];
static s32 sJunctionListOffset[2][BRANCH_CHUNKS];
static u8 sJunctionDetail[2][BRANCH_CHUNKS];
static GdxJunctionWorkspace sJunctionWorkspace;
static GdxJunctionTriangle sJunctionPieces[GDX_JUNCTION_MAX_PIECES];
static GdxJunctionCutter sJunctionCutters[BRANCH_JUNCTION_CUTTERS];
static GdxJunctionCutter sJunctionRailCutters[BRANCH_JUNCTION_CUTTERS / 4];
static struct {
    double min[3];
    double max[3];
    s32 fork;
} sJunctionOwners[BRANCH_CHUNKS];
static s32 sJunctionVertexCount;
static s32 sJunctionListCount;

const u8* gGdxCourseChunkDrawMask = NULL;

/* Control point of race segment 0, -1 when no rotation matched. */
static s32 sSegmentRotation;
static s32 sSegmentCount;
/* Ring 2's point count while its chunks are built, else 0. */
static s32 sRing2BuildCount;

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
    sBranch.gadgetCount = 0;
    sBranch.courseInfo = NULL;
    sBranch.ring1 = NULL;
    sBranch.ring1Count = 0;
    sBranch.forkCount = 0;
    sBranch.ring2Count = 0;
    sBranch.ring2ChunkCount = 0;
    sBranch.ring2SpanChunks = 0;
    sJunctionVertexCount = 0;
    sJunctionListCount = 0;
    memset(sJunctionListOffset, -1, sizeof(sJunctionListOffset));
    gGdxCourseChunkDrawMask = NULL;
    gdx_course_branch_reset_racers();
}

static s32 branch_junction_fork(const SegmentChunk* chunk, const s8* forkOf, s32 count) {
    s32 shape = chunk->trackSegmentInfo & TRACK_SHAPE_MASK;
    s32 index = chunk->segmentIndex;
    if ((shape != TRACK_SHAPE_ROAD && shape != TRACK_SHAPE_BORDERLESS_ROAD) ||
        (chunk->trackSegmentInfo & TRACK_TYPE_MASK) == TRACK_TYPE_NONE || index < 0 || index >= count) {
        return -1;
    }
    return forkOf[index];
}

static void branch_junction_vertices(const SegmentChunk* chunk, s32 corrected,
                                     const s16* coords, GdxJunctionVertex* out) {
    const s16* positions[8] = {chunk->referencePos1, chunk->referencePos6, chunk->referencePos7,
                              chunk->referencePos8, chunk->referencePos2, chunk->referencePos5,
                              chunk->referencePos4, chunk->referencePos3};
    static const s32 sIndex[8] = {4, 3, 2, 1, 0, 1, 2, 3};
    s32 left = corrected ? chunk->leftTextureCorrection : chunk->leftTextureCoord;
    s32 right = corrected ? chunk->rightTextureCorrection : chunk->rightTextureCoord;
    s32 i, j;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 3; j++) {
            out[i].pos[j] = positions[i][j];
        }
        out[i].uv[0] = coords[sIndex[i]];
        out[i].uv[1] = (i == 0 || i == 4) ? (left + right) >> 1 : (i < 4 ? right : left);
    }
}

static void branch_junction_triangles(const SegmentChunk* chunk, const SegmentChunk* next,
                                      GdxJunctionTriangle* out) {
    GdxJunctionVertex vertices[16];
    s16 coords[5] = {0};
    s32 i;
    Course_GdxRoadTextureCoords(chunk->trackSegmentInfo, coords);
    branch_junction_vertices(chunk, chunk->trackSegmentInfo & TRACK_FLAG_80000000, coords, vertices);
    branch_junction_vertices(next, 0, coords, vertices + 8);
    for (i = 0; i < 8; i++) {
        s32 k = (i + 2) % 8;
        s32 n = (k + 1) % 8;
        out[i * 2].v[0] = vertices[k];
        out[i * 2].v[1] = vertices[k + 8];
        out[i * 2].v[2] = vertices[n + 8];
        out[i * 2 + 1].v[0] = vertices[k];
        out[i * 2 + 1].v[1] = vertices[n + 8];
        out[i * 2 + 1].v[2] = vertices[n];
    }
}

static void branch_junction_bounds(const GdxJunctionTriangle* triangles, double* min, double* max) {
    s32 i, j, k;
    for (k = 0; k < 3; k++) {
        min[k] = max[k] = triangles[0].v[0].pos[k];
        for (i = 0; i < 16; i++) {
            for (j = 0; j < 3; j++) {
                double value = triangles[i].v[j].pos[k];
                if (value < min[k]) {
                    min[k] = value;
                }
                if (value > max[k]) {
                    max[k] = value;
                }
            }
        }
    }
}

static s32 branch_junction_append(const GdxJunctionTriangle* triangles, s32 count) {
    s32 i, j, k;
    if (count > (BRANCH_JUNCTION_VERTICES - sJunctionVertexCount) / 3) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        for (j = 0; j < 3; j++) {
            Vtx* v = &sJunctionVertices[sJunctionVertexCount++];
            memset(v, 0, sizeof(*v));
            for (k = 0; k < 3; k++) {
                v->v.ob[k] = gdx_junction_quantize(triangles[i].v[j].pos[k]);
            }
            v->v.tc[0] = gdx_junction_quantize(triangles[i].v[j].uv[0]);
            v->v.tc[1] = gdx_junction_quantize(triangles[i].v[j].uv[1]);
            v->v.cn[3] = 255;
        }
    }
    return 1;
}

static s32 branch_junction_list(s32 first) {
    s32 i;
    s32 vertices = sJunctionVertexCount - first;
    s32 needed = (vertices + 29) / 30 + (vertices / 3 + 1) / 2 + 1;
    if (needed > BRANCH_JUNCTION_LISTS - sJunctionListCount) {
        return 0;
    }
    for (i = first; i < sJunctionVertexCount; i += 30) {
        s32 count = sJunctionVertexCount - i;
        s32 j;
        if (count > 30) {
            count = 30;
        }
        gSPVertex(&sJunctionLists[sJunctionListCount++], &sJunctionVertices[i], count, 0);
        for (j = 0; j + 5 < count; j += 6) {
            gSP2Triangles(&sJunctionLists[sJunctionListCount++], j, j + 1, j + 2, 0,
                          j + 3, j + 4, j + 5, 0);
        }
        if (j < count) {
            gSP1Triangle(&sJunctionLists[sJunctionListCount++], j, j + 1, j + 2, 0);
        }
    }
    gSPEndDisplayList(&sJunctionLists[sJunctionListCount++]);
    return 1;
}

static s32 branch_junction_road_overlap(const GdxJunctionTriangle* road, const GdxJunctionTriangle* owner) {
    GdxJunctionCutter cutters[4];
    s32 i, count = 0;
    for (i = 2; i < 6; i++) {
        if (gdx_junction_cutter(&owner[i], &cutters[count])) {
            count++;
        }
    }
    for (i = 2; i < 6; i++) {
        s32 pieces = gdx_junction_subtract(&road[i], cutters, count, &sJunctionWorkspace,
                                           sJunctionPieces, GDX_JUNCTION_MAX_PIECES);
        if (pieces < 0) {
            return -1;
        }
        if (pieces != 1 || memcmp(sJunctionPieces, &road[i], sizeof(road[i])) != 0) {
            return 1;
        }
    }
    return 0;
}

static s32 branch_junction_pass(s32 side, s32 mainCount, s32 mainPoints, s32 alternatePoints) {
    SegmentChunk* chunks = side ? sRing2Chunks : gSegmentChunks;
    SegmentChunk* other = side ? gSegmentChunks : sRing2Chunks;
    const s8* forks = side ? sBranch.ring2ForkOf : sBranch.ring1ForkOf;
    const s8* otherForks = side ? sBranch.ring1ForkOf : sBranch.ring2ForkOf;
    s32 count = side ? sBranch.ring2ChunkCount : mainCount;
    s32 otherCount = side ? mainCount : sBranch.ring2ChunkCount;
    s32 points = side ? alternatePoints : mainPoints;
    s32 otherPoints = side ? mainPoints : alternatePoints;
    GdxJunctionTriangle triangles[16], owner[16];
    s32 i, j, k, axis;
    for (i = 0; i < otherCount; i++) {
        sJunctionOwners[i].fork = branch_junction_fork(&other[i], otherForks, otherPoints);
        if (sJunctionOwners[i].fork >= 0) {
            branch_junction_triangles(&other[i], &other[(i + 1) % otherCount], owner);
            branch_junction_bounds(owner, sJunctionOwners[i].min, sJunctionOwners[i].max);
        }
    }
    for (i = 0; i < count; i++) {
        double min[3], max[3];
        s32 candidates[BRANCH_JUNCTION_CUTTERS / 16];
        s32 candidateCount = 0, cutterCount = 0, railCount = 0, changed = 0;
        s32 first = sJunctionVertexCount;
        s32 fork = branch_junction_fork(&chunks[i], forks, points);
        if (fork < 0) {
            continue;
        }
        branch_junction_triangles(&chunks[i], &chunks[(i + 1) % count], triangles);
        branch_junction_bounds(triangles, min, max);
        for (j = 0; j < otherCount; j++) {
            s32 overlap;
            if (sJunctionOwners[j].fork != fork) {
                continue;
            }
            for (axis = 0; axis < 3; axis++) {
                if (min[axis] > sJunctionOwners[j].max[axis] + GDX_JUNCTION_DISTANCE ||
                    max[axis] < sJunctionOwners[j].min[axis] - GDX_JUNCTION_DISTANCE) {
                    break;
                }
            }
            if (axis != 3) {
                continue;
            }
            if (candidateCount == BRANCH_JUNCTION_CUTTERS / 16) {
                return 0;
            }
            candidates[candidateCount++] = j;
            branch_junction_triangles(&other[j], &other[(j + 1) % otherCount], owner);
            if (side) {
                for (k = 0; k < 16; k++) {
                    if (gdx_junction_cutter(&owner[k], &sJunctionCutters[cutterCount])) {
                        cutterCount++;
                    }
                }
            }
            overlap = branch_junction_road_overlap(triangles, owner);
            if (overlap < 0) {
                return 0;
            }
            if (overlap) {
                for (k = 2; k < 6; k++) {
                    // Native road rails rise 10 units and their outer sides descend 40.
                    if (gdx_junction_rail_cutter(&owner[k], 50, &sJunctionRailCutters[railCount])) {
                        railCount++;
                    }
                }
            }
        }
        if (!cutterCount && !railCount) {
            continue;
        }
        for (j = 0; j < 16; j++) {
            s32 pieces;
            s32 road = j >= 2 && j < 6;
            if (!road && railCount) {
                pieces = gdx_junction_subtract(&triangles[j], sJunctionRailCutters, railCount,
                                               &sJunctionWorkspace, sJunctionPieces, GDX_JUNCTION_MAX_PIECES);
            } else {
                pieces = gdx_junction_subtract(&triangles[j], sJunctionCutters, cutterCount,
                                               &sJunctionWorkspace, sJunctionPieces, GDX_JUNCTION_MAX_PIECES);
            }
            if (pieces < 0 || !branch_junction_append(sJunctionPieces, pieces)) {
                return 0;
            }
            changed |= pieces != 1 || memcmp(sJunctionPieces, &triangles[j], sizeof(triangles[j])) != 0;
        }
        if (!changed) {
            sJunctionVertexCount = first;
            continue;
        }
        sJunctionListOffset[side][i] = sJunctionListCount;
        if (!branch_junction_list(first)) {
            return 0;
        }
        sJunctionDetail[side][i] = sJunctionDetail[side][(i + 1) % count] = 1;
        for (j = 0; j < candidateCount; j++) {
            k = candidates[j];
            sJunctionDetail[side ^ 1][k] = sJunctionDetail[side ^ 1][(k + 1) % otherCount] = 1;
        }
    }
    return 1;
}

static s32 branch_junction_build(s32 mainCount, s32 mainPoints, s32 alternatePoints) {
    static s32 registered;
    s32 i;
    if (!registered) {
        gdx_register_host_wide_command_range(sJunctionLists, sizeof(sJunctionLists));
        registered = 1;
    }
    memset(sJunctionDetail, 0, sizeof(sJunctionDetail));
    if (!branch_junction_pass(1, mainCount, mainPoints, alternatePoints) ||
        !branch_junction_pass(0, mainCount, mainPoints, alternatePoints)) {
        return 0;
    }
    // Both rings must use the same fine-chunk boundaries as the retained cuts.
    for (i = 0; i < mainCount; i++) {
        if (sJunctionDetail[0][i]) {
            gSegmentChunks[i].trackSegmentInfo &= ~TRACK_FLAG_CONTINUOUS;
        }
    }
    for (i = 0; i < sBranch.ring2ChunkCount; i++) {
        if (sJunctionDetail[1][i]) {
            sRing2Chunks[i].trackSegmentInfo &= ~TRACK_FLAG_CONTINUOUS;
        }
    }
    gdx_dbg_logf("[fork] junctions: %d/%d vertices, %d/%d commands\n",
                 sJunctionVertexCount, BRANCH_JUNCTION_VERTICES,
                 sJunctionListCount, BRANCH_JUNCTION_LISTS);
    return 1;
}

Gfx* gdx_course_branch_chunk_mesh(const SegmentChunk* chunk, const SegmentChunk* next) {
    s32 index;
    s32 side = gGdxCourseChunkDrawMask != NULL;
    s32 count = side ? sBranch.ring2ChunkCount : sBranch.ring1ChunkCount;
    if (!sBranch.live || chunk < gSegmentChunks || chunk >= gSegmentChunks + count) {
        return NULL;
    }
    index = (s32)(chunk - gSegmentChunks);
    if (sJunctionListOffset[side][index] < 0 || next != &gSegmentChunks[(index + 1) % count]) {
        return NULL;
    }
    return &sJunctionLists[sJunctionListOffset[side][index]];
}

s32 gdx_course_branch_live(void) {
    return sBranch.live && (gCurrentCourseInfo == sBranch.courseInfo) &&
           (sBranch.courseInfo->courseSegments == sBranch.ring1) &&
           (sBranch.courseInfo->segmentCount == sBranch.ring1Count);
}

void gdx_course_branch_minimap(u8* texture, f32 scale, s32 fill) {
    extern void gdx_course_minimap_plot(unsigned char*, float, float, float, int);
    s32 i;

    if (!texture || !gdx_course_branch_live()) {
        return;
    }
    for (i = 0; i < sBranch.ring2Count; i++) {
        CourseSegment* segment = &sBranch.ring2[i];
        f32 t = 0.0f;
        s32 samples = 0;

        if (sBranch.ring2ForkOf[i] < 0) {
            continue;
        }
        while (t < 1.0f && samples++ < 4096) {
            Vec3f position;
            Vec3f tangent;
            f32 magnitude = Course_SplineGetTangent(segment, t, &tangent);

            Course_SplineGetPosition(segment, t, &position);
            gdx_course_minimap_plot(texture, position.x, position.z, scale, fill);
            if (!(magnitude > 0.001f)) {
                break;
            }
            t += 200.0f / magnitude;
        }
    }
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
    sRing2BuildCount = ring2Count;
    buildResult = func_800A1954(&ring2Info);
    sRing2BuildCount = 0;
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
     * that lands there moves back to the stock ring on its next walk. B spans get their own gadget links after this build. Every
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

    if (!branch_junction_build(ring1ChunkCount, count, ring2Count)) {
        sJunctionVertexCount = 0;
        sJunctionListCount = 0;
        memset(sJunctionListOffset, -1, sizeof(sJunctionListOffset));
        gdx_dbg_logf("[fork] junction capacity exceeded; keeping original geometry\n");
    }

    sBranch.courseInfo = info;
    sBranch.ring1 = ring1;
    sBranch.ring1Count = count;
    sBranch.ring1ChunkCount = ring1ChunkCount;
    sBranch.ring2Count = ring2Count;
    sBranch.live = 1;
    for (i = 0; i < ring2Count; i++) {
        if (sBranch.ring2ForkOf[i] >= 0) {
            sBranch.gadgetRing2[sBranch.gadgetCount++] = (s16) i;
        }
    }
    gdx_dbg_logf("[fork] %d fork(s) live: %d stock points, %d on the second ring, %d chunks (%d drawn)\n",
                 sBranch.forkCount, count, ring2Count, sBranch.ring2ChunkCount, sBranch.ring2SpanChunks);
}

static s32 branch_ring2_index(const CourseSegment* segment) {
    u64 offset = (u64) segment - (u64) sBranch.ring2;

    if (!gdx_course_branch_live() || offset >= sizeof(sBranch.ring2) || offset % sizeof(CourseSegment) != 0) {
        return -1;
    }
    return (s32) (offset / sizeof(CourseSegment));
}

s32 gdx_course_branch_gadget_count(s32 courseIndex) {
    if (gdx_course_branch_live() && !gInCourseEditor && &gCourseInfos[courseIndex] == sBranch.courseInfo) {
        return sBranch.ring1Count + sBranch.gadgetCount;
    }
    if (gdx_course_branch_live() && gInCourseEditTestRun && &gCourseInfos[courseIndex] == sBranch.courseInfo) {
        return sBranch.ring1Count + sBranch.gadgetCount;
    }
    return gCourseInfos[courseIndex].segmentCount;
}

CourseSegment* gdx_course_branch_gadget_segment(s32 courseIndex, s32 index) {
    s32 mainCount = gCourseInfos[courseIndex].segmentCount;

    if (index < 0) {
        return NULL;
    }
    if (index < mainCount) {
        return &gCourseInfos[courseIndex].courseSegments[index];
    }
    if (index < gdx_course_branch_gadget_count(courseIndex)) {
        return &sBranch.ring2[sBranch.gadgetRing2[index - mainCount]];
    }
    return NULL;
}

s32 gdx_course_branch_gadget_source(s32 index) {
    if (gdx_course_branch_live() && index >= sBranch.ring1Count &&
        index < sBranch.ring1Count + sBranch.gadgetCount) {
        s32 ringIndex = sBranch.gadgetRing2[index - sBranch.ring1Count];

        return sBranch.forks[sBranch.ring2ForkOf[ringIndex]].ring1Fork;
    }
    return index;
}

s32 gdx_course_branch_gadget_part(s32 index, s32 part, s32 fallback) {
    s32 ringIndex;
    s32 f;
    BranchFork* fork;

    if (!gdx_course_branch_live() || index < sBranch.ring1Count) {
        return fallback;
    }
    index -= sBranch.ring1Count;
    if (index < 0 || index >= sBranch.gadgetCount) {
        return -1;
    }
    ringIndex = sBranch.gadgetRing2[index];
    f = sBranch.ring2ForkOf[ringIndex];
    fork = &sBranch.forks[f];
    return gdx_branch_view_part(fork->setFork, ringIndex - fork->ring2Fork - 1, part, fallback);
}

s32 gdx_course_branch_settings_point(const CourseSegment* segment) {
    s32 index = branch_ring2_index(segment);

    if (index >= 0 && sBranch.ring2ForkOf[index] >= 0) {
        BranchFork* fork = &sBranch.forks[sBranch.ring2ForkOf[index]];

        return gdx_branch_view_settings_point(fork->setFork, index - fork->ring2Fork - 1);
    }
    return gdx_course_branch_control_point(segment->segmentIndex);
}

s32 gdx_course_branch_segment_road_theme(const CourseSegment* segment) {
    u64 offset = (u64) segment - (u64) sBranch.ring2;
    s32 index;
    s32 f;

    if ((sRing2BuildCount <= 0) || (offset >= sizeof(sBranch.ring2)) || (offset % sizeof(CourseSegment) != 0)) {
        return gdx_settings_view_road_theme(gdx_course_branch_settings_point(segment));
    }
    /* The chunk build of ring 2: the ring is not live yet and its segments still carry ring 2 indices. */
    index = (s32) (offset / sizeof(CourseSegment));
    if (index >= sRing2BuildCount) {
        return 0;
    }
    if (sBranch.ring2Source[index] >= 0) {
        /* A stock point. The fork point is one, so path B's first segment wears the fork point's theme. */
        return gdx_settings_view_road_theme(gdx_course_branch_control_point(sBranch.ring2Source[index]));
    }
    for (f = 0; f < sBranch.forkCount; f++) {
        const BranchFork* fork = &sBranch.forks[f];

        if ((index > fork->ring2Fork) && (index < fork->ring2Merge)) {
            return gdx_settings_view_road_theme(
                gdx_branch_view_settings_point(fork->setFork, index - fork->ring2Fork - 1));
        }
    }
    return 0;
}

f32 gdx_course_branch_gadget_next_t(const CourseSegment* segment, f32 t) {
    s32 index = branch_ring2_index(segment);
    s32 i;
    s32 found = 0;
    SegmentChunk* chunks = index >= 0 ? sRing2Chunks : gSegmentChunks;
    s32 count = index >= 0 ? sBranch.ring2ChunkCount : gSegmentChunkCount;

    if (index < 0) {
        index = segment->segmentIndex;
    }
    for (i = 0; i < count; i++) {
        if (chunks[i].segmentIndex == index) {
            found = 1;
            if (chunks[i].segmentTValue > t) {
                return chunks[i].segmentTValue;
            }
        }
    }
    return !found && t <= 0.00001f ? 0.0001f : 1.0f;
}

s32 gdx_course_branch_gadget_chunk(const CourseSegment* segment, f32 t) {
    s32 index = branch_ring2_index(segment);
    SegmentChunk* chunks = index >= 0 ? sRing2Chunks : gSegmentChunks;
    s32 count = index >= 0 ? sBranch.ring2ChunkCount : gSegmentChunkCount;
    s32 found = 0;
    s32 i;

    if (index < 0) {
        index = segment->segmentIndex;
    }
    for (i = 0; i < count; i++) {
        if (chunks[i].segmentIndex == index && chunks[i].segmentTValue <= t) {
            found = i;
        }
    }
    return branch_ring2_index(segment) >= 0 ? BRANCH_CHUNKS + found : found;
}

s32 gdx_course_branch_gadget_chunk_visible(s32 encoded) {
    if (encoded >= BRANCH_CHUNKS) {
        encoded -= BRANCH_CHUNKS;
        return encoded < sBranch.ring2ChunkCount && sRing2Chunks[encoded].drawState != 0;
    }
    return encoded >= 0 && encoded < gSegmentChunkCount && gSegmentChunks[encoded].drawState != 0;
}

s32 gdx_course_branch_gadget_adjacent(const CourseSegment* previous, const CourseSegment* current) {
    s32 nextIndex;

    if (previous->next == current) {
        return 1;
    }
    nextIndex = branch_ring2_index(previous->next);
    return nextIndex >= 0 && sBranch.ring2ForkOf[nextIndex] < 0 &&
           &sBranch.ring1[sBranch.ring2Source[nextIndex]] == current;
}

SegmentChunk* gdx_course_branch_gadget_load_chunk(const CourseSegment* segment, const Vec3f* pos) {
    s32 alternate = branch_ring2_index(segment) >= 0;
    SegmentChunk* chunks = alternate ? sRing2Chunks : gSegmentChunks;
    s32 count = alternate ? sBranch.ring2ChunkCount : gSegmentChunkCount;
    SegmentChunk* closest = chunks;
    f32 distance = 1.0e30f;
    s32 i;

    for (i = 0; i < count; i++) {
        f32 x = chunks[i].pos.x - pos->x;
        f32 y = chunks[i].pos.y - pos->y;
        f32 z = chunks[i].pos.z - pos->z;
        f32 candidate = x * x + y * y + z * z;

        if (candidate < distance) {
            closest = &chunks[i];
            distance = candidate;
        }
    }
    return closest;
}

void gdx_course_branch_sync_gadgets(void) {
    s32 i;

    if (!gdx_course_branch_live()) {
        return;
    }
    for (i = 0; i < sBranch.ring2Count; i++) {
        if (sBranch.ring2ForkOf[i] < 0) {
            CourseSegment* segment = &sBranch.ring2[i];
            CourseSegment* source = &sBranch.ring1[sBranch.ring2Source[i]];

            segment->jumpsStart = source->jumpsStart;
            segment->jumpsEnd = source->jumpsEnd;
            segment->landminesStart = source->landminesStart;
            segment->landminesEnd = source->landminesEnd;
            segment->effectsStart = source->effectsStart;
            segment->effectsEnd = source->effectsEnd;
        }
    }
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

        /* A G-NET room's custom track brings its own settings, decoded from the room's copy. */
        if (gdx_net_room_course_active()) {
            gdx_net_room_course_apply_settings(count);
        } else {
            gdx_settings_view_select_course(gdx_racer_course_file_id(courseId, sizeof(courseId)) ? courseId : NULL,
                                            count);
        }
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

        /* A G-NET room's custom track brings its own forks, decoded from the room's copy. */
        if (gdx_net_room_course_active()) {
            gdx_net_room_course_apply_forks();
        } else {
            gdx_branch_view_select_course(gdx_racer_course_file_id(courseId, sizeof(courseId)) ? courseId : NULL);
        }
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
    return gdx_course_contact_project(out, info, segment, BRANCH_REWALK_STEPS);
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

    // Unchanged chunks retain the native frame-pool guard; junctions use their audited retained pool.
    if (!gdx_course_branch_live() || (sBranch.ring2SpanChunks == 0) || (ring1ChunkCount != sBranch.ring1ChunkCount)) {
        return gfx;
    }

    memcpy(sRing1ChunkSave, gSegmentChunks, (ring1ChunkCount + 1) * sizeof(SegmentChunk));
    memcpy(gSegmentChunks, sRing2Chunks, (sBranch.ring2ChunkCount + 1) * sizeof(SegmentChunk));
    Course_GdxSetChunkRing(sBranch.ring2ChunkCount);
    gGdxCourseChunkDrawMask = sRing2ChunkMask;
    gfx = drawRing(gfx, cameraIndex);
    memcpy(sRing2Chunks, gSegmentChunks, (sBranch.ring2ChunkCount + 1) * sizeof(SegmentChunk));
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
