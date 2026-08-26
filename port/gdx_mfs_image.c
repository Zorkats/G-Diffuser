/* port/gdx_mfs_image.c -- read-only MFS filesystem over a foreign 64DD disk image file.
 *
 * Contract and provenance live in gdx_mfs_image.h. Same host-CRT target split as
 * gdx_content_io.c: this TU vendors the libleo zone tables and mirrors the MFS on-disk layout
 * with size static_asserts rather than including decomp headers (which drag in the PORT/EK
 * macro-gated declarations only gdiffuser_game is compiled with). No game globals, no write
 * path, fully reentrant.
 *
 * Endianness: console/emulator dumps store the MFS header big-endian; the port's own disk
 * writes are host-order (decomp/src/leo has no byteswap anywhere). The volume checksum (XOR of
 * every s32 over the 3-LBA header region, Mfs_CheckChecksum mfs_ram.c:696-715) is only an
 * integrity gate here: byteswapping permutes bit positions uniformly, so a valid volume passes
 * it under BOTH readings. The interpretation is instead pinned by the slot-0 root directory,
 * which retail always formats as attr=DIRECTORY with parent MFS_ENTRY_ROOT_PARENT_DIR
 * (Mfs_CreateRootDirectory, mfs_dir.c:64-91): exactly one byte-order reading of the (0x8000,
 * 0xFFFE) pair satisfies that, so the two checks together validate and disambiguate.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread below; harmless on non-MSVC */

#include "gdx_mfs_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------
 * Vendored libleo zone tables (decomp/src/leo/lib/leo_tbl.c, verbatim) + geometry constants
 * (decomp/include/PR/leoappli.h, port/n64_leo.c). The harness known-answer checks
 * (LEORAM_BYTE per type, full-image size) turn a transcription typo into a test failure.
 * ------------------------------------------------------------------------------- */

#define GDX_MFSIMG_LEOBYTE_TBL2_SIZE 9
static const uint16_t sLeoByteTbl2[GDX_MFSIMG_LEOBYTE_TBL2_SIZE] = { 0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70,
                                                                     0x3520, 0x2FD0, 0x2A80, 0x2530 };

static const uint16_t sLeoVzoneTbl[7][0x10] = {
    { 0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8, 0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6,
      0x0EB8, 0x0FCA, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x06A2, 0x07C6, 0x08D8, 0x09EA, 0x0AFC, 0x0BC8, 0x0C94, 0x0DA6,
      0x0EB8, 0x0FCA, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08C6, 0x09EA, 0x0AFC, 0x0C0E, 0x0CDA, 0x0DA6,
      0x0EB8, 0x0FCA, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AEA, 0x0C0E, 0x0D20, 0x0DEC,
      0x0EB8, 0x0FCA, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0D0E, 0x0E32,
      0x0EFE, 0x0FCA, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x0980, 0x0A92, 0x0BA4, 0x0CB6, 0x0DC8,
      0x0EEC, 0x1010, 0x10DC },
    { 0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x093A, 0x0A4C, 0x0B5E, 0x0C70, 0x0D82,
      0x0E94, 0x0FB8, 0x10DC },
};

static const uint8_t sLeoVzonePzoneHdTbl[7][0x10] = {
    { 0x00, 0x01, 0x02, 0x09, 0x08, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A },
    { 0x00, 0x01, 0x02, 0x03, 0x0A, 0x09, 0x08, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x0B, 0x0A, 0x09, 0x08, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x06, 0x07, 0x0F, 0x0E, 0x0D },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x0F, 0x0E },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x0F },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08 },
};

static const uint16_t sLeoRamStartLba[7] = { 0x05A2, 0x07C6, 0x09EA, 0x0C0E, 0x0E32, 0x1010, 0x10DC };
static const int32_t sLeoRamByte[7] = { 0x024A9DC0, 0x01C226C0, 0x01450F00, 0x00D35680,
                                        0x006CFD40, 0x001DA240, 0x00000000 };

#define GDX_MFSIMG_LBA_MAX 4291           /* LEO_LBA_MAX, leoappli.h:23 */
#define GDX_MFSIMG_LBA_COUNT 4292         /* logical LBAs 0..4291 */
#define GDX_MFSIMG_SYS_AREA_BYTES 0x738C0 /* GDX_NDD_SYSTEM_AREA_BYTES, n64_leo.c:140 */
#define GDX_MFSIMG_NDD_TOTAL_BYTES 64931840
#define GDX_MFSIMG_DISK_TYPES_WITH_RAM 6 /* type 6 is read-only media: sLeoRamByte[6] == 0 */

/* MFS on-disk layout (decomp/include/leo/mfs.h). */
#define GDX_MFSIMG_FAT_COUNT 0xB3A
#define GDX_MFSIMG_DIR_OFFSET 0x16B0 /* offsetof(MfsRamArea, directoryEntry) */
#define GDX_MFSIMG_DIR_ENTRY_SIZE 0x30
#define GDX_MFSIMG_MAX_DIR_ENTRIES 1112
#define GDX_MFSIMG_HEADER_LBAS 3

#define GDX_MFSIMG_ATTR_ENCODE (1 << 10)    /* MFS_FILE_ATTR_ENCODE, mfs.h:88 */
#define GDX_MFSIMG_ATTR_DIRECTORY (1 << 15) /* MFS_FILE_ATTR_DIRECTORY, mfs.h:93 */
#define GDX_MFSIMG_ATTR_FILE (1 << 14)      /* MFS_FILE_ATTR_FILE, mfs.h:92 */

#define GDX_MFSIMG_FAT_UNUSED 0x0000
#define GDX_MFSIMG_FAT_OUT_OF_MANAGEMENT 0xFFFD
#define GDX_MFSIMG_FAT_PROHIBITED 0xFFFE
#define GDX_MFSIMG_FAT_LAST_BLOCK 0xFFFF

typedef char gdx_mfsimg_size_check_ndd[(0x18 * 0x4D08 == GDX_MFSIMG_SYS_AREA_BYTES) ? 1 : -1];

/* ---------------------------------------------------------------------------------
 * Handle
 * ------------------------------------------------------------------------------- */

typedef struct GdxMfsImgRawEntry {
    uint16_t attr;
    uint16_t fatId;
    int32_t fileSize;
    char name[GDX_MFSIMG_NAME_LEN];
    char extension[GDX_MFSIMG_EXT_LEN];
} GdxMfsImgRawEntry;

struct GdxMfsImage {
    uint8_t* data; /* whole image file */
    int64_t size;
    int diskType;   /* 0..5 */
    int isRawRam;   /* 0: full .ndd, 1: raw RAM-partition dump */
    int bigEndian;  /* header interpretation chosen by the checksum */
    int32_t startLBA; /* logical RAM-area start: sLeoRamStartLba[type] - 0x18 */
    int32_t dirEntryCount;
    uint16_t fat[GDX_MFSIMG_FAT_COUNT];
    GdxMfsImgRawEntry* entries; /* dirEntryCount */
};

/* ---------------------------------------------------------------------------------
 * LBA math -- LeoLBAToByte / leoLba_to_vzone reimplemented over explicit parameters (the
 * decomp versions read LEOdisk_type / __leoActive globals this host TU must not touch).
 * Returns 0 for an out-of-range LBA; every real block is at least 0x2530 bytes, so 0 is
 * unambiguous.
 * ------------------------------------------------------------------------------- */

static int32_t gdx_mfsimg_block_bytes(int diskType, int32_t logicalLba) {
    uint32_t phys = (uint32_t)logicalLba + 0x18u;
    int vzone;
    int zone;

    if (logicalLba < 0 || logicalLba > GDX_MFSIMG_LBA_MAX) {
        return 0;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if (phys < sLeoVzoneTbl[diskType][vzone]) {
            break;
        }
    }
    if (vzone == 0x10) {
        return 0;
    }
    zone = sLeoVzonePzoneHdTbl[diskType][vzone];
    if (zone >= 8) {
        zone -= 7;
    }
    return sLeoByteTbl2[zone];
}

static int64_t gdx_mfsimg_range_bytes(int diskType, int32_t fromLogical, int32_t count) {
    int64_t total = 0;
    int32_t i;
    for (i = 0; i < count; i++) {
        total += gdx_mfsimg_block_bytes(diskType, fromLogical + i);
    }
    return total;
}

/* File offset of a logical LBA inside the opened image. */
static int64_t gdx_mfsimg_lba_offset(const GdxMfsImage* img, int32_t logicalLba) {
    if (img->isRawRam) {
        return gdx_mfsimg_range_bytes(img->diskType, img->startLBA, logicalLba - img->startLBA);
    }
    return GDX_MFSIMG_SYS_AREA_BYTES + gdx_mfsimg_range_bytes(img->diskType, 0, logicalLba);
}

/* ---------------------------------------------------------------------------------
 * Endianness-parameterized field reads
 * ------------------------------------------------------------------------------- */

static uint16_t gdx_mfsimg_rd16(const uint8_t* p, int be) {
    return be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t gdx_mfsimg_rd32(const uint8_t* p, int be) {
    if (be) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* ---------------------------------------------------------------------------------
 * Header validation + parse. Mirrors Mfs_ValidateRamVolume ("64dd-Multi", mfs_ram.c:382) and
 * Mfs_CheckChecksum (XOR of every s32 over the 3-LBA region == 0, mfs_ram.c:696-715). The
 * on-disk header is exactly 3 LBAs, which for some disk types is smaller than MfsRamArea --
 * the valid entry count follows the retail formula (mfs_ram.c:179-180).
 * ------------------------------------------------------------------------------- */

static int gdx_mfsimg_try_header(GdxMfsImage* img, int64_t off, int32_t hdrBytes, int be) {
    const uint8_t* hdr;
    const uint8_t* root;
    uint32_t checksum = 0;
    int32_t i;

    if (off < 0 || hdrBytes < GDX_MFSIMG_DIR_OFFSET + GDX_MFSIMG_DIR_ENTRY_SIZE ||
        off + hdrBytes > img->size) {
        return -1;
    }
    hdr = img->data + off;
    if (memcmp(hdr, "64dd-Multi", 10) != 0) {
        return -1;
    }
    for (i = 0; i < hdrBytes / 4; i++) {
        checksum ^= gdx_mfsimg_rd32(hdr + i * 4, be);
    }
    if (checksum != 0) {
        return -1;
    }
    /* Endianness discrimination. The checksum above is bit-permutation-invariant and passes
     * under both readings of a valid volume; the slot-0 root directory separates them, since
     * retail always formats entry 0 as DIRECTORY with parent MFS_ENTRY_ROOT_PARENT_DIR
     * (Mfs_CreateRootDirectory, mfs_dir.c:64-91) and only one byte order reads (0x8000, 0xFFFE)
     * out of the stored bytes. */
    root = hdr + GDX_MFSIMG_DIR_OFFSET;
    if (!(gdx_mfsimg_rd16(root + 0x00, be) & GDX_MFSIMG_ATTR_DIRECTORY) ||
        gdx_mfsimg_rd16(root + 0x02, be) != 0xFFFE) {
        return -1;
    }

    img->bigEndian = be;
    img->dirEntryCount = (hdrBytes - GDX_MFSIMG_DIR_OFFSET) / GDX_MFSIMG_DIR_ENTRY_SIZE;
    if (img->dirEntryCount > GDX_MFSIMG_MAX_DIR_ENTRIES) {
        img->dirEntryCount = GDX_MFSIMG_MAX_DIR_ENTRIES;
    }
    for (i = 0; i < GDX_MFSIMG_FAT_COUNT; i++) {
        img->fat[i] = gdx_mfsimg_rd16(hdr + 0x3C + i * 2, be);
    }
    img->entries = (GdxMfsImgRawEntry*)malloc(sizeof(GdxMfsImgRawEntry) * (size_t)img->dirEntryCount);
    if (img->entries == NULL) {
        return -2;
    }
    for (i = 0; i < img->dirEntryCount; i++) {
        const uint8_t* e = hdr + GDX_MFSIMG_DIR_OFFSET + i * GDX_MFSIMG_DIR_ENTRY_SIZE;
        GdxMfsImgRawEntry* out = &img->entries[i];
        out->attr = gdx_mfsimg_rd16(e + 0x00, be);
        out->fatId = gdx_mfsimg_rd16(e + 0x0A, be);
        out->fileSize = (int32_t)gdx_mfsimg_rd32(e + 0x0C, be);
        memcpy(out->name, e + 0x10, GDX_MFSIMG_NAME_LEN);
        memcpy(out->extension, e + 0x24, GDX_MFSIMG_EXT_LEN);
    }
    return 0;
}

/* ---------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------- */

int gdx_mfsimg_open(const char* path, GdxMfsImage** out) {
    GdxMfsImage* img;
    FILE* f;
    long fileSize;
    int64_t primaryOff;
    int64_t backupOff;
    int32_t primaryBytes;
    int32_t backupBytes;
    int rc = GDX_MFSIMG_ERR_NOT_MFS;

    if (path == NULL || out == NULL) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }
    *out = NULL;

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_MFSIMG_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return GDX_MFSIMG_ERR_IO;
    }

    img = (GdxMfsImage*)calloc(1, sizeof(GdxMfsImage));
    if (img == NULL) {
        fclose(f);
        return GDX_MFSIMG_ERR_IO;
    }
    img->data = (uint8_t*)malloc((size_t)fileSize);
    if (img->data == NULL) {
        free(img);
        fclose(f);
        return GDX_MFSIMG_ERR_IO;
    }
    img->size = fileSize;
    if (fileSize > 0 && fread(img->data, 1, (size_t)fileSize, f) != (size_t)fileSize) {
        gdx_mfsimg_close(img);
        fclose(f);
        return GDX_MFSIMG_ERR_IO;
    }
    fclose(f);

    /* Image kind + disk type. A full .ndd names its type in the system area (byte 5, same as
     * gdx_leo_on_disk_loaded, n64_leo.c:47); a raw .ram is detected by exact size and every
     * candidate is then checksum-validated, so the size match is never trusted on its own. */
    if (img->size == GDX_MFSIMG_NDD_TOTAL_BYTES) {
        img->isRawRam = 0;
        img->diskType = img->data[5] & 0xF;
        if (img->diskType >= GDX_MFSIMG_DISK_TYPES_WITH_RAM) {
            gdx_mfsimg_close(img);
            return GDX_MFSIMG_ERR_NOT_MFS; /* type 6+: no writable RAM area */
        }
    } else {
        int t;
        img->isRawRam = 1;
        img->diskType = -1;
        for (t = 0; t < GDX_MFSIMG_DISK_TYPES_WITH_RAM; t++) {
            if (img->size == sLeoRamByte[t]) {
                img->diskType = t;
                break;
            }
        }
        if (img->diskType < 0) {
            gdx_mfsimg_close(img);
            return GDX_MFSIMG_ERR_NOT_MFS;
        }
    }

    img->startLBA = sLeoRamStartLba[img->diskType] - 0x18;
    primaryBytes = (int32_t)gdx_mfsimg_range_bytes(img->diskType, img->startLBA, GDX_MFSIMG_HEADER_LBAS);
    backupBytes = (int32_t)gdx_mfsimg_range_bytes(img->diskType, img->startLBA + GDX_MFSIMG_HEADER_LBAS,
                                                  GDX_MFSIMG_HEADER_LBAS);
    primaryOff = gdx_mfsimg_lba_offset(img, img->startLBA);
    backupOff = primaryOff + primaryBytes;

    /* Primary header, then the backup copy at startLBA+3 (Mfs_CopyRamAreaFromBackup,
     * mfs_ram.c:729); each under both endianness interpretations. try_header allocates only
     * after the checksum passes, so a failed attempt leaves img->entries NULL. */
    {
        int pass;
        for (pass = 0; pass < 2 && rc != GDX_MFSIMG_OK; pass++) {
            int64_t off = (pass == 0) ? primaryOff : backupOff;
            int32_t bytes = (pass == 0) ? primaryBytes : backupBytes;
            if (gdx_mfsimg_try_header(img, off, bytes, 1) == 0 ||
                gdx_mfsimg_try_header(img, off, bytes, 0) == 0) {
                rc = GDX_MFSIMG_OK;
            }
        }
    }

    if (rc != GDX_MFSIMG_OK) {
        gdx_mfsimg_close(img);
        return GDX_MFSIMG_ERR_NOT_MFS;
    }
    *out = img;
    return GDX_MFSIMG_OK;
}

void gdx_mfsimg_close(GdxMfsImage* img) {
    if (img == NULL) {
        return;
    }
    free(img->entries);
    free(img->data);
    free(img);
}

const char* gdx_mfsimg_strerror(int err) {
    switch (err) {
        case GDX_MFSIMG_OK:
            return "ok";
        case GDX_MFSIMG_ERR_BAD_ARGS:
            return "bad arguments";
        case GDX_MFSIMG_ERR_IO:
            return "could not read the image file";
        case GDX_MFSIMG_ERR_NOT_MFS:
            return "not a valid MFS disk image (expected a 64931840-byte .ndd or an exact-size .ram "
                   "partition; volume id or checksum mismatch)";
        case GDX_MFSIMG_ERR_CORRUPT:
            return "corrupt filesystem (broken FAT chain or data past end of image)";
        case GDX_MFSIMG_ERR_ENCODED:
            return "file is encoded on disk; extraction is not supported";
        default:
            return "unknown error";
    }
}

static int gdx_mfsimg_classify(const GdxMfsImgRawEntry* e) {
    /* Same predicate as gdx_content_classify (port/gdx_content_io.c:290). */
    if (memcmp(e->extension, "CRS", 3) == 0) {
        return GDX_MFSIMG_TYPE_TRACK;
    }
    if (memcmp(e->extension, "CAR", 3) == 0) {
        return GDX_MFSIMG_TYPE_MACHINE;
    }
    return GDX_MFSIMG_TYPE_NONE;
}

int gdx_mfsimg_list(GdxMfsImage* img, GdxMfsImageEntry* out, int capacity) {
    int32_t i;
    int count = 0;

    if (img == NULL || capacity < 0 || (out == NULL && capacity != 0)) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }
    for (i = 0; i < img->dirEntryCount; i++) {
        const GdxMfsImgRawEntry* e = &img->entries[i];
        int type;
        if (!(e->attr & GDX_MFSIMG_ATTR_FILE)) {
            continue;
        }
        type = gdx_mfsimg_classify(e);
        if (type == GDX_MFSIMG_TYPE_NONE) {
            continue;
        }
        if (out != NULL && count < capacity) {
            GdxMfsImageEntry* o = &out[count];
            o->entryId = (uint16_t)i;
            o->attr = e->attr;
            memcpy(o->name, e->name, GDX_MFSIMG_NAME_LEN);
            o->name[GDX_MFSIMG_NAME_LEN] = '\0';
            memcpy(o->extension, e->extension, GDX_MFSIMG_EXT_LEN);
            o->extension[GDX_MFSIMG_EXT_LEN] = '\0';
            o->fileSize = e->fileSize;
            o->contentType = type;
            o->encoded = (e->attr & GDX_MFSIMG_ATTR_ENCODE) ? 1 : 0;
        }
        count++;
    }
    return count;
}

int gdx_mfsimg_read_file(GdxMfsImage* img, uint16_t entryId, uint8_t* buf, int32_t bufSize) {
    const GdxMfsImgRawEntry* e;
    int32_t remaining;
    uint16_t fatId;
    int guard = 0;

    if (img == NULL || buf == NULL) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }
    if (entryId >= img->dirEntryCount) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }
    e = &img->entries[entryId];
    if (!(e->attr & GDX_MFSIMG_ATTR_FILE) || (e->attr & GDX_MFSIMG_ATTR_DIRECTORY)) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }
    if (e->attr & GDX_MFSIMG_ATTR_ENCODE) {
        /* Per-block Mfs_DecodeFile is keyed by the LBA-14 disk ID; Course Edit files are never
         * encoded, so v1 refuses loudly instead of half-supporting it. */
        return GDX_MFSIMG_ERR_ENCODED;
    }
    if (e->fileSize < 0 || bufSize < e->fileSize) {
        return GDX_MFSIMG_ERR_BAD_ARGS;
    }

    /* Block-by-block FAT walk. The retail loader reads consecutive runs (mfs_load.c); the byte
     * stream is identical and this form keeps every guard explicit. */
    remaining = e->fileSize;
    fatId = e->fatId;
    while (remaining > 0) {
        int32_t absLba;
        int32_t blockBytes;
        int64_t off;
        int32_t n;
        uint16_t next;

        if (fatId == GDX_MFSIMG_FAT_LAST_BLOCK || fatId == GDX_MFSIMG_FAT_UNUSED ||
            fatId >= GDX_MFSIMG_FAT_OUT_OF_MANAGEMENT) {
            return GDX_MFSIMG_ERR_CORRUPT; /* chain ended (or never started) with bytes left */
        }
        if (fatId >= GDX_MFSIMG_FAT_COUNT) {
            return GDX_MFSIMG_ERR_CORRUPT;
        }
        absLba = img->startLBA + fatId;
        blockBytes = gdx_mfsimg_block_bytes(img->diskType, absLba);
        if (blockBytes == 0) {
            return GDX_MFSIMG_ERR_CORRUPT; /* LBA past the disk */
        }
        off = gdx_mfsimg_lba_offset(img, absLba);
        if (off < 0 || off + blockBytes > img->size) {
            return GDX_MFSIMG_ERR_CORRUPT; /* truncated image */
        }
        n = (blockBytes < remaining) ? blockBytes : remaining;
        memcpy(buf, img->data + off, (size_t)n);
        buf += n;
        remaining -= n;
        if (remaining == 0) {
            break;
        }
        next = img->fat[fatId];
        if (++guard > GDX_MFSIMG_FAT_COUNT) {
            return GDX_MFSIMG_ERR_CORRUPT; /* cycle */
        }
        fatId = next;
    }
    return GDX_MFSIMG_OK;
}
