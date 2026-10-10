/* port/gdx_rom_courses.h -- read course data out of an F-Zero X ROM and convert it into
 * Content Library track payloads (W2).
 *
 * A ROM course and an Expansion Kit Course Edit track are the SAME structure. CourseData is
 * 0x7E0 (fzx_course.h:14-34) and a CRSD payload is sizeof(CourseContext) = 0xC830, which is
 * CourseData followed by three ghost slots and a records block. So a course lifted out of a ROM
 * becomes a valid track payload by byteswapping it to host order and zeroing the tail.
 *
 * That is what makes FZEP track hacks importable without parsing anything FZEP produces: the
 * community workflow already turns a hack into a patched ROM, and the patched ROM is the only
 * thing this module reads. Nothing here links to, bundles, or understands `.fzep`.
 *
 * The stored checksum survives the conversion untouched. Course_CalculateChecksum's base and
 * EXPANSION_KIT branches (decomp/src/game/course.c:4932) compute an IDENTICAL sum; the EK branch
 * only adds NaN and range rejection on top. Verified against a retail US rev0 ROM on 2026-08-27:
 * all 26 stock courses reproduce their stored checksum under the EK rules and none is
 * geometry-rejected. Conversion therefore never has to recompute and self-certify a checksum --
 * the integrity check that gdx_content_import.c runs stays a real one.
 *
 * The clean ROM the player already supplied (gdx_rom_path, port/rom_buffer.cpp) is the baseline:
 * comparing it against the patched ROM says which courses a hack actually changed, and whether
 * the hack touched anything outside the course table -- which is the signal that it needs the
 * ROM-hack path instead (W3) because only layout-preserving course edits can work here.
 *
 * Read-only by design. Installing goes through the existing validation chain and disk-thread op
 * in gdx_content_import.c, never through here.
 *
 * Design record: devdocs/1.2.0-import-and-limits-scope.md (2026-08-27, W2).
 */
#ifndef GDX_ROM_COURSES_H
#define GDX_ROM_COURSES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Course table geometry, from decomp/assets/yaml/us/rev0/course_data.yaml: 26 entries, one
 * contiguous run, stride sizeof(CourseData). */
#define GDX_ROMCOURSE_COUNT 26
#define GDX_ROMCOURSE_TABLE_OFFSET 0x2AD1E0
#define GDX_ROMCOURSE_STRIDE 0x7E0
#define GDX_ROMCOURSE_TABLE_BYTES (GDX_ROMCOURSE_COUNT * GDX_ROMCOURSE_STRIDE)

/* sizeof(CourseContext), the CRSD payload size gdx_content_io.h:63 declares. */
#define GDX_ROMCOURSE_PAYLOAD_SIZE 0xC830

#define GDX_ROMCOURSE_NAME_LEN 22 /* CourseData.fileName, fzx_course.h:22 */

/* CourseData's own array bound. Not a tunable: ControlPoint[64] plus ten parallel [64] arrays are
 * what make sizeof(CourseData) 0x7E0, which is in turn the on-disk MFS file size. FZEP enforces
 * the same 4..64 range, so no ROM course can exceed it either. */
#define GDX_ROMCOURSE_MAX_CONTROL_POINTS 64

#define GDX_ROMCOURSE_OK 0
#define GDX_ROMCOURSE_ERR_BAD_ARGS (-1)
#define GDX_ROMCOURSE_ERR_IO (-2)          /* open/read/allocate failed */
#define GDX_ROMCOURSE_ERR_NOT_ROM (-3)     /* not an N64 ROM, or too small to hold the table */
#define GDX_ROMCOURSE_ERR_NO_COURSES (-4)  /* the course table does not parse as CourseData */
#define GDX_ROMCOURSE_ERR_NO_BASELINE (-5) /* the clean ROM is missing or unreadable */

typedef struct GdxRomCourseEntry {
    int index;                             /* 0..GDX_ROMCOURSE_COUNT-1, also the payload id */
    char name[GDX_ROMCOURSE_NAME_LEN + 1]; /* CourseData.fileName, cut at its NUL */
    const char* symbol;                    /* stock slot name, e.g. "aCourseRainbowRoad" */
    int32_t controlPointCount;
    int32_t venue;
    int32_t skybox;
    int32_t bgm;
    uint32_t checksum; /* the course's own stored checksum, host order */
    int changed;       /* nonzero when this slot differs from the clean ROM */
} GdxRomCourseEntry;

typedef struct GdxRomCourseSet GdxRomCourseSet; /* opaque */

/* Opens a (possibly patched) ROM and, when cleanPath is non-NULL, the player's clean ROM as the
 * comparison baseline. Byte order is normalised on load, so .z64, .v64 and .n64 are all accepted.
 * Passing cleanPath == NULL still lists every course, with `changed` reported as 0 throughout. */
int gdx_romcourse_open(const char* patchedPath, const char* cleanPath, GdxRomCourseSet** out);
void gdx_romcourse_close(GdxRomCourseSet* set);

const char* gdx_romcourse_strerror(int err);

/* Lists course slots. Pass out == NULL to query the count. When changedOnly is nonzero only the
 * slots that differ from the baseline are reported, which is the useful view for a hack. Returns
 * the entry count or a negative GDX_ROMCOURSE_ERR_*. */
int gdx_romcourse_list(GdxRomCourseSet* set, GdxRomCourseEntry* out, int capacity, int changedOnly);

/* Writes the GDX_ROMCOURSE_PAYLOAD_SIZE-byte CourseContext for a slot: the course byteswapped to
 * host order, followed by a zeroed ghost/records tail. Feed the result to
 * gdx_content_import_validate_payload / gdx_content_import_begin_payload. */
int gdx_romcourse_payload(GdxRomCourseSet* set, int index, uint8_t* buf, int32_t bufSize);

/* Nonzero when the patched ROM differs from the baseline anywhere OUTSIDE the course table, i.e.
 * the hack changes more than courses and belongs on the ROM-hack path. Always 0 when no baseline
 * was supplied, since nothing can be concluded without one.
 *
 * The two N64 header CRC words at 0x10 are deliberately NOT counted: FZEP's "Recalculate Header
 * Checksum" rewrites them so a patched ROM still boots, and treating that as content would flag
 * ordinary course-only hacks. Ask gdx_romcourse_header_checksum_changed() for that separately.
 * Every other header byte still counts. */
int gdx_romcourse_touches_non_course(const GdxRomCourseSet* set);

/* Nonzero when only the ROM header's CRC words differ from the baseline. Informational: it means
 * the ROM was made bootable after patching, which is normal and affects no content. */
int gdx_romcourse_header_checksum_changed(const GdxRomCourseSet* set);

/* How many slots differ from the baseline. 0 when no baseline was supplied. */
int gdx_romcourse_changed_count(const GdxRomCourseSet* set);

/* Nonzero when a baseline ROM was supplied and read successfully. */
int gdx_romcourse_has_baseline(const GdxRomCourseSet* set);

#ifdef __cplusplus
}
#endif

#endif /* GDX_ROM_COURSES_H */
