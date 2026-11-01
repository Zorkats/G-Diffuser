/* port/gdx_course_settings.h -- per-track and per-point settings, GXK1 block, GKS1 state file, .gdxb sidecar.
 *
 * Course Edit keeps values the stock course file has no room for: how wide a pit, dirt or ice part is, how
 * hard and how far one magnet pulls, and three values for the whole track (does lava kill, magnet strength,
 * magnet range). 0 always means "not set", so an old course behaves as it always did.
 *
 * Width is a percent of the road, 1..100, where 100 covers the whole road. Strength is stored as
 * round(value * 80), which fits the 0.25..3.0 menu range in a byte. Range is stored as is, 10..200.
 *
 * GXK1 block (little-endian), written by hand like GXB1:
 *
 *   0x00  magic           "GXK1"
 *   0x04  version         u16 = 1
 *   0x06  ringCount       u8, the ring the point entries belong to
 *   0x07  lavaMode        u8, GDX_SETTINGS_LAVA_*
 *   0x08  magnetStrength  u8, 0 or 20..240
 *   0x09  magnetRange     u8, 0 or 10..200
 *   0x0A  entryCount      u16
 *   0x0C  bindCrc         u32, CRC-32 of the bytes this block is bound to
 *   0x10  entries         entryCount x { u8 kind, u8 index, u8 pitWidth, dirtWidth, iceWidth,
 *                                        magnetStrength, magnetRange }
 *   end-4 crc32           u32, CRC-32 over every byte before it
 *
 * kind 0 is a ring point. kind 1..4 is a node of fork kind-1. Entries are sparse, sorted by kind then index,
 * and never all zero, so one set of values has one encoding.
 *
 * GKS1 state file: same shape as GBS1 (gdx_course_branch_format.h), blocks bound to the CRC-32 of the course id.
 *
 * GXD1 sidecar, the .gdxb file next to a .gdxc in an export:
 *
 *   0x00  magic       "GXD1"
 *   0x04  version     u16 = 1
 *   0x06  entryCount  u16, 1..GDX_SIDECAR_MAX_ENTRIES
 *   0x08  fileLength  u32, every byte of the file including the trailing CRC
 *   0x0C  entries     entryCount x { u8 idLen, courseId[idLen], u32 bindCrc,
 *                                    u16 forkLen, GXB1 block[forkLen], u16 settingsLen, GXK1 block[settingsLen] }
 *   end-4 crc32       u32, CRC-32 over every byte before it
 *
 * A length of 0 means that entry has no such block. Both blocks of an entry are bound to bindCrc, the CRC-32
 * of the course bytes they belong to, so they cannot land on another course.
 */

#ifndef GDX_COURSE_SETTINGS_H
#define GDX_COURSE_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "gdx_course_branch_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_SETTINGS_MAX_POINTS GDX_BRANCH_MAX_TOTAL_POINTS
#define GDX_SETTINGS_ENTRY_BYTES 7
#define GDX_SETTINGS_MAX_ENTRIES (GDX_SETTINGS_MAX_POINTS + GDX_BRANCH_MAX_FORKS * GDX_BRANCH_MAX_NODES)
#define GDX_SETTINGS_BLOCK_HEADER_SIZE 16
#define GDX_SETTINGS_BLOCK_MAX_BYTES \
    (GDX_SETTINGS_BLOCK_HEADER_SIZE + GDX_SETTINGS_MAX_ENTRIES * GDX_SETTINGS_ENTRY_BYTES + 4)

#define GDX_SETTINGS_FILE_HEADER_SIZE 12
#define GDX_SETTINGS_FILE_ENTRY_MAX (1 + GDX_BRANCH_COURSE_ID_MAX + 2 + GDX_SETTINGS_BLOCK_MAX_BYTES)
#define GDX_SETTINGS_FILE_MAX_BYTES \
    (GDX_SETTINGS_FILE_HEADER_SIZE + GDX_BRANCH_STORE_SLOTS * GDX_SETTINGS_FILE_ENTRY_MAX + 4)

#define GDX_SIDECAR_MAX_ENTRIES 6
#define GDX_SIDECAR_HEADER_SIZE 12
#define GDX_SIDECAR_ENTRY_MAX \
    (1 + GDX_BRANCH_COURSE_ID_MAX + 4 + 2 + GDX_BRANCH_BLOCK_MAX_BYTES + 2 + GDX_SETTINGS_BLOCK_MAX_BYTES)
#define GDX_SIDECAR_MAX_BYTES (GDX_SIDECAR_HEADER_SIZE + GDX_SIDECAR_MAX_ENTRIES * GDX_SIDECAR_ENTRY_MAX + 4)

#define GDX_SETTINGS_LAVA_FOLLOW_MENU 0
#define GDX_SETTINGS_LAVA_KILLS 1
#define GDX_SETTINGS_LAVA_NEVER_KILLS 2

#define GDX_SETTINGS_WIDTH_MAX 100
#define GDX_SETTINGS_STRENGTH_MIN 20
#define GDX_SETTINGS_STRENGTH_MAX 240
#define GDX_SETTINGS_RANGE_MIN 10
#define GDX_SETTINGS_RANGE_MAX 200
/* Stored strength per unit of the menu's strength value. */
#define GDX_SETTINGS_STRENGTH_SCALE 80.0f

#define GDX_SETTINGS_OK 0
#define GDX_SETTINGS_ERR_ARGS (-1)      /* NULL pointer or otherwise unusable argument */
#define GDX_SETTINGS_ERR_FULL (-2)      /* buffer or store is full */
#define GDX_SETTINGS_ERR_VALUE (-3)     /* a value is out of its range */
#define GDX_SETTINGS_ERR_TRUNCATED (-20) /* shorter than the minimum */
#define GDX_SETTINGS_ERR_MAGIC (-21)    /* wrong magic */
#define GDX_SETTINGS_ERR_VERSION (-22)  /* unknown version */
#define GDX_SETTINGS_ERR_CRC (-23)      /* trailing CRC-32 does not match the bytes */
#define GDX_SETTINGS_ERR_BINDING (-24)  /* block is bound to a different CRC-32 */
#define GDX_SETTINGS_ERR_LAYOUT (-25)   /* sizes, order or ids do not add up */

typedef struct GdxCourseSettings {
    uint8_t ringCount;
    uint8_t lavaMode;
    uint8_t magnetStrength;
    uint8_t magnetRange;
    GdxPointExtras ring[GDX_SETTINGS_MAX_POINTS];
    GdxPointExtras nodes[GDX_BRANCH_MAX_FORKS][GDX_BRANCH_MAX_NODES];
} GdxCourseSettings;

/* 1 when nothing is set. ringCount does not count: it only says what the point entries belong to. */
int gdx_settings_is_empty(const GdxCourseSettings* settings);

/* Returns GDX_SETTINGS_OK when every value is 0 or in its range and no entry sits past ringCount. */
int gdx_settings_validate(const GdxCourseSettings* settings);

/* GXK1 codec. length is the exact block size. decode zeroes out on every failure. */
int gdx_settings_encode(const GdxCourseSettings* settings, uint32_t bindCrc, unsigned char* out, size_t cap);
int gdx_settings_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseSettings* out);

/* Store keyed by course id, same rules as gdx_branch_store_*: an empty value removes the entry, and a rename
 * from an id with nothing clears the new id. */
int gdx_settings_store_set(const char* courseId, const GdxCourseSettings* settings);
int gdx_settings_store_get(const char* courseId, GdxCourseSettings* out); /* 1 if found, else 0 */
int gdx_settings_store_rename(const char* oldId, const char* newId);
void gdx_settings_store_clear(void);
unsigned long gdx_settings_store_revision(void);
int gdx_settings_store_encode_file(unsigned char* out, size_t cap);
int gdx_settings_store_restore_file(const unsigned char* data, size_t length);

/* Values of the course being raced or edited. Game thread only. NULL clears. */
const GdxCourseSettings* gdx_settings_active(void);
void gdx_settings_set_active(const GdxCourseSettings* settings);

/* .gdxb sidecar. A NULL or empty forks or settings pointer writes no block for it. */
typedef struct GdxSidecarEntry {
    const char* courseId;
    uint32_t bindCrc;
    const GdxBranchSet* forks;
    const GdxCourseSettings* settings;
} GdxSidecarEntry;

/* 1 when the entry would write a block. An export with no such entry writes no .gdxb, because older builds
 * refuse a pack that has one. */
int gdx_course_sidecar_entry_has_data(const GdxSidecarEntry* entry);

/* Returns the file length, or a negative code. Entries with no data are skipped; when all are, returns 0. */
int gdx_course_sidecar_encode(const GdxSidecarEntry* entries, int count, unsigned char* out, size_t cap);

/* Checks the whole file first, then calls visit once per entry. forks and settings are NULL for a block the
 * entry does not have. A bad file visits nothing. The pointers are valid during the call only. */
typedef void (*GdxSidecarVisit)(const char* courseId, uint32_t bindCrc, const GdxBranchSet* forks,
                                const GdxCourseSettings* settings, void* ctx);
int gdx_course_sidecar_decode(const unsigned char* data, size_t length, GdxSidecarVisit visit, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_SETTINGS_H */
