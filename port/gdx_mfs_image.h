/* port/gdx_mfs_image.h -- read-only MFS filesystem over a foreign 64DD disk image file.
 *
 * Opens any of the four 64DD image layouts in circulation, validates the MFS volume header
 * (primary, then the backup copy), lists Course Edit tracks ("CRS" extension) and Create Machine
 * machines ("CAR"), and extracts file payloads byte-exact:
 *
 *   .ndd   SDK layout, exactly 64,931,840 bytes: libleo block order after the system area.
 *   .disk  MAME/ares physical layout, exactly 70,627,520 bytes: physical surface order, with
 *          room reserved for every block on the disk and defective tracks skipped per the
 *          system data's defect table.
 *   .d64   variable size: a 0x200-byte header followed only by the ROM and RAM area LBAs.
 *   .ram   raw RAM-partition dump, exactly LEORAM_BYTE[type] bytes.
 *
 * The layout is chosen by exact file size and then confirmed by a real MFS parse, so a size
 * match alone never counts as recognition. Both big-endian (console/emulator dumps) and
 * little-endian (port-written) images are accepted; the interpretation is pinned per image by
 * the slot-0 root directory entry. Read-only by design: installing content onto the live disk
 * goes through the .gdxc validation chain (gdx_content_import.c), never through here.
 *
 * Format provenance: decomp/include/leo/mfs.h, the sources under decomp/src/leo/mfs/, zone tables in
 * decomp/src/leo/lib/leo_tbl.c. The physical-surface geometry (track and zone-start tables) and
 * the D64 range header match ares mia/medium/nintendo-64dd.cpp; both describe 64DD media
 * geometry rather than game content. Design record: devdocs/change-plan.md (2026-08-26, A1) and
 * devdocs/1.2.0-import-and-limits-scope.md (2026-08-27, W1).
 */
#ifndef GDX_MFS_IMAGE_H
#define GDX_MFS_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_MFSIMG_NAME_LEN 20 /* MfsRamDirectoryEntry.name, fixed-width, not NUL-terminated */
#define GDX_MFSIMG_EXT_LEN 5   /* MfsRamDirectoryEntry.extension, same */

#define GDX_MFSIMG_TYPE_NONE 0
#define GDX_MFSIMG_TYPE_TRACK 1
#define GDX_MFSIMG_TYPE_MACHINE 2

#define GDX_MFSIMG_OK 0
#define GDX_MFSIMG_ERR_BAD_ARGS (-1) /* NULL out-params, undersized buffer, unknown entryId */
#define GDX_MFSIMG_ERR_IO (-2)       /* fopen/fread/malloc failure */
#define GDX_MFSIMG_ERR_NOT_MFS (-3)  /* size/layout, "64dd-Multi" id, or checksum: not an MFS image */
#define GDX_MFSIMG_ERR_CORRUPT (-4)  /* FAT chain broken / entry out of range / read past EOF */
#define GDX_MFSIMG_ERR_ENCODED (-5)  /* MFS_FILE_ATTR_ENCODE payload: extraction refused */

typedef struct GdxMfsImage GdxMfsImage; /* opaque */

typedef struct GdxMfsImageEntry {
    uint16_t entryId; /* raw MFS directory index; pass back to gdx_mfsimg_read_file */
    uint16_t attr;    /* host-order attr word */
    char name[GDX_MFSIMG_NAME_LEN + 1];       /* NUL-terminated copy of the fixed-width field */
    char extension[GDX_MFSIMG_EXT_LEN + 1];   /* same */
    int32_t fileSize;
    int contentType; /* GDX_MFSIMG_TYPE_* */
    int encoded;     /* MFS_FILE_ATTR_ENCODE set: gdx_mfsimg_read_file refuses */
} GdxMfsImageEntry;

int gdx_mfsimg_open(const char* path, GdxMfsImage** out);
void gdx_mfsimg_close(GdxMfsImage* img);

const char* gdx_mfsimg_strerror(int err);

/* List file entries whose extension starts "CRS" or "CAR", across ALL parent directories (a
 * foreign disk has no meaningful working directory for us). Pass out == NULL to query the
 * count. Returns the entry count or a negative GDX_MFSIMG_ERR_*. */
int gdx_mfsimg_list(GdxMfsImage* img, GdxMfsImageEntry* out, int capacity);

/* Extract the payload of the listed entry into buf (entry.fileSize bytes). */
int gdx_mfsimg_read_file(GdxMfsImage* img, uint16_t entryId, uint8_t* buf, int32_t bufSize);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MFS_IMAGE_H */
