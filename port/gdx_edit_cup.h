/* port/gdx_edit_cup.h -- Edit Cup slot names on disk: CRS_ENTRY.CENT plus CRS_ENTRY.CENL.
 *
 * The Edit Cup keeps 24 slot names (4 cups x 6 courses). The stock disk file CRS_ENTRY.CENT holds them as
 * 24 x 9 bytes, so a slot name fits 8 characters. Course Edit names reach 20 (GDX_EK_FILE_NAME_MAX), so the
 * port keeps the slots 21 bytes wide in memory and splits them on disk:
 *
 *   - CRS_ENTRY.CENT stays the stock 216-byte image. A name of 8 characters or fewer is stored there; a
 *     longer one leaves its row empty, so the stock layout never holds a cut name.
 *   - CRS_ENTRY.CENL holds the long names: "GCL1" then 24 x 21 bytes, one row per slot, empty for a short
 *     name. It exists only while some slot holds a long name.
 *
 * Reading merges the two: CENT rows first, then a long CENL name fills a row whose CENT row is empty. A
 * stray CENL (an older build cleared a slot in CENT) loses to CENT, and a cleared slot stays cleared.
 *
 * Pure C with plain int sizes, so decomp files and the unit tests can include it.
 */

#ifndef GDX_EDIT_CUP_H
#define GDX_EDIT_CUP_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_EDIT_CUP_ROWS 24
#define GDX_EDIT_CUP_NAME_MAX 20
#define GDX_EDIT_CUP_NAME_SIZE (GDX_EDIT_CUP_NAME_MAX + 1)
#define GDX_EDIT_CUP_WIDE_SIZE (GDX_EDIT_CUP_ROWS * GDX_EDIT_CUP_NAME_SIZE)
#define GDX_EDIT_CUP_CENT_SLOT 9
#define GDX_EDIT_CUP_CENT_NAME_MAX (GDX_EDIT_CUP_CENT_SLOT - 1)
#define GDX_EDIT_CUP_CENT_SIZE (GDX_EDIT_CUP_ROWS * GDX_EDIT_CUP_CENT_SLOT)
#define GDX_EDIT_CUP_CENL_MAGIC "GCL1"
#define GDX_EDIT_CUP_CENL_MAGIC_SIZE 4
#define GDX_EDIT_CUP_CENL_SIZE (GDX_EDIT_CUP_CENL_MAGIC_SIZE + GDX_EDIT_CUP_WIDE_SIZE)

/* Returns 1 when name/extension is CRS_ENTRY.CENT and size is the wide in-memory image. Disk calls of that
 * shape go through the split below; any other size is passed to MFS unchanged. */
int gdx_edit_cup_is_wide_cent(const char* name, const char* extension, long size);

/* Writes the stock CENT image (GDX_EDIT_CUP_CENT_SIZE bytes) from the wide rows. Long rows become empty. */
void gdx_edit_cup_pack_cent(const char* wide, unsigned char* cent);

/* Writes the CENL image (GDX_EDIT_CUP_CENL_SIZE bytes) from the wide rows: the long rows, others empty.
 * Returns 1 when any row is long (the file is needed), 0 otherwise. */
int gdx_edit_cup_pack_cenl(const char* wide, unsigned char* cenl);

/* Fills the wide rows from a CENT image. Each row is cut at its first NUL and always ends with one. */
void gdx_edit_cup_unpack_cent(const unsigned char* cent, char* wide);

/* Checks a CENL image: exact size, magic, and each row NUL-terminated within its 21 bytes. Returns 1 if valid. */
int gdx_edit_cup_cenl_valid(const unsigned char* cenl, long size);

/* Merges a valid CENL image into wide rows already filled from CENT: a long CENL name fills a row whose
 * row is empty. Short CENL names are ignored (CENT owns them). Returns the number of rows filled. */
int gdx_edit_cup_merge_cenl(const unsigned char* cenl, char* wide);

#ifdef __cplusplus
}
#endif

#endif
