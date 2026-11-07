// Copyright (c) Zorkats
//
// Foreign track normalization (F3). Pure C with no game symbols, so the game's import path
// (gdx_content_import.c) and the Companion's read-only Tracks inspector run the same code.
//
// A course payload is 0xC830 bytes. Its first 0x7E0 bytes are CourseData, mirrored below from
// decomp/include/fzx_course.h:7-34. The rest are ghosts and records, which normalize byte-swaps
// but never validates. The checks mirror the game's own load path: course_edit/19C470.c:160-167
// (checksum, creatorId, BGM) with the checksum algorithm at the end of game/course.c, plus bounds
// checks the game lacks because the payload is untrusted.
//
// The highest control point the checksum accepts is a parameter: the game passes
// gdx_course_edit_max_y(), which follows the extended height setting, and the Companion checks
// both GDX_FOREIGN_TRACK_STOCK_MAX_Y and GDX_FOREIGN_TRACK_EXTENDED_MAX_Y to tell the player
// whether a track needs that setting.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_FOREIGN_TRACK_PAYLOAD_SIZE 0xC830
#define GDX_FOREIGN_TRACK_COURSE_SIZE 0x7E0

#define GDX_FOREIGN_TRACK_MIN_Y (-250.0f)
#define GDX_FOREIGN_TRACK_STOCK_MAX_Y (5000.0f)
#define GDX_FOREIGN_TRACK_EXTENDED_MAX_Y (30000.0f)

// CREATOR_NINTENDO (fzx_course.h:103) and BGM_NEW_04 (sfx.h:110).
#define GDX_FOREIGN_CREATOR_NINTENDO 4
#define GDX_FOREIGN_BGM_MAX 13

#define GDX_FOREIGN_HOST_ORDER 0
#define GDX_FOREIGN_BIG_ENDIAN 1

// Error bits. Each equals the GDX_IMPORT_ERR_* bit of the same name; gdx_content_import.c
// checks that at compile time.
#define GDX_FOREIGN_TRACK_ERR_BAD_SIZE (1u << 6)
#define GDX_FOREIGN_TRACK_ERR_CHECKSUM (1u << 8)
#define GDX_FOREIGN_TRACK_ERR_CREATOR (1u << 9)
#define GDX_FOREIGN_TRACK_ERR_BGM (1u << 10)
#define GDX_FOREIGN_TRACK_ERR_GEOMETRY (1u << 11)
#define GDX_FOREIGN_TRACK_ERR_ENCODING (1u << 21)

typedef struct GdxForeignVec3f {
    float x;
    float y;
    float z;
} GdxForeignVec3f;

// Mirrors ControlPoint, fzx_course.h:7-12. Size 0x14.
typedef struct GdxForeignControlPoint {
    GdxForeignVec3f pos;
    int16_t radiusLeft;
    int16_t radiusRight;
    int32_t trackSegmentInfo;
} GdxForeignControlPoint;

// Mirrors CourseData, fzx_course.h:14-34. Size 0x7E0.
typedef struct GdxForeignCourseData {
    uint8_t creatorId;
    int8_t controlPointCount;
    int8_t venue;
    int8_t skybox;
    uint32_t checksum;
    uint8_t flag;
    char fileName[22];
    int8_t bgm;
    GdxForeignControlPoint controlPoint[64];
    int16_t bankAngle[64];
    int8_t pit[64];
    int8_t dash[64];
    int8_t dirt[64];
    int8_t ice[64];
    int8_t jump[64];
    int8_t landmine[64];
    int8_t gate[64];
    int8_t building[64];
    int8_t sign[64];
} GdxForeignCourseData;

typedef struct GdxForeignCourseDiagnostic {
    int present;
    int encoding;
    int rawCreator;
    int rawCount;
    int creator;
    int count;
    uint32_t storedChecksum;
    uint32_t computedChecksum;
    int checksumChecked;
    int sanitizedPits;
} GdxForeignCourseDiagnostic;

// Called once per pit value the repair clamps, with the control point index and the old value.
typedef void (*GdxForeignPitLog)(void* context, int index, int value);

// Mirror of func_i2_800BE9D4 (game/course.c): nonzero when the float bit pattern is NaN, infinite
// or subnormal and nonzero.
int gdx_foreign_track_float_invalid(const float* value);

// Mirror of Course_CalculateChecksum's EXPANSION_KIT branch. The float expression is verbatim, so
// host float semantics give the identical sum. Sets *geometryError and returns 0, as the game bails
// with -1, when a control point leaves the accepted ranges: x and z within +-15000, y within
// GDX_FOREIGN_TRACK_MIN_Y..maxY. The caller keeps controlPointCount within 0..64.
uint32_t gdx_foreign_track_checksum(const GdxForeignCourseData* course, float maxY, int* geometryError);

// Clamps pit values outside -1..10 to -1 (PIT_NONE) in a host-order payload, since the editor
// tables index by pit + 1 into 12 entries, then stores the recomputed checksum when the geometry
// is valid. Returns the number of values clamped. log may be NULL.
int gdx_foreign_track_sanitize_pits(uint8_t* payload, float maxY, GdxForeignPitLog log, void* logContext);

// Copies a raw track payload of size bytes to host, byte-swapping a big-endian (N64 disk) payload
// into host order, then validates its CourseData against maxY. Returns GDX_FOREIGN_TRACK_ERR_*
// bits; zero means host may be installed. Pits are repaired only when the payload passed
// unchanged. host must hold GDX_FOREIGN_TRACK_PAYLOAD_SIZE bytes, may alias raw, and need not be
// aligned. diagnostic and log may be NULL.
uint32_t gdx_foreign_track_normalize(const uint8_t* raw, int32_t size, int encoding, uint8_t* host, float maxY,
                                     GdxForeignCourseDiagnostic* diagnostic, GdxForeignPitLog log,
                                     void* logContext);

#ifdef __cplusplus
}
#endif
