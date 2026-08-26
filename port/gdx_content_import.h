/* port/gdx_content_import.h -- .gdxc course/machine import (content share direction, E2).
 *
 * Mirror of the E1 export slice (gdx_content_io.h): reads .gdxc containers from the exports/
 * folder, validates them with the game's own checks, and installs them into the MFS working
 * directory. Container layout and validation order come from devdocs/menu/CONTENT_EXPORT.md
 * section "IMPORT":
 *
 *   magic/version -> known contentType/extTag -> exact payloadSize -> payload CRC-32 ->
 *   the game's validators (course checksum + creatorId + BGM range; CARD u16 checksum + stats
 *   weighting) -> Mfs_ValidateFileName -> 100-file class cap.
 *
 * Every check runs even after an earlier one failed (a rejected file must say WHY, all reasons
 * at once), and no failure ever reaches a disk write. The course checksum is mirrored verbatim
 * from Course_CalculateChecksum's EXPANSION_KIT branch (decomp/src/game/course.c, end of file)
 * over the payload bytes -- the real function checks the live COURSE_CONTEXT() global and masks
 * trackSegmentInfo bits in place, so it cannot be pointed at an untrusted buffer.
 *
 * Threading: enumeration, container parse and validation run on the menu thread under the E1
 * busy-guard (gdx_content_mfs_busy()). The install itself is enqueued to the game's disk thread
 * via the op-slot pattern in decomp/src/sys/disk/75000.c (GdxContentImport_EnqueueDiskOp,
 * op 30 -> GdxContentImport_RunOnDiskThread), which performs the twin-extension delete +
 * Mfs_SaveFile (__TMP__ swap lives inside Mfs_SaveFile, mfs_save.c:182-186) and, for CRSE
 * tracks, the edit-cup slot clear + CRS_ENTRY.CENT rewrite mirroring the editor's save op.
 * Payloads are installed verbatim -- never byteswapped (risk R7).
 *
 * E3 adds Edit-Cup register/unregister (op 31 -> GdxContentCup_RunOnDiskThread): the disk
 * thread mutates gEditCupTrackNames[slot] and persists CRS_ENTRY.CENT, mirroring
 * func_xk2_800EC110 (course_edit/19D4A0.c:176-181). Cup slots hold 8 chars + NUL; a longer
 * track name can never resolve (Mfs_GetFileIndex compares the full fixed-width name), so
 * registration refuses it instead of writing a dangling slot the game would silently clear.
 *
 * E4 adds cup bundles (contentType 3, payload layout in gdx_content_io.h): the same validation
 * chain runs per bundled entry (per-entry CRC, track validators, name rules) plus a whole-
 * bundle quota check BEFORE any write; the disk-thread install (op 32 ->
 * GdxContentBundle_RunOnDiskThread) saves every CRSD then merges only the 6 Edit-Cup rows of
 * the bundled CENT into gEditCupTrackNames (rows 6..23 keep the importer's other cups) and
 * persists CRS_ENTRY.CENT.
 *
 * A2 adds a disk-image import source (gdx_mfs_image.h): tracks and machines are read out of a
 * foreign emulator/console 64DD dump as raw payloads and fed through the same validation chain
 * minus the container-only checks (magic/version/CRC), then installed through the identical
 * op-30 disk-thread path. The image side is strictly read-only.
 */

#ifndef GDX_CONTENT_IMPORT_H
#define GDX_CONTENT_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#include "gdx_content_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validation failure bits (GdxContentImportEntry.errors). Any nonzero bit = no install. */
#define GDX_IMPORT_ERR_IO (1u << 0)               /* host file unreadable or truncated */
#define GDX_IMPORT_ERR_BAD_MAGIC (1u << 1)        /* not "GXC1" */
#define GDX_IMPORT_ERR_BAD_VERSION (1u << 2)      /* container version != 1 */
#define GDX_IMPORT_ERR_BAD_TYPE (1u << 3)         /* contentType not 1 (track) / 2 (machine) */
#define GDX_IMPORT_ERR_BAD_EXT (1u << 4)          /* extTag not CRSD/CRSE/CARD */
#define GDX_IMPORT_ERR_TYPE_EXT_MISMATCH (1u << 5)/* track type with CARD ext or vice versa */
#define GDX_IMPORT_ERR_BAD_SIZE (1u << 6)         /* payloadSize not the type's exact size */
#define GDX_IMPORT_ERR_BAD_CRC (1u << 7)          /* payload CRC-32 mismatch */
#define GDX_IMPORT_ERR_TRACK_CHECKSUM (1u << 8)   /* Course_CalculateChecksum mismatch */
#define GDX_IMPORT_ERR_TRACK_CREATOR (1u << 9)    /* creatorId != CREATOR_NINTENDO */
#define GDX_IMPORT_ERR_TRACK_BGM (1u << 10)       /* bgm > BGM_NEW_04 */
#define GDX_IMPORT_ERR_TRACK_GEOMETRY (1u << 11)  /* control point count/float out of range */
#define GDX_IMPORT_ERR_MACHINE_CHECKSUM (1u << 12)/* CARD u16 additive checksum mismatch/0 */
#define GDX_IMPORT_ERR_MACHINE_STATS (1u << 13)   /* stat rank > E..A or weighting > 13 */
#define GDX_IMPORT_ERR_BAD_NAME (1u << 14)        /* Mfs_ValidateFileName rejected the name */
#define GDX_IMPORT_ERR_QUOTA_FULL (1u << 15)      /* 100-file extension-class cap reached */
#define GDX_IMPORT_ERR_DISK_SPACE (1u << 16)      /* Mfs_RamGetFreeSize < payloadSize */
#define GDX_IMPORT_ERR_STATE_UNKNOWN (1u << 17)   /* disk busy: quota/collision not checked */
#define GDX_IMPORT_ERR_BUNDLE_STRUCTURE (1u << 18)/* bundle entry table malformed/truncated/dupe */
#define GDX_IMPORT_ERR_BUNDLE_CENT (1u << 19)     /* CENT entry missing/wrong size/dangling slot */

/* Advisory bits (GdxContentImportEntry.warnings). Install stays allowed. */
#define GDX_IMPORT_WARN_QUOTA (1u << 0)       /* class count >= QuotaWarnThreshold (default 90) */
#define GDX_IMPORT_WARN_OVERWRITE (1u << 1)   /* same name+ext exists; import overwrites it */
#define GDX_IMPORT_WARN_TWIN_DELETE (1u << 2) /* the opposite-extension twin will be deleted */
#define GDX_IMPORT_WARN_CUP_CLEAR (1u << 3)   /* CRSE: an edit-cup slot naming it will be cleared */
#define GDX_IMPORT_WARN_CUP_REPLACE (1u << 4) /* bundle: replaces the current Edit Cup contents */

/* gdx_content_import_begin return codes (on top of the shared GDX_CONTENT_ERR_* ones). */
#define GDX_CONTENT_IMPORT_ERR_VALIDATION (-100) /* errors bits set; see the entry from list */
#define GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY (-101)/* refused while a race is live */
#define GDX_CONTENT_IMPORT_ERR_PENDING (-102)    /* a previous import has not completed yet */

/* A real disk holds at most 100 files per extension class; 64 import rows is generous. */
#define GDX_CONTENT_IMPORT_MAX 64

typedef struct GdxContentImportEntry {
    char fileName[256];  /* host basename inside exports/ */
    char name[21];       /* MFS directory-entry name from the container (bundle: informational) */
    char extension[6];   /* "CRSD" / "CRSE" / "CARD" from extTag (bundle: "CENT") */
    int32_t contentType; /* GDX_CONTENT_TYPE_* */
    uint32_t flags;      /* container flags (strip bits, informational on import) */
    uint32_t errors;     /* GDX_IMPORT_ERR_* bitmask */
    uint32_t warnings;   /* GDX_IMPORT_WARN_* bitmask */
    int32_t classFileCount; /* files in the target extension class; -1 when unknown (busy) */
    int32_t bundleTrackCount; /* E4: tracks inside a cup bundle; -1 for single items */
    char bundleContents[160]; /* E4: ", "-joined bundled track names for the confirm prompt */
} GdxContentImportEntry;

/* Lists every .gdxc file in exports/ with full validation per file (payload bytes are not
 * retained). Returns the entry count or a negative GDX_CONTENT_ERR_* code. */
int gdx_content_import_list(GdxContentImportEntry* outEntries, int capacity);

/* Re-reads, re-validates and installs exports/<fileName> (never trusts a stale list snapshot).
 * Validation and staging happen on the calling (menu) thread; the MFS install is enqueued to
 * the disk thread. A cup bundle (contentType 3) validates every entry and installs all tracks +
 * the CENT merge via op 32, all-or-nothing at the validation gate. Returns GDX_CONTENT_OK once
 * the install has been queued. */
int gdx_content_import_begin(const char* fileName);
/* Nonzero while an install is queued/running on the disk thread. */
int gdx_content_import_pending(void);

/* Result of the last begun import once gdx_content_import_pending() returns 0:
 * GDX_CONTENT_OK on success, GDX_CONTENT_ERR_IO on an MFS failure (outMfsError then receives
 * gMfsError), or GDX_CONTENT_ERR_NOT_FOUND when no import has run yet. */
int gdx_content_import_result(int32_t* outMfsError);

/* After a failed bundle install (result == GDX_CONTENT_ERR_IO): the bundled entry index whose
 * write failed (bundleTrackCount = the CENT write itself), or -1 when not applicable. Earlier
 * entries were already written -- MFS has no multi-file transaction, so a mid-install failure
 * leaves a partial install; the admission gate is the pre-write validation. */
int32_t gdx_content_import_bundle_failed_index(void);

/* A2 disk-image source (direct payloads, no .gdxc container). */

/* Validates a payload read out of a foreign disk image (gdx_mfs_image.h) with the same checks
 * and order as the .gdxc container chain, minus the container-only ones (magic/version/CRC).
 * Fills outEntry for UI display; fileName is left empty (there is no host file). Menu thread. */
void gdx_content_import_validate_payload(const char* name, const char* extension, int32_t contentType,
                                         const uint8_t* payload, int32_t payloadSize, GdxContentImportEntry* outEntry);

/* Re-validates and installs a payload read out of a foreign disk image. Same guards, staging and
 * disk-thread install (op 30, twin delete + cup clear) as gdx_content_import_begin; completion is
 * polled through the same gdx_content_import_pending/result pair. Returns GDX_CONTENT_OK once the
 * install has been queued. */
int gdx_content_import_begin_payload(const char* name, const char* extension, int32_t contentType,
                                     const uint8_t* payload, int32_t payloadSize);

/* Formats error/warning bitmasks as a "; "-joined, human-readable list for the UI. */
void gdx_content_import_format_errors(uint32_t errors, char* out, size_t outCap);
void gdx_content_import_format_warnings(uint32_t warnings, char* out, size_t outCap);

/* ---------------------------------------------------------------------------------
 * E3 Edit-Cup registration (op slot 31).
 * ------------------------------------------------------------------------------- */

#define GDX_CONTENT_CUP_SLOT_COUNT 6
#define GDX_CONTENT_CUP_NAME_MAX 8 /* a cup slot is char[9]: 8 chars + NUL */

/* gdx_content_cup_register_begin / gdx_content_cup_unregister_begin return codes, on top of the
 * shared GDX_CONTENT_ERR_* and GDX_CONTENT_IMPORT_ERR_* ones. */
#define GDX_CONTENT_CUP_ERR_NAME_TOO_LONG (-110) /* > 8 chars can never resolve from a cup slot */
#define GDX_CONTENT_CUP_ERR_BAD_SLOT (-111)      /* slot outside 0..5 */
#define GDX_CONTENT_CUP_ERR_EMPTY_SLOT (-112)    /* unregister of an already-empty slot */

/* Copies the 6 Edit-Cup slot occupants ("" for an empty slot). Busy-guarded: returns
 * GDX_CONTENT_ERR_BUSY while the disk thread owns MFS state, GDX_CONTENT_ERR_NO_FS before the
 * disk filesystem is up. */
int gdx_content_cup_list(char outNames[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1]);

/* Stages and enqueues a slot write + CENT persist to the disk thread. Registration requires the
 * named CRSD to exist in the working directory; the name is validated against MFS rules and the
 * 8-char slot cap. Refused while a race is live or another content op is pending. */
int gdx_content_cup_register_begin(const char* name, int32_t slot);
int gdx_content_cup_unregister_begin(int32_t slot);

/* Same completion shape as the import: poll pending, then take the result. */
int gdx_content_cup_pending(void);
int gdx_content_cup_result(int32_t* outMfsError);

#ifdef __cplusplus
}
#endif

#endif /* GDX_CONTENT_IMPORT_H */
