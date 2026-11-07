/* port/gdx_course_settings.h -- per-track and per-point settings, GXK1 block, GKS1 state file, .gdxb sidecar.
 *
 * Course Edit keeps values the stock course file has no room for: how wide a pit, dirt, ice or dash part is, how
 * hard and how far one magnet pulls, what kind of dash pad a point has, the wind on a segment, and values for the
 * whole track (does lava kill, magnet strength, magnet range, how often CPUs attack). 0 always means "not set", so
 * an old course behaves as it always did.
 *
 * Width is a percent of the road, 1..100, where 100 covers the whole road. Strength is stored as
 * round(value * 80), which fits the 0.25..3.0 menu range in a byte. Range is stored as is, 10..200.
 *
 * A dash pad is GDX_SETTINGS_PAD_NORMAL, _SUPER or _BRAKE. padBoost is a Super pad's speed floor above the stock
 * dash, in percent minus 100: 10..200 is 110% to 300%, and 0 is GDX_SETTINGS_PAD_BOOST_DEFAULT. It is only set on
 * a Super pad, so one set of values has one encoding.
 *
 * Wind (port/gdx_wind.h) is per segment only: a point's strength in percent, 1..100, and the compass angle
 * 0..359 it blows toward, on the segment that starts there. A strength of 0 is no wind, and its angle is 0.
 *
 * CPU aggression is a preset for the whole track, GDX_SETTINGS_AI_*: Track (0, stock), Calm, Normal, Fierce or
 * Brutal. Track keeps the player's AI aggression option. Calm, Fierce and Brutal multiply it, and Normal replaces
 * it with 100%. Either way it scales how often CPUs side attack, spin attack and swerve into a machine behind
 * them, on top of the difficulty. gdx_settings_ai_resolve gives the final scale.
 *
 * GXK1 block (little-endian), written by hand like GXB1:
 *
 *   0x00  magic           "GXK1"
 *   0x04  version         u16 = 1, 2 when any point has a dash pad value, 3 when there is any wind, 4 when the
 *                         track has a CPU aggression preset
 *   0x06  ringCount       u8, the ring the point entries belong to
 *   0x07  lavaMode        u8, GDX_SETTINGS_LAVA_*
 *   0x08  magnetStrength  u8, 0 or 20..240
 *   0x09  magnetRange     u8, 0 or 10..200
 *   0x0A  entryCount      u16
 *   0x0C  bindCrc         u32, CRC-32 of the bytes this block is bound to
 *   0x10  version 3: 4 spare bytes, written 0 and ignored when read, so the entries start at 0x14. Test
 *         builds kept a wind for the whole track there, and calm points (strength 255) in the entries; both
 *         load as no wind, and an entry left with nothing else is dropped.
 *   0x10  version 4: u8 aiAggression, 1..4, then 3 spare bytes written 0 and ignored. Entries as version 3.
 *   0x10  entries         entryCount x { u8 kind, u8 index, u8 pitWidth, dirtWidth, iceWidth,
 *                                        magnetStrength, magnetRange }
 *                         version 2 adds u8 padKind, padWidth, padBoost to every entry
 *                         version 3 and 4 add those and u8 windStrength, u16 windAngle
 *   end-4 crc32           u32, CRC-32 over every byte before it
 *
 * kind 0 is a ring point. kind 1..4 is a node of fork kind-1. Entries are sparse, sorted by kind then index,
 * and never all zero, so one set of values has one encoding. A block is written with the lowest version that
 * holds its values, so builds without pads or wind still read a course that does not use them.
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
#define GDX_SETTINGS_ENTRY_BYTES 7     /* version 1 */
#define GDX_SETTINGS_ENTRY_BYTES_V2 10 /* version 2, with the dash pad values */
#define GDX_SETTINGS_ENTRY_BYTES_V3 13 /* version 3, with the wind */
#define GDX_SETTINGS_MAX_ENTRIES (GDX_SETTINGS_MAX_POINTS + GDX_BRANCH_MAX_FORKS * GDX_BRANCH_MAX_NODES)
#define GDX_SETTINGS_BLOCK_HEADER_SIZE 16
#define GDX_SETTINGS_BLOCK_HEADER_SIZE_V3 20
#define GDX_SETTINGS_BLOCK_MAX_BYTES \
    (GDX_SETTINGS_BLOCK_HEADER_SIZE_V3 + GDX_SETTINGS_MAX_ENTRIES * GDX_SETTINGS_ENTRY_BYTES_V3 + 4)

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

#define GDX_SETTINGS_PAD_NORMAL 0
#define GDX_SETTINGS_PAD_SUPER 1
#define GDX_SETTINGS_PAD_BRAKE 2
#define GDX_SETTINGS_PAD_BOOST_MIN 10
#define GDX_SETTINGS_PAD_BOOST_MAX 200
/* The boost of a Super pad with no boost of its own: 150% of the stock dash. */
#define GDX_SETTINGS_PAD_BOOST_DEFAULT 50

#define GDX_SETTINGS_WIND_MAX 100
#define GDX_SETTINGS_WIND_ANGLE_MAX 359

#define GDX_SETTINGS_AI_TRACK 0 /* not set: stock combat */
#define GDX_SETTINGS_AI_CALM 1
#define GDX_SETTINGS_AI_NORMAL 2
#define GDX_SETTINGS_AI_FIERCE 3
#define GDX_SETTINGS_AI_BRUTAL 4

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
    uint8_t aiAggression; /* GDX_SETTINGS_AI_* */
    GdxPointExtras ring[GDX_SETTINGS_MAX_POINTS];
    GdxPointExtras nodes[GDX_BRANCH_MAX_FORKS][GDX_BRANCH_MAX_NODES];
} GdxCourseSettings;

/* 1 when nothing is set. ringCount does not count: it only says what the point entries belong to. */
int gdx_settings_is_empty(const GdxCourseSettings* settings);

/* Returns GDX_SETTINGS_OK when every value is 0 or in its range and no entry sits past ringCount. */
int gdx_settings_validate(const GdxCourseSettings* settings);

/* The multiplier a GDX_SETTINGS_AI_* preset puts on the AI aggression option: Calm 0.5, Fierce 1.5, Brutal 2. Track
 * and Normal give 1 here, but Normal replaces the option rather than multiplying it, so use gdx_settings_ai_resolve
 * for the race. An unknown value gives 1. */
float gdx_settings_ai_scale(int aiAggression);

/* The AI aggression scale for a preset, given the player's option as a scale (the option as a fraction of 100).
 * Track keeps optionScale, Normal is 1 whatever it is, and Calm, Fierce and Brutal multiply it by 0.5, 1.5 and 2.
 * An unknown value keeps optionScale. */
float gdx_settings_ai_resolve(int aiAggression, float optionScale);

/* GXK1 codec. length is the exact block size. decode zeroes out on every failure. */
int gdx_settings_encode(const GdxCourseSettings* settings, uint32_t bindCrc, unsigned char* out, size_t cap);
int gdx_settings_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseSettings* out);

/* Copies a valid block under another binding, changing only its binding and CRC, and returns its length. A block
 * from an older build can decode to values that encode differently, so decoding and encoding again would change
 * bytes a snapshot seal covered. */
int gdx_settings_rebind(const unsigned char* data, size_t length, uint32_t oldBindCrc, uint32_t newBindCrc,
                        unsigned char* out, size_t cap);

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
