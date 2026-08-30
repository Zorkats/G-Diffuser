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

/* Physical-surface geometry, needed only by the MAME/ares layout. Head 0 owns physical zones
 * 0..7 and counts tracks upward from its zone start; head 1 owns 8..15 and counts downward.
 * Provenance: the same constants ares uses to convert between layouts in
 * mia/medium/nintendo-64dd.cpp (trackPhysicalTable / startOffsetTable). These describe 64DD
 * media geometry, not game content. */
static const uint16_t sLeoTrackPhysical[16] = { 0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425,
                                                0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A };

static const uint32_t sLeoPzoneStartOffset[16] = { 0x0000000, 0x05F15E0, 0x0B79D00, 0x10801A0,
                                                   0x1523720, 0x1963D80, 0x1D414C0, 0x20BBCE0,
                                                   0x23196E0, 0x28A1E00, 0x2DF5DC0, 0x3299340,
                                                   0x36D99A0, 0x3AB70E0, 0x3E31900, 0x4149200 };

#define GDX_MFSIMG_LBA_MAX 4291           /* LEO_LBA_MAX, leoappli.h:23 */
#define GDX_MFSIMG_LBA_COUNT 4292         /* logical LBAs 0..4291 */
#define GDX_MFSIMG_SYS_AREA_BYTES 0x738C0 /* GDX_NDD_SYSTEM_AREA_BYTES, n64_leo.c:140 */
#define GDX_MFSIMG_NDD_TOTAL_BYTES 64931840
#define GDX_MFSIMG_DISK_TYPES_WITH_RAM 6 /* type 6 is read-only media: sLeoRamByte[6] == 0 */

/* Foreign layouts. Sizes are the ones ares recognises (mia/medium/nintendo-64dd.cpp). */
#define GDX_MFSIMG_MAME_TOTAL_BYTES 70627520 /* 0x435B0C0, MAME/ares physical surface order */
#define GDX_MFSIMG_D64_MIN_BYTES 0x4F08      /* smallest meaningful D64 */
#define GDX_MFSIMG_D64_MAX_BYTES 0x3D79140   /* exclusive; above this a file is .ndd or MAME */
#define GDX_MFSIMG_D64_DATA_OFFSET 0x200     /* D64 header (0xE8 sys + disk id at 0x100) then LBAs */

#define GDX_MFSIMG_PHYS_LBA_COUNT 0x10DC /* physical LBAs 0..0x10DB; logical 0 is physical 0x18 */
#define GDX_MFSIMG_PHYS_LBA_BIAS 0x18
#define GDX_MFSIMG_SYS_DATA_BYTES 0xE8 /* the system data block the layouts key off */
#define GDX_MFSIMG_DEFECT_TBL_OFFSET 0x20
#define GDX_MFSIMG_ZONE_BLOCK_MAX 0x4D08 /* sLeoByteTbl2[0]; biggest block on the surface */

/* Image kinds. Selected by exact file size, then confirmed by a real MFS parse. */
#define GDX_MFSIMG_KIND_SDK 0      /* .ndd, libleo block order after the system area */
#define GDX_MFSIMG_KIND_RAM 1      /* raw RAM-partition dump */
#define GDX_MFSIMG_KIND_PHYSICAL 2 /* .disk, MAME/ares physical surface order */
#define GDX_MFSIMG_KIND_D64 3      /* .d64, sequential but ROM/RAM ranges only */

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
    int diskType;     /* 0..5 */
    int kind;         /* GDX_MFSIMG_KIND_* */
    int bigEndian;    /* header interpretation chosen by the root directory entry */
    int32_t startLBA; /* logical RAM-area start: sLeoRamStartLba[type] - 0x18 */
    int32_t dirEntryCount;
    /* Physical/D64 layouts need the disk's own system data: byte 5 carries the disk type, bytes
     * 0x08..0x17 the per-zone cumulative defect counts, and 0x20 onward the defective track
     * numbers. Zero for the sequential layouts, which never consult it. */
    uint8_t sysData[GDX_MFSIMG_SYS_DATA_BYTES];
    /* D64 only: the physical LBA ranges actually present in the file. */
    int32_t romStartLba;
    int32_t romEndLba;
    int32_t ramStartLba;
    int32_t ramEndLba;
    uint16_t fat[GDX_MFSIMG_FAT_COUNT];
    GdxMfsImgRawEntry* entries; /* dirEntryCount */
};

/* ---------------------------------------------------------------------------------
 * LBA math -- LeoLBAToByte / leoLba_to_vzone reimplemented over explicit parameters (the
 * decomp versions read LEOdisk_type / __leoActive globals this host TU must not touch).
 * Returns 0 for an out-of-range LBA; every real block is at least 0x2530 bytes, so 0 is
 * unambiguous.
 * ------------------------------------------------------------------------------- */

/* Byte size of the block holding a PHYSICAL LBA. Head 1's zones (pzone 8..15) share head 0's
 * size table shifted by 7, exactly as leoLba_to_vzone folds them. */
static int32_t gdx_mfsimg_phys_block_bytes(int diskType, int32_t physLba) {
    int vzone;
    int zone;

    if (physLba < 0 || physLba >= GDX_MFSIMG_PHYS_LBA_COUNT) {
        return 0;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if ((uint32_t)physLba < sLeoVzoneTbl[diskType][vzone]) {
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

static int32_t gdx_mfsimg_block_bytes(int diskType, int32_t logicalLba) {
    if (logicalLba < 0 || logicalLba > GDX_MFSIMG_LBA_MAX) {
        return 0;
    }
    return gdx_mfsimg_phys_block_bytes(diskType, logicalLba + GDX_MFSIMG_PHYS_LBA_BIAS);
}

static int64_t gdx_mfsimg_range_bytes(int diskType, int32_t fromLogical, int32_t count) {
    int64_t total = 0;
    int32_t i;
    for (i = 0; i < count; i++) {
        total += gdx_mfsimg_block_bytes(diskType, fromLogical + i);
    }
    return total;
}

/* MAME/ares physical surface order. A logical LBA resolves to a (zone, track, block) triple and
 * from there to a fixed offset, because the physical layout reserves room for every block on the
 * surface including the ones a dump never used. Defective tracks recorded in the system data
 * push later tracks outward, so the defect walk has to run before the offset is computed.
 * Mirrors the transform in ares mia/medium/nintendo-64dd.cpp. */
static int64_t gdx_mfsimg_physical_offset(const GdxMfsImage* img, int32_t physLba) {
    int vzone;
    int pzone;
    int head;
    int sizeIndex;
    int32_t lbaInVzone;
    int32_t trackStart;
    int32_t track;
    int32_t blockSize;
    int32_t blockIndex;
    int defectOffset;
    int defectAmount;

    if (physLba < 0 || physLba >= GDX_MFSIMG_PHYS_LBA_COUNT) {
        return -1;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if ((uint32_t)physLba < sLeoVzoneTbl[img->diskType][vzone]) {
            break;
        }
    }
    if (vzone == 0x10) {
        return -1;
    }
    pzone = sLeoVzonePzoneHdTbl[img->diskType][vzone];
    head = (pzone > 7) ? 1 : 0;
    sizeIndex = head ? (pzone - 7) : pzone;

    lbaInVzone = physLba;
    if (vzone > 0) {
        lbaInVzone -= (int32_t)sLeoVzoneTbl[img->diskType][vzone - 1];
    }

    trackStart = (int32_t)sLeoTrackPhysical[head ? (pzone - 8) : pzone];
    track = (int32_t)sLeoTrackPhysical[pzone];
    if (head) {
        track -= (lbaInVzone >> 1);
    } else {
        track += (lbaInVzone >> 1);
    }

    defectOffset = (pzone > 0) ? (int)img->sysData[8 + pzone - 1] : 0;
    defectAmount = (int)img->sysData[8 + pzone] - defectOffset;
    while (defectAmount > 0 && defectOffset >= 0 &&
           GDX_MFSIMG_DEFECT_TBL_OFFSET + defectOffset < GDX_MFSIMG_SYS_DATA_BYTES &&
           (int32_t)img->sysData[GDX_MFSIMG_DEFECT_TBL_OFFSET + defectOffset] + trackStart <= track) {
        track++;
        defectOffset++;
        defectAmount--;
    }

    /* Two blocks per track; LBAs alternate 0,1,1,0 across each pair of tracks. */
    blockIndex = ((physLba & 3) == 0 || (physLba & 3) == 3) ? 0 : 1;
    blockSize = (int32_t)sLeoByteTbl2[sizeIndex];

    return (int64_t)sLeoPzoneStartOffset[pzone] + (int64_t)(track - trackStart) * blockSize * 2 +
           (int64_t)blockSize * blockIndex;
}

/* D64 stores only the ROM and RAM areas, back to back, after a 0x200-byte header. */
static int gdx_mfsimg_d64_has_lba(const GdxMfsImage* img, int32_t physLba) {
    if (physLba < img->romStartLba) {
        return 0;
    }
    if (physLba > img->romEndLba && physLba < img->ramStartLba) {
        return 0;
    }
    if (physLba > img->ramEndLba) {
        return 0;
    }
    return 1;
}

static int64_t gdx_mfsimg_d64_offset(const GdxMfsImage* img, int32_t physLba) {
    int64_t off = GDX_MFSIMG_D64_DATA_OFFSET;
    int32_t i;

    if (!gdx_mfsimg_d64_has_lba(img, physLba)) {
        return -1;
    }
    for (i = 0; i < physLba; i++) {
        if (!gdx_mfsimg_d64_has_lba(img, i)) {
            continue;
        }
        off += gdx_mfsimg_phys_block_bytes(img->diskType, i);
    }
    return off;
}

/* File offset of a logical LBA inside the opened image, per layout. Returns a negative value
 * when the LBA is not present in this image; every caller already treats that as a failure. */
static int64_t gdx_mfsimg_lba_offset(const GdxMfsImage* img, int32_t logicalLba) {
    switch (img->kind) {
        case GDX_MFSIMG_KIND_RAM:
            return gdx_mfsimg_range_bytes(img->diskType, img->startLBA, logicalLba - img->startLBA);
        case GDX_MFSIMG_KIND_PHYSICAL:
            return gdx_mfsimg_physical_offset(img, logicalLba + GDX_MFSIMG_PHYS_LBA_BIAS);
        case GDX_MFSIMG_KIND_D64:
            return gdx_mfsimg_d64_offset(img, logicalLba + GDX_MFSIMG_PHYS_LBA_BIAS);
        case GDX_MFSIMG_KIND_SDK:
        default:
            return GDX_MFSIMG_SYS_AREA_BYTES + gdx_mfsimg_range_bytes(img->diskType, 0, logicalLba);
    }
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

static int gdx_mfsimg_try_header(GdxMfsImage* img, const uint8_t* hdr, int32_t hdrBytes, int be) {
    const uint8_t* root;
    uint32_t checksum = 0;
    int32_t i;

    if (hdr == NULL || hdrBytes < GDX_MFSIMG_DIR_OFFSET + GDX_MFSIMG_DIR_ENTRY_SIZE) {
        return -1;
    }
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

/* Copy `count` consecutive logical LBAs into `dst`, which must hold at least
 * count * GDX_MFSIMG_ZONE_BLOCK_MAX bytes. The MFS header is 3 LBAs, and on the physical and
 * D64 layouts those three are not adjacent in the file, so every layout is gathered rather than
 * pointed at in place. Returns the byte count, or -1 if any LBA is missing or truncated. */
static int32_t gdx_mfsimg_gather_lbas(const GdxMfsImage* img, int32_t firstLogicalLba, int32_t count,
                                      uint8_t* dst) {
    int32_t total = 0;
    int32_t i;

    for (i = 0; i < count; i++) {
        int32_t lba = firstLogicalLba + i;
        int32_t blockBytes = gdx_mfsimg_block_bytes(img->diskType, lba);
        int64_t off = gdx_mfsimg_lba_offset(img, lba);

        if (blockBytes <= 0 || off < 0 || off + blockBytes > img->size) {
            return -1;
        }
        memcpy(dst + total, img->data + off, (size_t)blockBytes);
        total += blockBytes;
    }
    return total;
}

/* Try to mount the volume already described by img->kind, diskType and sysData: primary header
 * first, then the backup copy at startLBA+3 (Mfs_CopyRamAreaFromBackup, mfs_ram.c:729), each
 * under both endianness interpretations. try_header allocates only after every gate passes, so a
 * failed attempt leaves img->entries NULL. */
static int gdx_mfsimg_try_mount(GdxMfsImage* img) {
    uint8_t* scratch;
    int rc = GDX_MFSIMG_ERR_NOT_MFS;
    int pass;

    if (img->diskType < 0 || img->diskType >= GDX_MFSIMG_DISK_TYPES_WITH_RAM) {
        return GDX_MFSIMG_ERR_NOT_MFS;
    }
    img->startLBA = sLeoRamStartLba[img->diskType] - GDX_MFSIMG_PHYS_LBA_BIAS;

    scratch = (uint8_t*)malloc((size_t)GDX_MFSIMG_HEADER_LBAS * GDX_MFSIMG_ZONE_BLOCK_MAX);
    if (scratch == NULL) {
        return GDX_MFSIMG_ERR_IO;
    }
    for (pass = 0; pass < 2 && rc != GDX_MFSIMG_OK; pass++) {
        int32_t first = img->startLBA + pass * GDX_MFSIMG_HEADER_LBAS;
        int32_t bytes = gdx_mfsimg_gather_lbas(img, first, GDX_MFSIMG_HEADER_LBAS, scratch);
        if (bytes < 0) {
            continue;
        }
        if (gdx_mfsimg_try_header(img, scratch, bytes, 1) == 0 ||
            gdx_mfsimg_try_header(img, scratch, bytes, 0) == 0) {
            rc = GDX_MFSIMG_OK;
        }
    }
    free(scratch);
    return rc;
}

/* ---------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------- */

int gdx_mfsimg_open(const char* path, GdxMfsImage** out) {
    GdxMfsImage* img;
    FILE* f;
    long fileSize;
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

    /* Image kind by exact size, then disk type from the layout's own system data (byte 5, same
     * as gdx_leo_on_disk_loaded, n64_leo.c:47). The size match is never trusted on its own: every
     * candidate still has to produce a real MFS volume below. .ram sizes fall inside the D64 span,
     * so they are tested first. */
    if (img->size == GDX_MFSIMG_NDD_TOTAL_BYTES) {
        img->kind = GDX_MFSIMG_KIND_SDK;
        memcpy(img->sysData, img->data, GDX_MFSIMG_SYS_DATA_BYTES);
        img->diskType = img->sysData[5] & 0xF;
        rc = gdx_mfsimg_try_mount(img);
    } else if (img->size == GDX_MFSIMG_MAME_TOTAL_BYTES) {
        /* Physical layout: the four system blocks live at physical LBAs 0, 1, 8 and 9, which map
         * to fixed offsets in pzone 0. A dump may have any of them unreadable, so each is tried
         * until one yields a mountable volume. */
        static const int32_t kSystemBlocks[4] = { 0, 1, 8, 9 };
        int i;
        img->kind = GDX_MFSIMG_KIND_PHYSICAL;
        for (i = 0; i < 4 && rc != GDX_MFSIMG_OK; i++) {
            int64_t sysOff = (int64_t)kSystemBlocks[i] * GDX_MFSIMG_ZONE_BLOCK_MAX;
            if (sysOff + GDX_MFSIMG_SYS_DATA_BYTES > img->size) {
                continue;
            }
            memcpy(img->sysData, img->data + sysOff, GDX_MFSIMG_SYS_DATA_BYTES);
            img->diskType = img->sysData[5] & 0xF;
            rc = gdx_mfsimg_try_mount(img);
        }
    } else {
        int t;
        img->diskType = -1;
        for (t = 0; t < GDX_MFSIMG_DISK_TYPES_WITH_RAM; t++) {
            if (img->size == sLeoRamByte[t]) {
                img->diskType = t;
                break;
            }
        }
        if (img->diskType >= 0) {
            img->kind = GDX_MFSIMG_KIND_RAM;
            rc = gdx_mfsimg_try_mount(img);
        } else if (img->size >= GDX_MFSIMG_D64_MIN_BYTES && img->size < GDX_MFSIMG_D64_MAX_BYTES) {
            /* D64 keeps its system data in the first 0xE8 bytes and states which physical LBA
             * ranges the file actually contains. A volume with no RAM area (0xFFFF sentinels)
             * carries no MFS and simply fails to mount. */
            img->kind = GDX_MFSIMG_KIND_D64;
            memcpy(img->sysData, img->data, GDX_MFSIMG_SYS_DATA_BYTES);
            img->diskType = img->sysData[5] & 0xF;
            img->romStartLba = GDX_MFSIMG_PHYS_LBA_BIAS;
            img->romEndLba = ((int32_t)img->sysData[0xE0] << 8) + img->sysData[0xE1] + GDX_MFSIMG_PHYS_LBA_BIAS;
            img->ramStartLba = ((int32_t)img->sysData[0xE2] << 8) + img->sysData[0xE3];
            img->ramEndLba = ((int32_t)img->sysData[0xE4] << 8) + img->sysData[0xE5];
            if (img->ramStartLba == 0xFFFF || img->ramEndLba == 0xFFFF) {
                gdx_mfsimg_close(img);
                return GDX_MFSIMG_ERR_NOT_MFS;
            }
            img->ramStartLba += GDX_MFSIMG_PHYS_LBA_BIAS;
            img->ramEndLba += GDX_MFSIMG_PHYS_LBA_BIAS;
            rc = gdx_mfsimg_try_mount(img);
        }
    }

    if (rc != GDX_MFSIMG_OK) {
        gdx_mfsimg_close(img);
        return (rc == GDX_MFSIMG_ERR_IO) ? GDX_MFSIMG_ERR_IO : GDX_MFSIMG_ERR_NOT_MFS;
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
            return "not a valid MFS disk image (expected a 64931840-byte .ndd, a 70627520-byte "
                   "MAME/ares .disk, a .d64, or an exact-size .ram partition; volume id or "
                   "checksum mismatch)";
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
