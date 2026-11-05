// port/gdx_course_splits.h: per-lap and per-node split times for one course. Pure data: no file IO, no RNG, no
// networking. The stored byte layout is described below.
//
// A split record holds the race time at the end of each lap, and at each checkpoint node inside a lap. A staff run
// fills one in (GXM1 tag 5, gdx_course_meta.h). CPU pacing reads it to follow the creator's run.
//
// Node index meaning: node 0 is the first node after the start line. The start line has no node of its own. Node 1
// is the next checkpoint after node 0, and so on. A lap begins at the previous lap end, or at time 0 for lap 0. In
// the game a node is a control point: node n is where the lap distance reaches stock segment n + 1
// (gdx_course_splits_view.c).
//
// Stored form (little-endian, no checksum):
//
//   0x00  laps        u8, 0 for the empty record or 1..GDX_SPLITS_LAPS_MAX
//   0x01  nodes       u8, 0 or 1..GDX_SPLITS_NODES_MAX
//   0x02  lapEnd      laps x u32, absolute lap end in milliseconds
//   then  node times  laps x nodes x u16, each one a delta
//
// A node delta is the gap from the node before it in the same lap. For node 0 that node is the lap start: the
// previous lap end, or time 0 for lap 0. Every delta must fit in 1..65535. The codec has no checksum, so a caller
// that stores the bytes must add its own check.
//
// Pacing: gdx_splits_pace_laps turns a share of a race window into the distance the staff run had covered by
// then. Between nodes the run is taken as steady. A model with no nodes paces per lap. A lap past the recorded
// laps repeats the duration and the node offsets of the last recorded lap, starting at the previous lap end.

#ifndef GDX_COURSE_SPLITS_H
#define GDX_COURSE_SPLITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_SPLITS_LAPS_MAX 9
// Course point budget: GDX_BRANCH_MAX_TOTAL_POINTS in gdx_course_branch_format.h. Repeated here so this header needs
// no course include.
#define GDX_SPLITS_NODES_MAX 127
// Largest race time in milliseconds: one hour minus 2 ms.
#define GDX_SPLITS_TIME_MAX (60u * 60u * 1000u - 2u)
// Largest stored record: 2 header bytes, the lap ends, then every node delta.
#define GDX_SPLITS_VALUE_MAX_BYTES (2 + GDX_SPLITS_LAPS_MAX * 4 + GDX_SPLITS_LAPS_MAX * GDX_SPLITS_NODES_MAX * 2)

#define GDX_SPLITS_ERR_ARGS (-1)        // NULL pointer
#define GDX_SPLITS_ERR_FULL (-2)        // output buffer too small
#define GDX_SPLITS_ERR_VALUE (-3)       // a value breaks its rules
#define GDX_SPLITS_ERR_RANGE (-6)       // a node delta is outside 1..65535
#define GDX_SPLITS_ERR_TRUNCATED (-20)  // shorter than the 2-byte header
#define GDX_SPLITS_ERR_LAYOUT (-25)     // length or counts do not match the header

// Entries past laps or nodes must stay zero. The empty model is all zero.
typedef struct GdxCourseSplits {
    int laps;
    int nodes;
    uint32_t lapEnd[GDX_SPLITS_LAPS_MAX];
    uint32_t node[GDX_SPLITS_LAPS_MAX][GDX_SPLITS_NODES_MAX];
} GdxCourseSplits;

// Builds a model while a run is in progress. A repeated or lower node index is ignored. A skipped node, a time that
// does not move forward or passes GDX_SPLITS_TIME_MAX, a short lap, or a tenth lap sets failed. Every later call is
// then ignored.
typedef struct GdxSplitsRecorder {
    GdxCourseSplits s;  // laps closed so far
    int lap;            // index of the lap in progress
    int nextNode;       // node index the next record must use
    int failed;         // non-zero once the run breaks a rule
} GdxSplitsRecorder;

// Returns 0 when the model obeys every rule. Returns VALUE when a rule breaks, and ARGS for NULL.
int gdx_splits_validate(const GdxCourseSplits* s);

// Writes the stored form and returns its length. Node deltas are checked first, so a zero delta returns RANGE. The
// model must then pass validate (VALUE), and cap must hold the record (FULL). Returns ARGS for NULL. out is not
// touched on failure.
int gdx_splits_encode(const GdxCourseSplits* s, unsigned char* out, size_t cap);

// Reads exactly `length` bytes into out and returns 0. On failure out is the empty model, and the code is TRUNCATED
// for fewer than 2 bytes, LAYOUT for a length or count mismatch, or VALUE for times that break validate.
int gdx_splits_decode(const unsigned char* in, size_t length, GdxCourseSplits* out);

// Starts a run with `nodes` checkpoints per lap, from 0 to GDX_SPLITS_NODES_MAX. Use 0 for lap times only. Any other
// count fails the run. A NULL r is ignored.
void gdx_splits_rec_begin(GdxSplitsRecorder* r, int nodes);

// Records node `node` of the lap in progress at race time `raceTime`, in milliseconds. A repeated or lower node index
// is ignored. An index outside the run, a skipped node, or a time that does not move forward fails the run.
void gdx_splits_rec_node(GdxSplitsRecorder* r, int node, uint32_t raceTime);

// Records every node of the lap in progress that the racer passed between two frames: from position prevAt at
// prevTime to position at at raceTime. Positions are shares of the lap in progress, and may run below 0 or past 1
// so a frame that crosses the line can be split in two calls. Node times are interpolated between the two frames
// and rounded to the millisecond, then go through gdx_splits_rec_node. A frame that does not move forward records
// nothing. nodeAt holds the r->s.nodes node positions, in (0, 1) and strictly increasing. A NULL r or nodeAt is
// ignored.
void gdx_splits_rec_track(GdxSplitsRecorder* r, const float* nodeAt, double prevAt, uint32_t prevTime, double at,
                          uint32_t raceTime);

// Closes the lap in progress at race time `raceTime`, in milliseconds. Fails the run when a node is missing, the time
// does not move forward, or the time passes GDX_SPLITS_TIME_MAX.
void gdx_splits_rec_lap(GdxSplitsRecorder* r, uint32_t raceTime);

// Copies the closed laps to out. A lap still open is left out. Returns 0 when the run has no failure and at least one
// lap closed. Returns VALUE when the run failed, LAYOUT when no lap closed, and ARGS for a NULL pointer. out is the
// empty model on every failure.
int gdx_splits_rec_finish(const GdxSplitsRecorder* r, GdxCourseSplits* out);

// The pace of the staff run over a race of raceLaps laps (1..255). The window runs from the end of lap fromLap
// (0..raceLaps - 1, 0 for the start) to the finish: share 0 is that lap end and share 1 the finish, in staff time.
// Writes to outLaps how far the staff run had come at that share, in laps from the start line, so 1.5 is halfway
// round the second lap. nodeAt gives the node positions as for gdx_splits_rec_track, and may be NULL for a model with
// no nodes. Outside 0..1 the share runs on at the window's average pace. Returns 0, ARGS for a NULL s or outLaps,
// or VALUE for an invalid or empty model, bad node positions, laps out of range or a share that is not finite.
int gdx_splits_pace_laps(const GdxCourseSplits* s, const float* nodeAt, int raceLaps, int fromLap, double share,
                         double* outLaps);

#ifdef __cplusplus
}
#endif

#endif
