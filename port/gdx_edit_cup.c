/* port/gdx_edit_cup.c -- Edit Cup slot names on disk: CRS_ENTRY.CENT plus CRS_ENTRY.CENL. */

#include "gdx_edit_cup.h"

#include <string.h>

/* Length of a row, stopping at its first NUL or at max. */
static int gdx_edit_cup_row_len(const char* row, int max) {
    int len = 0;

    while (len < max && row[len] != '\0') {
        len++;
    }
    return len;
}

int gdx_edit_cup_is_wide_cent(const char* name, const char* extension, long size) {
    if (name == NULL || extension == NULL) {
        return 0;
    }
    return size == GDX_EDIT_CUP_WIDE_SIZE && strcmp(name, "CRS_ENTRY") == 0 && strcmp(extension, "CENT") == 0;
}

void gdx_edit_cup_pack_cent(const char* wide, unsigned char* cent) {
    int i;

    memset(cent, 0, GDX_EDIT_CUP_CENT_SIZE);
    for (i = 0; i < GDX_EDIT_CUP_ROWS; i++) {
        const char* row = wide + i * GDX_EDIT_CUP_NAME_SIZE;
        int len = gdx_edit_cup_row_len(row, GDX_EDIT_CUP_NAME_MAX);

        if (len <= GDX_EDIT_CUP_CENT_NAME_MAX) {
            memcpy(cent + i * GDX_EDIT_CUP_CENT_SLOT, row, (size_t) len);
        }
    }
}

int gdx_edit_cup_pack_cenl(const char* wide, unsigned char* cenl) {
    int anyLong = 0;
    int i;

    memset(cenl, 0, GDX_EDIT_CUP_CENL_SIZE);
    memcpy(cenl, GDX_EDIT_CUP_CENL_MAGIC, GDX_EDIT_CUP_CENL_MAGIC_SIZE);
    for (i = 0; i < GDX_EDIT_CUP_ROWS; i++) {
        const char* row = wide + i * GDX_EDIT_CUP_NAME_SIZE;
        int len = gdx_edit_cup_row_len(row, GDX_EDIT_CUP_NAME_MAX);

        if (len > GDX_EDIT_CUP_CENT_NAME_MAX) {
            memcpy(cenl + GDX_EDIT_CUP_CENL_MAGIC_SIZE + i * GDX_EDIT_CUP_NAME_SIZE, row, (size_t) len);
            anyLong = 1;
        }
    }
    return anyLong;
}

void gdx_edit_cup_unpack_cent(const unsigned char* cent, char* wide) {
    int i;

    memset(wide, 0, GDX_EDIT_CUP_WIDE_SIZE);
    for (i = 0; i < GDX_EDIT_CUP_ROWS; i++) {
        const char* slot = (const char*) cent + i * GDX_EDIT_CUP_CENT_SLOT;
        int len = gdx_edit_cup_row_len(slot, GDX_EDIT_CUP_CENT_NAME_MAX);

        memcpy(wide + i * GDX_EDIT_CUP_NAME_SIZE, slot, (size_t) len);
    }
}

int gdx_edit_cup_cenl_valid(const unsigned char* cenl, long size) {
    int i;

    if (cenl == NULL || size != GDX_EDIT_CUP_CENL_SIZE) {
        return 0;
    }
    if (memcmp(cenl, GDX_EDIT_CUP_CENL_MAGIC, GDX_EDIT_CUP_CENL_MAGIC_SIZE) != 0) {
        return 0;
    }
    for (i = 0; i < GDX_EDIT_CUP_ROWS; i++) {
        const char* row = (const char*) cenl + GDX_EDIT_CUP_CENL_MAGIC_SIZE + i * GDX_EDIT_CUP_NAME_SIZE;

        if (gdx_edit_cup_row_len(row, GDX_EDIT_CUP_NAME_SIZE) == GDX_EDIT_CUP_NAME_SIZE) {
            return 0;
        }
    }
    return 1;
}

int gdx_edit_cup_merge_cenl(const unsigned char* cenl, char* wide) {
    int filled = 0;
    int i;

    for (i = 0; i < GDX_EDIT_CUP_ROWS; i++) {
        const char* row = (const char*) cenl + GDX_EDIT_CUP_CENL_MAGIC_SIZE + i * GDX_EDIT_CUP_NAME_SIZE;
        char* dest = wide + i * GDX_EDIT_CUP_NAME_SIZE;
        int len = gdx_edit_cup_row_len(row, GDX_EDIT_CUP_NAME_MAX);

        if (len > GDX_EDIT_CUP_CENT_NAME_MAX && dest[0] == '\0') {
            memset(dest, 0, GDX_EDIT_CUP_NAME_SIZE);
            memcpy(dest, row, (size_t) len);
            filled++;
        }
    }
    return filled;
}
