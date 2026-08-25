/* port/gdx_content_io.h -- .gdxc course/machine export (content share direction, E1).
 *
 * Spec: devdocs/menu/CONTENT_EXPORT.md (container layout, validation order, threading) with the
 * corrections from devdocs/POST_1_0_SCOPING.md section 2 (8-char practical name cap, memcpy-20
 * name copies, strip region sizes 0xBF40/0xC050 with split flag bits, busy-guard B1 mandatory
 * for export reads, direct directoryEntry[] walk B2 -- never the prepared-dir iterators).
 *
 * Container v1 (all integers little-endian, hand-packed byte stream -- never a C struct overlay):
 *
 *   0x00  magic         "GXC1" (4 bytes)
 *   0x04  version       u32 = 1
 *   0x08  contentType   u32: 1 = track (CRSD/CRSE), 2 = machine (CARD)
 *   0x0C  extTag        u32: fourcc of the MFS extension ("CRSD"/"CRSE"/"CARD")
 *   0x10  name          char[21], MFS directory-entry name, NUL-padded
 *   0x25  flags         u32: bit0 = ghosts stripped, bit1 = records stripped (track only)
 *   0x29  payloadSize   u32: 0xC830 (track) / 0x20 (machine) / variable (bundle)
 *   0x2D  payloadCrc32  u32, CRC-32 of payload
 *   0x31  reserved      16 bytes, zero
 *   0x41  payload       verbatim MFS file bytes
 *
 * E4 bundle extension (contentType 3 = cup bundle; header version stays 1, bundles are
 * self-describing via contentType and v1 readers reject the unknown type cleanly):
 * extTag = "CENT", name = "EDIT-CUP", payload =
 *
 *   u16 entryCount (LE; 1 + track count, hard-capped at 7), then per entry a hand-packed LE
 *   record { extTag u32, name char[21] NUL-padded, payloadSize u32, payloadCrc32 u32,
 *   payload bytes }. Entry 0 is the CRS_ENTRY.CENT image (216 bytes, 24 x 9-byte cup slots,
 *   only the first 6 are the Edit Cup); entries 1..N are the CRSD payloads it references.
 *
 * Threading: export is read-only against game state and runs on the menu thread, but ONLY while
 * the disk thread is idle (gdx_content_mfs_busy()). The busy-guard is structural, not
 * circumstantial: the MFS loader's tail-LBA path shares static scratch with the disk thread's
 * writes (blocker B1). Import (E2) lives in gdx_content_import.h/.c and enqueues its writes to
 * the game's disk thread; this header only carries the shared read-side queries it reuses.
 */

#ifndef GDX_CONTENT_IO_H
#define GDX_CONTENT_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CONTENT_OK 0
#define GDX_CONTENT_ERR_BAD_ARGS (-1)
#define GDX_CONTENT_ERR_BUSY (-2)       /* an MFS op is in flight on the disk thread */
#define GDX_CONTENT_ERR_IO (-3)
#define GDX_CONTENT_ERR_NOT_FOUND (-4)  /* name+ext no longer in the working directory */
#define GDX_CONTENT_ERR_BAD_SIZE (-5)   /* dir-entry size is not the exact type size */
#define GDX_CONTENT_ERR_LOAD (-6)       /* Mfs_LoadFileInDir rejected the read */
#define GDX_CONTENT_ERR_NO_FS (-7)      /* no disk filesystem is mounted/initialized */

#define GDX_CONTENT_TYPE_TRACK 1
#define GDX_CONTENT_TYPE_MACHINE 2
#define GDX_CONTENT_TYPE_BUNDLE 3 /* E4: CENT + referenced CRSDs; payload layout in the header doc */

#define GDX_CONTENT_FLAG_GHOSTS_STRIPPED (1u << 0)
#define GDX_CONTENT_FLAG_RECORDS_STRIPPED (1u << 1)

#define GDX_CONTENT_TRACK_PAYLOAD_SIZE 0xC830 /* sizeof(CourseContext), fzx_course.h:41-47 */
#define GDX_CONTENT_MACHINE_PAYLOAD_SIZE 0x20 /* sizeof(CustomMachine), unk_structs.h:504-528 */

/* E4 bundle limits: one CENT image plus one CRSD per Edit-Cup slot. */
#define GDX_CONTENT_CENT_PAYLOAD_SIZE 216          /* 24 x 9-byte slots (EK gEditCupTrackNames) */
#define GDX_CONTENT_BUNDLE_MAX_TRACKS 6
#define GDX_CONTENT_BUNDLE_ENTRY_HEADER_SIZE 33    /* extTag u32 + name[21] + size u32 + crc u32 */

/* The 100-file cap is per extension CLASS via a 3-char prefix filter (A8140.c:316), so a
 * 1112-entry table is the hard upper bound; 256 keeps the menu window's storage modest and is
 * still far above anything a real disk can hold per class. */
#define GDX_CONTENT_MAX_ENTRIES 256

typedef struct GdxContentEntry {
    char name[21];      /* memcpy of the 20-byte MFS dir-entry name + own NUL */
    char extension[6];  /* "CRSD" / "CRSE" / "CARD", NUL-terminated */
    int32_t fileSize;   /* from the dir entry */
    int32_t contentType;/* GDX_CONTENT_TYPE_* */
} GdxContentEntry;

/* Nonzero while the game's disk thread owns MFS state. Enumeration and export must be refused
 * in that window. */
int gdx_content_mfs_busy(void);

/* Fills outEntries with the working directory's CRSD/CRSE/CARD files (same 3-char prefix
 * predicate as the game's own browser). Returns the entry count, or a negative GDX_CONTENT_ERR_*
 * code. Never touches the prepared-dir iterator globals (blocker B2). */
int gdx_content_list(GdxContentEntry* outEntries, int capacity);

/* Exports one listed entry to exports/<sanitized name>.gdxc next to the exe, atomically
 * (tmp + rename). When the entry is a track and gEnhancements.Content.ExportIncludeGhosts is 0,
 * the ghostSave+records region is zeroed before packing and both strip flag bits are set.
 * On success outPath receives the written path. */
int gdx_content_export(const GdxContentEntry* entry, char* outPath, size_t outCap);

/* E4: exports the whole Edit Cup as one bundle container to exports/edit-cup.gdxc. Gathers the
 * in-memory CENT image plus the CRSD payload of every slot that names an existing CRSD file
 * (empty and dangling slots are skipped; a track named by two slots is packed once). Track
 * payloads honor the same ExportIncludeGhosts strip toggle as single exports. Returns
 * GDX_CONTENT_ERR_NOT_FOUND when no slot resolves to a track. Menu thread, busy-guarded like
 * gdx_content_export. */
int gdx_content_export_bundle(char* outPath, size_t outCap);

/* exports/ directory next to the exe (created when create != 0). Same helper shape as the ghost
 * library directory. */
int gdx_content_exports_directory(char* outPath, size_t outCap, int create);

/* CRC-32 (IEEE 802.3 / zlib polynomial), shared with the E2 import validator. */
uint32_t gdx_content_crc32(const unsigned char* data, size_t length);

/* E2 import preflight queries. Both walk gMfsRamArea.directoryEntry[] directly (blocker B2) and
 * refuse with GDX_CONTENT_ERR_BUSY while the disk thread owns MFS state (blocker B1). */

/* Counts working-directory files in a 3-char extension class ("CRS" covers CRSD+CRSE, "CAR"
 * covers CARD) -- the same predicate the game's browser applies its 100-file cap to. */
int gdx_content_count_extension_class(const char extensionPrefix[3]);

/* 1 when a file with exactly this name+extension sits in the working directory, 0 when not.
 * name is a NUL-terminated string (<= 20 chars); extension is compared over its 4 significant
 * chars ("CRSD"/"CRSE"/"CARD"). */
int gdx_content_file_exists(const char* name, const char extension[5]);

const char* gdx_content_error_string(int rc);

#ifdef __cplusplus
}
#endif

#endif /* GDX_CONTENT_IO_H */
