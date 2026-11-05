/* port/gdx_course_meta.h -- creator metadata of a track: author, Edited by, staff time. GXM1 block, GMS1 state
 * file, .gdxm sidecar.
 *
 * Who made a track and how fast its creator drove it. Neither fits in CourseData (0x7E0 bytes, no spare bytes),
 * so they travel beside it like the per-track settings (gdx_course_settings.h).
 *
 * Author lock: the first Course Edit save of a track with no author writes the local creator name as the author.
 * Saves by anyone else keep the author and record them as Edited by. This only stops the editor from rewriting the
 * name. Anyone can still edit the file bytes; signed identity is a separate, later feature.
 *
 * Staff time: the creator's race time and the lap count it was driven over. CPUs on the track pace against it
 * (gdx_meta_cpu_target). A Course Edit save clears it, because the layout it was set on is gone.
 *
 * Splits: the lap and checkpoint times of that same staff run (gdx_course_splits.h). On Expert and Master the CPUs
 * follow the creator's pace through the lap instead of a straight line. They belong to the staff time: setting a
 * new staff time clears them, and a record that does not match the staff time is dropped on read.
 *
 * Made here: a Course Edit save on this install marks the track. Until online profiles give a creator name, the
 * mark is what lets this install offer a Time Attack run as the staff time (gdx_meta_staff_offer). It is local:
 * kept in the GMS1 state file, never written to or read from a .gdxm, so an imported track is never marked.
 *
 * Not part of .gcs snapshots on purpose: the snapshot seal hashes every metadata section into the course checksum
 * (gdx_course_snapshot.cpp), so renaming the author would change the identity of the track.
 *
 * GXM1 block (little-endian), written by hand like GXK1:
 *
 *   0x00  magic         "GXM1"
 *   0x04  version       u16 = 1
 *   0x06  recordCount   u16
 *   0x08  bindCrc       u32, CRC-32 of the bytes this block is bound to
 *   0x0C  records       recordCount x { u8 tag, u16 length, value[length] }, tags strictly increasing
 *   end-4 crc32         u32, CRC-32 over every byte before it
 *
 * Tags of version 1:
 *
 *   1  author     1..GDX_META_NAME_MAX printable ASCII bytes, no NUL, no space at either end
 *   2  editedBy   same rules; needs an author and differs from it
 *   3  staff      u32 raceTime (1..GDX_META_STAFF_TIME_MAX, the game timer's milliseconds), u8 laps (1..99)
 *   4  madeHere   no value. GMS1 only
 *   5  splits     a split record (gdx_course_splits.h), at most GDX_META_SPLITS_MAX_BYTES. Needs a staff time,
 *                 with the same lap count and a last lap end equal to it. A record that breaks this is dropped on
 *                 read and the rest of the block still loads. A Course Edit save clears it.
 *   6  signature  GDX_META_SIGNATURE_BYTES: an Ed25519 public key and signature (gdx_course_sign.h) over the author
 *                 and the digest of the course bytes. Needs an author. The codec checks its size only: checking the
 *                 signature needs the course bytes (gdx_course_meta_sign.h). A Course Edit save clears it.
 *
 * The decoder skips tags it does not know. A later build can add records and this build still reads the author
 * and staff time. Unknown records are not kept, so a re-save drops them. GXE1 rejects
 * unknown records instead, which is why its titles had to move to their own file.
 *
 * GMS1 state file: same shape as GKS1 (gdx_course_settings.h), blocks bound to the CRC-32 of the course id.
 *
 * GMX1 sidecar, the .gdxm file next to a .gdxc in an export:
 *
 *   0x00  magic       "GMX1"
 *   0x04  version     u16 = 1
 *   0x06  entryCount  u16, 1..GDX_META_SIDECAR_MAX_ENTRIES
 *   0x08  fileLength  u32, every byte of the file including the trailing CRC
 *   0x0C  entries     entryCount x { u8 idLen, courseId[idLen], u32 bindCrc, u16 blockLen, GXM1 block[blockLen] }
 *   end-4 crc32       u32, CRC-32 over every byte before it
 *
 * Each block is bound to bindCrc, the CRC-32 of the course bytes it belongs to, so it cannot land on another course.
 * An export writes a .gdxm only when a track has metadata: 1.2.3 and older refuse a .gdxp with a member they do not
 * know (gdx_content_pack.cpp).
 */

#ifndef GDX_COURSE_META_H
#define GDX_COURSE_META_H

#include <stddef.h>
#include <stdint.h>

#include "gdx_course_branch_format.h"
#include "gdx_course_splits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_META_NAME_MAX 20
#define GDX_META_LAPS_MAX 99
/* MAX_TIMER (decomp/include/macros.h) is 60 * 60 * 1000 - 1. A staff time at or past it would mean no target. */
#define GDX_META_STAFF_TIME_MAX (60u * 60u * 1000u - 2u)
/* The lap count of the game's Time Attack records. */
#define GDX_META_RECORD_LAPS 3

#define GDX_META_TAG_AUTHOR 1
#define GDX_META_TAG_EDITED_BY 2
#define GDX_META_TAG_STAFF 3
#define GDX_META_TAG_MADE_HERE 4
#define GDX_META_STAFF_BYTES 5
#define GDX_META_TAG_SPLITS 5
#define GDX_META_SPLITS_MAX_BYTES GDX_SPLITS_VALUE_MAX_BYTES
#define GDX_META_TAG_SIGNATURE 6
#define GDX_META_SIGNATURE_BYTES 96

#define GDX_META_BLOCK_HEADER_SIZE 12
#define GDX_META_RECORD_HEADER_SIZE 3
/* The largest block this build writes: all six known records. */
#define GDX_META_BLOCK_MAX_BYTES                                                                           \
    (GDX_META_BLOCK_HEADER_SIZE + 2 * (GDX_META_RECORD_HEADER_SIZE + GDX_META_NAME_MAX) +                  \
     GDX_META_RECORD_HEADER_SIZE + GDX_META_STAFF_BYTES + GDX_META_RECORD_HEADER_SIZE +                    \
     GDX_META_RECORD_HEADER_SIZE + GDX_META_SPLITS_MAX_BYTES + GDX_META_RECORD_HEADER_SIZE +               \
     GDX_META_SIGNATURE_BYTES + 4)
/* The largest block this build reads, so a later build's extra records still fit. */
#define GDX_META_BLOCK_READ_MAX_BYTES 32768

#define GDX_META_FILE_HEADER_SIZE 12
#define GDX_META_FILE_ENTRY_MAX (1 + GDX_BRANCH_COURSE_ID_MAX + 2 + GDX_META_BLOCK_MAX_BYTES)
#define GDX_META_FILE_MAX_BYTES (GDX_META_FILE_HEADER_SIZE + GDX_BRANCH_STORE_SLOTS * GDX_META_FILE_ENTRY_MAX + 4)

#define GDX_META_SIDECAR_MAX_ENTRIES 6
#define GDX_META_SIDECAR_HEADER_SIZE 12
#define GDX_META_SIDECAR_MAX_BYTES                                                                         \
    (GDX_META_SIDECAR_HEADER_SIZE +                                                                        \
     GDX_META_SIDECAR_MAX_ENTRIES * (1 + GDX_BRANCH_COURSE_ID_MAX + 4 + 2 + GDX_META_BLOCK_READ_MAX_BYTES) + 4)
/* The largest .gdxm this build writes. */
#define GDX_META_SIDECAR_WRITE_MAX_BYTES                                                                   \
    (GDX_META_SIDECAR_HEADER_SIZE +                                                                        \
     GDX_META_SIDECAR_MAX_ENTRIES * (1 + GDX_BRANCH_COURSE_ID_MAX + 4 + 2 + GDX_META_BLOCK_MAX_BYTES) + 4)

#define GDX_META_OK 0
#define GDX_META_ERR_ARGS (-1)       /* NULL pointer or otherwise unusable argument */
#define GDX_META_ERR_FULL (-2)       /* buffer or store is full */
#define GDX_META_ERR_VALUE (-3)      /* a value breaks its rules */
#define GDX_META_ERR_TRUNCATED (-20) /* shorter than the minimum */
#define GDX_META_ERR_MAGIC (-21)     /* wrong magic */
#define GDX_META_ERR_VERSION (-22)   /* unknown version */
#define GDX_META_ERR_CRC (-23)       /* trailing CRC-32 does not match the bytes */
#define GDX_META_ERR_BINDING (-24)   /* block is bound to a different CRC-32 */
#define GDX_META_ERR_LAYOUT (-25)    /* sizes, order or ids do not add up */

typedef struct GdxCourseMeta {
    char author[GDX_META_NAME_MAX + 1];   /* "" when none */
    char editedBy[GDX_META_NAME_MAX + 1]; /* "" when none */
    uint32_t staffTime;                   /* 0 when none */
    uint8_t staffLaps;                    /* 0 when staffTime is 0, else 1..GDX_META_LAPS_MAX */
    uint8_t madeHere;                     /* 1 when a Course Edit save on this install wrote the track, else 0 */
    uint8_t hasSignature;                 /* 1 when signature holds a value, else 0 */
    unsigned char signature[GDX_META_SIGNATURE_BYTES];
    uint16_t splitsLength;                /* 0 when none, else the stored split record's length */
    unsigned char splits[GDX_META_SPLITS_MAX_BYTES];
} GdxCourseMeta;

/* 1 for a legal author or Edited by name: 1..GDX_META_NAME_MAX printable ASCII, no space at either end. */
int gdx_meta_name_ok(const char* name);

/* 1 when nothing is set. */
int gdx_meta_is_empty(const GdxCourseMeta* meta);

/* GDX_META_OK when every set field follows its rules and the fields agree with each other. */
int gdx_meta_validate(const GdxCourseMeta* meta);

/* GXM1 codec. length is the exact block size. decode zeroes out on every failure. */
int gdx_meta_encode(const GdxCourseMeta* meta, uint32_t bindCrc, unsigned char* out, size_t cap);
int gdx_meta_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseMeta* out);

/* What a Course Edit save does to a track's metadata. localName is the creator name of this install, "" or NULL
 * when none is set. The signature is always cleared and the track is marked made here. keepStaff nonzero keeps the
 * staff time and its splits: a later run must beat it to replace it, and splits whose checkpoint count no longer
 * matches the course pace the CPUs per lap instead (gdx_splits_view_pace_begin). keepStaff 0 clears them. */
void gdx_meta_apply_editor_save(GdxCourseMeta* meta, const char* localName, int keepStaff);

/* Clears the staff time and its splits, so the next finished run of GDX_META_RECORD_LAPS laps is offered as the
 * staff time even when it is slower. */
void gdx_meta_clear_staff(GdxCourseMeta* meta);

/* 1 when a finished run of raceTime over raceLaps should be offered as the staff time: the track was made here,
 * the run has GDX_META_RECORD_LAPS laps, and the track has no staff time over that many laps or the run beats it. */
int gdx_meta_staff_offer(const GdxCourseMeta* meta, int raceLaps, uint32_t raceTime);

/* Sets the staff time and clears its splits, which belonged to the old run. GDX_META_ERR_VALUE when raceTime or
 * raceLaps is out of range, meta untouched then. */
int gdx_meta_set_staff(GdxCourseMeta* meta, int raceLaps, uint32_t raceTime);

/* Sets the splits of the staff run, or clears them for NULL. GDX_META_ERR_VALUE when they do not encode or do not
 * match the staff time (same lap count, last lap end equal to it), meta untouched then. */
int gdx_meta_set_splits(GdxCourseMeta* meta, const GdxCourseSplits* splits);

/* 1 and the decoded splits when meta has them, else 0 and out is the empty model. */
int gdx_meta_get_splits(const GdxCourseMeta* meta, GdxCourseSplits* out);

/* Writes a race time the way the game shows it, 1'23"456, NUL-terminated. Returns the length, or -1 when out is
 * NULL, cap is too small or the time is past GDX_META_STAFF_TIME_MAX. */
int gdx_meta_format_time(uint32_t raceTime, char* out, size_t cap);

/* CPU target race time for a staff time, or -1 when there is none to give. stockTargets holds courseCount x 4
 * difficulty targets (D_i3_8013E328) and stockStaff the courseCount stock staff times, -1 where none loaded. The
 * median ratio of target to staff time over the stock courses, per difficulty, carries over to the custom track,
 * and the staff time is scaled from staffLaps to raceLaps.
 *
 * Expert and Master then refine it like a stock course (cpu.c func_i3_80116974): when playerRecord, the track's
 * best Time Attack time, beats the staff time, the target moves by the same amount. Time Attack records are kept
 * over GDX_META_RECORD_LAPS laps, so only when the race and the staff time both have that many. playerRecord <= 0
 * means none. */
int32_t gdx_meta_cpu_target(const GdxCourseMeta* meta, int raceLaps, int difficulty, int32_t playerRecord,
                            const uint32_t* stockTargets, const int32_t* stockStaff, int courseCount);

/* Store keyed by course id, same rules as gdx_settings_store_*: an empty value removes the entry, and a rename
 * from an id with nothing clears the new id. */
int gdx_meta_store_set(const char* courseId, const GdxCourseMeta* meta);
int gdx_meta_store_get(const char* courseId, GdxCourseMeta* out); /* 1 if found, else 0 */
int gdx_meta_store_rename(const char* oldId, const char* newId);
void gdx_meta_store_clear(void);
unsigned long gdx_meta_store_revision(void);
int gdx_meta_store_encode_file(unsigned char* out, size_t cap);
int gdx_meta_store_restore_file(const unsigned char* data, size_t length);

/* .gdxm sidecar. */
typedef struct GdxMetaSidecarEntry {
    const char* courseId;
    uint32_t bindCrc;
    const GdxCourseMeta* meta;
} GdxMetaSidecarEntry;

/* Returns the file length, or a negative code. The made-here mark is left out, and entries with nothing else are
 * skipped; when all are, returns 0 and an export writes no .gdxm. */
int gdx_meta_sidecar_encode(const GdxMetaSidecarEntry* entries, int count, unsigned char* out, size_t cap);

/* Checks the whole file first, then calls visit once per entry, madeHere always 0. A bad file visits nothing. The
 * pointer is valid during the call only. */
typedef void (*GdxMetaSidecarVisit)(const char* courseId, uint32_t bindCrc, const GdxCourseMeta* meta, void* ctx);
int gdx_meta_sidecar_decode(const unsigned char* data, size_t length, GdxMetaSidecarVisit visit, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_META_H */
