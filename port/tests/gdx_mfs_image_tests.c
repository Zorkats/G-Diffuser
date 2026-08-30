/* Standalone unit-test harness for port/gdx_mfs_image.c. Console exe, no game deps, no
 * libultraship, no decomp headers: the module compiles unmodified alongside this file (see
 * port/CMakeLists.txt's gdx_mfs_image_tests) and is driven entirely through its public API.
 *
 * Fixtures are synthesized in memory: valid .ndd (full 64,931,840-byte image) and .ram
 * (raw RAM-partition) files in BOTH endiannesses, plus refusal fixtures (corrupt
 * primary+backup headers, wrong size, FAT cycle, early-ending chain, LBA out of range,
 * ENCODE-attr file). The fixture builder re-derives every offset from its OWN copy of the
 * libleo zone tables; two known-answer checks anchor that copy to retail constants
 * (LEORAM_BYTE per disk type, and total user bytes + 0x738C0 == 64,931,840), so a table
 * transcription typo fails here instead of silently agreeing with the module.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gdx_mfs_image.h"

/* Sub-check bookkeeping, same shape as pcm_capture_tests.c. */
static int gSubChecks = 0;
static int gSubFails = 0;

static void checkTrue(const char* what, int cond) {
    gSubChecks++;
    if (!cond) {
        gSubFails++;
        printf("    [x] %s\n", what);
    }
}

static void checkEqLong(const char* what, long got, long expected) {
    gSubChecks++;
    if (got != expected) {
        gSubFails++;
        printf("    [x] %s: got %ld, expected %ld\n", what, got, expected);
    }
}

/* ---------------------------------------------------------------------------------
 * Fixture-builder copy of the libleo zone tables (decomp/src/leo/lib/leo_tbl.c) and geometry
 * constants. Retyped independently of the module; anchored by the known-answer case below.
 * ------------------------------------------------------------------------------- */

static const uint16_t hxByteTbl2[9] = { 0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70,
                                        0x3520, 0x2FD0, 0x2A80, 0x2530 };

static const uint16_t hxVzoneTbl[7][0x10] = {
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

static const uint8_t hxPzoneHdTbl[7][0x10] = {
    { 0x00, 0x01, 0x02, 0x09, 0x08, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A },
    { 0x00, 0x01, 0x02, 0x03, 0x0A, 0x09, 0x08, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x0B, 0x0A, 0x09, 0x08, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x06, 0x07, 0x0F, 0x0E, 0x0D },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x0F, 0x0E },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x0F },
    { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08 },
};

static const uint16_t hxRamStartLba[7] = { 0x05A2, 0x07C6, 0x09EA, 0x0C0E, 0x0E32, 0x1010, 0x10DC };
static const int32_t hxRamByte[7] = { 0x024A9DC0, 0x01C226C0, 0x01450F00, 0x00D35680,
                                      0x006CFD40, 0x001DA240, 0x00000000 };

#define HX_LBA_MAX 4291
#define HX_LBA_COUNT 4292
#define HX_SYS_AREA_BYTES 0x738C0
#define HX_NDD_TOTAL_BYTES 64931840
#define HX_FAT_COUNT 0xB3A
#define HX_DIR_OFFSET 0x16B0
#define HX_DIR_ENTRY_SIZE 0x30
#define HX_HEADER_LBAS 3

#define HX_ATTR_FILE (1 << 14)
#define HX_ATTR_DIRECTORY (1 << 15)
#define HX_ATTR_ENCODE (1 << 10)

#define HX_FAT_UNUSED 0x0000
#define HX_FAT_OUT_OF_MANAGEMENT 0xFFFD
#define HX_FAT_LAST_BLOCK 0xFFFF

static int32_t hx_block_bytes(int diskType, int32_t logicalLba) {
    uint32_t phys = (uint32_t)logicalLba + 0x18u;
    int vzone;
    int zone;

    if (logicalLba < 0 || logicalLba > HX_LBA_MAX) {
        return 0;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if (phys < hxVzoneTbl[diskType][vzone]) {
            break;
        }
    }
    if (vzone == 0x10) {
        return 0;
    }
    zone = hxPzoneHdTbl[diskType][vzone];
    if (zone >= 8) {
        zone -= 7;
    }
    return hxByteTbl2[zone];
}

static int64_t hx_range_bytes(int diskType, int32_t fromLogical, int32_t count) {
    int64_t total = 0;
    int32_t i;
    for (i = 0; i < count; i++) {
        total += hx_block_bytes(diskType, fromLogical + i);
    }
    return total;
}

static int64_t hx_lba_offset(int isRawRam, int diskType, int32_t startLBA, int32_t logicalLba) {
    if (isRawRam) {
        return hx_range_bytes(diskType, startLBA, logicalLba - startLBA);
    }
    return HX_SYS_AREA_BYTES + hx_range_bytes(diskType, 0, logicalLba);
}

/* ---------------------------------------------------------------------------------
 * Foreign-layout encoders. These exist so the round-trip tests never need a captured disk
 * dump: a synthesized .ndd is re-encoded here into the MAME/ares physical layout and into
 * D64, and the module must read identical files and identical payload bytes out of all three.
 * Tables are retyped independently of the module, same rule as the zone tables above.
 * ------------------------------------------------------------------------------- */

static const uint16_t hxTrackPhysical[16] = { 0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425,
                                              0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A };

static const uint32_t hxPzoneStartOffset[16] = { 0x0000000, 0x05F15E0, 0x0B79D00, 0x10801A0,
                                                 0x1523720, 0x1963D80, 0x1D414C0, 0x20BBCE0,
                                                 0x23196E0, 0x28A1E00, 0x2DF5DC0, 0x3299340,
                                                 0x36D99A0, 0x3AB70E0, 0x3E31900, 0x4149200 };

#define HX_MAME_TOTAL_BYTES 70627520 /* 0x435B0C0 */
#define HX_PHYS_LBA_COUNT 0x10DC
#define HX_PHYS_LBA_BIAS 0x18
#define HX_SYS_DATA_BYTES 0xE8
#define HX_D64_DATA_OFFSET 0x200

static int32_t hx_phys_block_bytes(int diskType, int32_t physLba) {
    int vzone;
    int zone;

    if (physLba < 0 || physLba >= HX_PHYS_LBA_COUNT) {
        return 0;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if ((uint32_t)physLba < hxVzoneTbl[diskType][vzone]) {
            break;
        }
    }
    if (vzone == 0x10) {
        return 0;
    }
    zone = hxPzoneHdTbl[diskType][vzone];
    if (zone >= 8) {
        zone -= 7;
    }
    return hxByteTbl2[zone];
}

/* Offset of a physical LBA inside a full .ndd: the SDK layout is simply every physical block
 * end to end, and the first 24 of them are the system area (0x18 * 0x4D08 == 0x738C0). */
static int64_t hx_ndd_phys_offset(int diskType, int32_t physLba) {
    int64_t off = 0;
    int32_t i;
    for (i = 0; i < physLba; i++) {
        off += hx_phys_block_bytes(diskType, i);
    }
    return off;
}

static int64_t hx_physical_offset(int diskType, const uint8_t* sysData, int32_t physLba) {
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

    if (physLba < 0 || physLba >= HX_PHYS_LBA_COUNT) {
        return -1;
    }
    for (vzone = 0; vzone < 0x10; vzone++) {
        if ((uint32_t)physLba < hxVzoneTbl[diskType][vzone]) {
            break;
        }
    }
    if (vzone == 0x10) {
        return -1;
    }
    pzone = hxPzoneHdTbl[diskType][vzone];
    head = (pzone > 7) ? 1 : 0;
    sizeIndex = head ? (pzone - 7) : pzone;

    lbaInVzone = physLba;
    if (vzone > 0) {
        lbaInVzone -= (int32_t)hxVzoneTbl[diskType][vzone - 1];
    }

    trackStart = (int32_t)hxTrackPhysical[head ? (pzone - 8) : pzone];
    track = (int32_t)hxTrackPhysical[pzone];
    if (head) {
        track -= (lbaInVzone >> 1);
    } else {
        track += (lbaInVzone >> 1);
    }

    defectOffset = (pzone > 0) ? (int)sysData[8 + pzone - 1] : 0;
    defectAmount = (int)sysData[8 + pzone] - defectOffset;
    while (defectAmount > 0 && HX_SYS_DATA_BYTES > 0x20 + defectOffset &&
           (int32_t)sysData[0x20 + defectOffset] + trackStart <= track) {
        track++;
        defectOffset++;
        defectAmount--;
    }

    blockIndex = ((physLba & 3) == 0 || (physLba & 3) == 3) ? 0 : 1;
    blockSize = (int32_t)hxByteTbl2[sizeIndex];

    return (int64_t)hxPzoneStartOffset[pzone] + (int64_t)(track - trackStart) * blockSize * 2 +
           (int64_t)blockSize * blockIndex;
}

/* Re-encode a full .ndd buffer into the MAME/ares physical layout. */
static uint8_t* hx_encode_physical(const uint8_t* ndd, int64_t nddSize, int diskType, const uint8_t* sysData,
                                   int64_t* outSize) {
    uint8_t* out = (uint8_t*)calloc(1, HX_MAME_TOTAL_BYTES);
    int64_t srcOff = 0;
    int32_t lba;

    if (out == NULL) {
        return NULL;
    }
    for (lba = 0; lba < HX_PHYS_LBA_COUNT; lba++) {
        int32_t bytes = hx_phys_block_bytes(diskType, lba);
        int64_t dstOff = hx_physical_offset(diskType, sysData, lba);
        if (bytes <= 0 || dstOff < 0 || dstOff + bytes > HX_MAME_TOTAL_BYTES || srcOff + bytes > nddSize) {
            free(out);
            return NULL;
        }
        memcpy(out + dstOff, ndd + srcOff, (size_t)bytes);
        srcOff += bytes;
    }
    /* The physical layout's own system data sits at physical block 0, which maps to offset 0. */
    memcpy(out, sysData, HX_SYS_DATA_BYTES);
    *outSize = HX_MAME_TOTAL_BYTES;
    return out;
}

/* Re-encode a full .ndd buffer into D64: 0x200-byte header, then only the ROM and RAM ranges. */
static uint8_t* hx_encode_d64(const uint8_t* ndd, int64_t nddSize, int diskType, const uint8_t* sysData,
                              int32_t romEndStored, int64_t* outSize) {
    int32_t romStart = HX_PHYS_LBA_BIAS;
    int32_t romEnd = romEndStored + HX_PHYS_LBA_BIAS;
    int32_t ramStartStored = hxRamStartLba[diskType] - HX_PHYS_LBA_BIAS;
    int32_t ramEndStored = HX_LBA_COUNT - 1;
    int32_t ramStart = ramStartStored + HX_PHYS_LBA_BIAS;
    int32_t ramEnd = ramEndStored + HX_PHYS_LBA_BIAS;
    int64_t total = HX_D64_DATA_OFFSET;
    uint8_t* out;
    int64_t dstOff;
    int32_t lba;

    for (lba = 0; lba < HX_PHYS_LBA_COUNT; lba++) {
        if (lba < romStart) {
            continue;
        }
        if (lba > romEnd && lba < ramStart) {
            continue;
        }
        if (lba > ramEnd) {
            continue;
        }
        total += hx_phys_block_bytes(diskType, lba);
    }
    out = (uint8_t*)calloc(1, (size_t)total);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, sysData, HX_SYS_DATA_BYTES);
    out[0xE0] = (uint8_t)((romEndStored >> 8) & 0xFF);
    out[0xE1] = (uint8_t)(romEndStored & 0xFF);
    out[0xE2] = (uint8_t)((ramStartStored >> 8) & 0xFF);
    out[0xE3] = (uint8_t)(ramStartStored & 0xFF);
    out[0xE4] = (uint8_t)((ramEndStored >> 8) & 0xFF);
    out[0xE5] = (uint8_t)(ramEndStored & 0xFF);

    dstOff = HX_D64_DATA_OFFSET;
    for (lba = 0; lba < HX_PHYS_LBA_COUNT; lba++) {
        int32_t bytes = hx_phys_block_bytes(diskType, lba);
        int64_t srcOff = hx_ndd_phys_offset(diskType, lba);
        if (lba < romStart) {
            continue;
        }
        if (lba > romEnd && lba < ramStart) {
            continue;
        }
        if (lba > ramEnd) {
            continue;
        }
        if (bytes <= 0 || srcOff + bytes > nddSize || dstOff + bytes > total) {
            free(out);
            return NULL;
        }
        memcpy(out + dstOff, ndd + srcOff, (size_t)bytes);
        dstOff += bytes;
    }
    *outSize = total;
    return out;
}

static void hx_wr16(uint8_t* p, uint16_t v, int be) {
    if (be) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static void hx_wr32(uint8_t* p, uint32_t v, int be) {
    if (be) {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }
}

/* ---------------------------------------------------------------------------------
 * Fixture builder
 *
 * Directory slots: 0 = root dir, 1 = track "TESTCRS" (CRSD), 2 = machine "PURPLE" (CARD,
 * inside subdirectory slot 6 to prove cross-parent listing), 3 = encoded track "ENC" (CRSD),
 * 4 = decoy "NOTES" (TXT, must not be listed), 6 = subdirectory "SUBDIR".
 * FAT: [0..5] out-of-management (header + backup), track chain 6 -> 7 -> 20 -> LAST
 * (mutated by spec), 8/9/10 single-block files.
 * ------------------------------------------------------------------------------- */

#define HX_SLOT_TRACK 1
#define HX_SLOT_MACHINE 2
#define HX_SLOT_ENCODED 3
#define HX_SLOT_DECOY 4
#define HX_SLOT_SUBDIR 6

typedef struct HxSpec {
    int diskType;
    int bigEndian;
    int isRawRam;
    int corruptPrimary;
    int corruptBackup;
    int fatCycle;       /* track chain 6 <-> 7 with a fileSize that spans the guard */
    int chainEndsEarly; /* FAT[6] = LAST with 3 blocks of fileSize left */
    int fatIdOutOfRange;/* track's first fatId points past the disk */
} HxSpec;

typedef struct HxFixture {
    uint8_t* data;
    int64_t size;
    int32_t trackSize;
    int32_t machineSize;
    int32_t encodedSize;
} HxFixture;

static uint8_t hx_track_pattern(int32_t i) {
    return (uint8_t)((i * 131 + 17) & 0xFF);
}

static uint8_t hx_machine_pattern(int32_t i) {
    return (uint8_t)((i * 53 + 1) & 0xFF);
}

static void hx_put_entry(uint8_t* hdr, int be, int slot, uint16_t attr, uint16_t parent, uint16_t fatId,
                         int32_t fileSize, const char* name, const char* ext) {
    uint8_t* e = hdr + HX_DIR_OFFSET + slot * HX_DIR_ENTRY_SIZE;
    hx_wr16(e + 0x00, attr, be);
    hx_wr16(e + 0x02, parent, be);
    hx_wr16(e + 0x0A, fatId, be);
    hx_wr32(e + 0x0C, (uint32_t)fileSize, be);
    if (name != NULL) {
        memcpy(e + 0x10, name, strlen(name));
    }
    if (ext != NULL) {
        memcpy(e + 0x24, ext, strlen(ext));
    }
}

static int hx_build_fixture(const HxSpec* spec, HxFixture* out) {
    int32_t startLBA = hxRamStartLba[spec->diskType] - 0x18;
    int32_t hdrBytes = (int32_t)hx_range_bytes(spec->diskType, startLBA, HX_HEADER_LBAS);
    int64_t primaryOff;
    int64_t backupOff;
    uint8_t* hdr;
    int be = spec->bigEndian;
    int32_t bb6;
    int32_t i;

    memset(out, 0, sizeof(*out));
    out->size = spec->isRawRam ? hx_range_bytes(spec->diskType, startLBA, HX_LBA_COUNT - startLBA)
                               : HX_NDD_TOTAL_BYTES;
    out->data = (uint8_t*)calloc(1, (size_t)out->size);
    hdr = (uint8_t*)calloc(1, (size_t)hdrBytes);
    if (out->data == NULL || hdr == NULL) {
        free(out->data);
        free(hdr);
        return -1;
    }

    if (!spec->isRawRam) {
        out->data[5] = (uint8_t)spec->diskType; /* system-area disk type, n64_leo.c:47 */
    }

    /* MfsRamId. */
    memcpy(hdr, "64dd-Multi", 10);
    hdr[0x0F] = (uint8_t)spec->diskType;
    memcpy(hdr + 0x10, "GDX TEST", 8);
    hx_wr16(hdr + 0x28, 1, be); /* renewalCounter */

    /* FAT: header + backup blocks are out of management. */
    for (i = 0; i < 6; i++) {
        hx_wr16(hdr + 0x3C + i * 2, HX_FAT_OUT_OF_MANAGEMENT, be);
    }
    if (spec->fatCycle) {
        hx_wr16(hdr + 0x3C + 6 * 2, 7, be);
        hx_wr16(hdr + 0x3C + 7 * 2, 6, be);
    } else if (spec->chainEndsEarly) {
        hx_wr16(hdr + 0x3C + 6 * 2, HX_FAT_LAST_BLOCK, be);
    } else {
        hx_wr16(hdr + 0x3C + 6 * 2, 7, be);
        hx_wr16(hdr + 0x3C + 7 * 2, 20, be);
        hx_wr16(hdr + 0x3C + 20 * 2, HX_FAT_LAST_BLOCK, be);
    }
    hx_wr16(hdr + 0x3C + 8 * 2, HX_FAT_LAST_BLOCK, be);
    hx_wr16(hdr + 0x3C + 9 * 2, HX_FAT_LAST_BLOCK, be);
    hx_wr16(hdr + 0x3C + 10 * 2, HX_FAT_LAST_BLOCK, be);

    /* Directory entries. */
    bb6 = hx_block_bytes(spec->diskType, startLBA + 6);
    out->trackSize = bb6 + hx_block_bytes(spec->diskType, startLBA + 7) + 100;
    out->machineSize = 32;
    out->encodedSize = 16;

    hx_put_entry(hdr, be, 0, HX_ATTR_DIRECTORY, 0xFFFE, 0, 0, "ROOT", NULL);
    hx_put_entry(hdr, be, HX_SLOT_TRACK, HX_ATTR_FILE, 0, spec->fatIdOutOfRange ? (HX_FAT_COUNT - 1) : 6,
                 spec->fatCycle ? (HX_FAT_COUNT + 2) * bb6 : out->trackSize, "TESTCRS", "CRSD");
    hx_put_entry(hdr, be, HX_SLOT_MACHINE, HX_ATTR_FILE, HX_SLOT_SUBDIR, 8, out->machineSize, "PURPLE", "CARD");
    hx_put_entry(hdr, be, HX_SLOT_ENCODED, HX_ATTR_FILE | HX_ATTR_ENCODE, 0, 9, out->encodedSize, "ENC", "CRSD");
    hx_put_entry(hdr, be, HX_SLOT_DECOY, HX_ATTR_FILE, 0, 10, 16, "NOTES", "TXT");
    hx_put_entry(hdr, be, HX_SLOT_SUBDIR, HX_ATTR_DIRECTORY, 0, 0, 0, "SUBDIR", NULL);

    /* Volume checksum: XOR of every s32 over the 3-LBA region, field zeroed then stored
     * (Mfs_CalculateVolumeChecksum, mfs_ram.c:678-694). */
    {
        uint32_t checksum = 0;
        for (i = 0; i < hdrBytes / 4; i++) {
            uint32_t w = be ? ((uint32_t)hdr[i * 4] << 24) | ((uint32_t)hdr[i * 4 + 1] << 16) |
                                  ((uint32_t)hdr[i * 4 + 2] << 8) | hdr[i * 4 + 3]
                            : ((uint32_t)hdr[i * 4 + 3] << 24) | ((uint32_t)hdr[i * 4 + 2] << 16) |
                                  ((uint32_t)hdr[i * 4 + 1] << 8) | hdr[i * 4];
            checksum ^= w;
        }
        hx_wr32(hdr + 0x2C, checksum, be);
    }

    /* Primary + backup header copies. */
    primaryOff = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA);
    backupOff = primaryOff + hdrBytes;
    memcpy(out->data + primaryOff, hdr, (size_t)hdrBytes);
    memcpy(out->data + backupOff, hdr, (size_t)hdrBytes);
    free(hdr);

    /* Payloads (pattern bytes; endianness-free). */
    if (!spec->fatIdOutOfRange) {
        int64_t off6 = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA + 6);
        int64_t off7 = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA + 7);
        int64_t off20 = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA + 20);
        int64_t off8 = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA + 8);
        int64_t off9 = hx_lba_offset(spec->isRawRam, spec->diskType, startLBA, startLBA + 9);
        int32_t bb7 = hx_block_bytes(spec->diskType, startLBA + 7);
        for (i = 0; i < bb6; i++) {
            out->data[off6 + i] = hx_track_pattern(i);
        }
        for (i = 0; i < bb7; i++) {
            out->data[off7 + i] = hx_track_pattern(bb6 + i);
        }
        for (i = 0; i < 100; i++) {
            out->data[off20 + i] = hx_track_pattern(bb6 + bb7 + i);
        }
        for (i = 0; i < out->machineSize; i++) {
            out->data[off8 + i] = hx_machine_pattern(i);
        }
        for (i = 0; i < out->encodedSize; i++) {
            out->data[off9 + i] = (uint8_t)(0xA5 ^ i);
        }
    }

    if (spec->corruptPrimary) {
        out->data[primaryOff + 0x3C] ^= 0xFF;
    }
    if (spec->corruptBackup) {
        out->data[backupOff + 0x3C] ^= 0xFF;
    }
    return 0;
}

static void hx_free_fixture(HxFixture* fx) {
    free(fx->data);
    fx->data = NULL;
}

/* ---------------------------------------------------------------------------------
 * Temp-file plumbing (one shared path, cases are sequential)
 * ------------------------------------------------------------------------------- */

#define HX_TEMP_PATH "gdx_mfsimg_test.bin"

static int hx_write_temp(const uint8_t* data, int64_t size) {
    FILE* f = fopen(HX_TEMP_PATH, "wb");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------------------
 * Cases
 * ------------------------------------------------------------------------------- */

/* Anchors the fixture builder's zone math to retail constants (leo_tbl.c:45-47,
 * n64_leo.c:140). A transcription typo in EITHER copy of the tables fails here. */
static void CaseZoneMathKnownAnswers(void) {
    int t;
    /* Casts: every value here is at most 64,931,840, which fits a 32-bit long. */
    checkEqLong("full .ndd size: sys area + user bytes",
                (long)(hx_range_bytes(0, 0, HX_LBA_COUNT) + HX_SYS_AREA_BYTES), HX_NDD_TOTAL_BYTES);
    for (t = 0; t < 6; t++) {
        int32_t ramTop = hxRamStartLba[t] - 0x18;
        checkEqLong("LEORAM_BYTE[type]", (long)hx_range_bytes(t, ramTop, HX_LBA_COUNT - ramTop), hxRamByte[t]);
    }
}

/* Shared runner for the four valid-image variants. */
static void RunValidImage(const HxSpec* spec) {
    HxFixture fx;
    GdxMfsImage* img = NULL;
    GdxMfsImageEntry entries[8];
    GdxMfsImageEntry* track = NULL;
    GdxMfsImageEntry* machine = NULL;
    GdxMfsImageEntry* encoded = NULL;
    int count;
    int i;
    int rc;

    checkTrue("fixture built", hx_build_fixture(spec, &fx) == 0);
    if (fx.data == NULL) {
        return;
    }
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);

    rc = gdx_mfsimg_open(HX_TEMP_PATH, &img);
    checkEqLong("open", rc, GDX_MFSIMG_OK);
    if (rc != GDX_MFSIMG_OK) {
        printf("      (strerror: %s)\n", gdx_mfsimg_strerror(rc));
        hx_free_fixture(&fx);
        return;
    }

    checkEqLong("list count query (NULL out)", gdx_mfsimg_list(img, NULL, 0), 3);
    count = gdx_mfsimg_list(img, entries, 8);
    checkEqLong("list count", count, 3);
    if (count == 3) {
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "TESTCRS") == 0) {
                track = &entries[i];
            } else if (strcmp(entries[i].name, "PURPLE") == 0) {
                machine = &entries[i];
            } else if (strcmp(entries[i].name, "ENC") == 0) {
                encoded = &entries[i];
            }
        }
        checkTrue("track listed", track != NULL);
        checkTrue("machine listed (from subdirectory)", machine != NULL);
        checkTrue("encoded file listed", encoded != NULL);
        if (track != NULL) {
            checkEqLong("track size", track->fileSize, fx.trackSize);
            checkEqLong("track type", track->contentType, GDX_MFSIMG_TYPE_TRACK);
            checkEqLong("track encoded flag", track->encoded, 0);
            checkTrue("track extension", strcmp(track->extension, "CRSD") == 0);
        }
        if (machine != NULL) {
            checkEqLong("machine size", machine->fileSize, fx.machineSize);
            checkEqLong("machine type", machine->contentType, GDX_MFSIMG_TYPE_MACHINE);
        }
        if (encoded != NULL) {
            checkEqLong("encoded flag", encoded->encoded, 1);
        }
    }

    if (track != NULL) {
        uint8_t* buf = (uint8_t*)malloc((size_t)fx.trackSize);
        checkTrue("track buf alloc", buf != NULL);
        if (buf != NULL) {
            rc = gdx_mfsimg_read_file(img, track->entryId, buf, fx.trackSize);
            checkEqLong("track read", rc, GDX_MFSIMG_OK);
            if (rc == GDX_MFSIMG_OK) {
                int mismatch = 0;
                for (i = 0; i < fx.trackSize; i++) {
                    if (buf[i] != hx_track_pattern(i)) {
                        mismatch = 1;
                        printf("      first payload mismatch at byte %d\n", i);
                        break;
                    }
                }
                checkTrue("track payload byte-exact", !mismatch);
            }
            checkEqLong("undersized buffer refused",
                        gdx_mfsimg_read_file(img, track->entryId, buf, fx.trackSize - 1), GDX_MFSIMG_ERR_BAD_ARGS);
            free(buf);
        }
        checkEqLong("unknown entryId refused", gdx_mfsimg_read_file(img, 4000, (uint8_t*)entries, 16),
                    GDX_MFSIMG_ERR_BAD_ARGS);
    }
    if (machine != NULL) {
        uint8_t buf[32];
        rc = gdx_mfsimg_read_file(img, machine->entryId, buf, sizeof(buf));
        checkEqLong("machine read", rc, GDX_MFSIMG_OK);
        if (rc == GDX_MFSIMG_OK) {
            int mismatch = 0;
            for (i = 0; i < fx.machineSize; i++) {
                if (buf[i] != hx_machine_pattern(i)) {
                    mismatch = 1;
                    break;
                }
            }
            checkTrue("machine payload byte-exact", !mismatch);
        }
    }
    if (encoded != NULL) {
        uint8_t buf[16];
        checkEqLong("encoded read refused", gdx_mfsimg_read_file(img, encoded->entryId, buf, sizeof(buf)),
                    GDX_MFSIMG_ERR_ENCODED);
    }

    gdx_mfsimg_close(img);
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CaseValidNddBE(void) {
    HxSpec spec = { 0, 1, 0, 0, 0, 0, 0, 0 };
    RunValidImage(&spec);
}

static void CaseValidNddLE(void) {
    HxSpec spec = { 0, 0, 0, 0, 0, 0, 0, 0 };
    RunValidImage(&spec);
}

static void CaseValidRamBE(void) {
    HxSpec spec = { 5, 1, 1, 0, 0, 0, 0, 0 };
    RunValidImage(&spec);
}

static void CaseValidRamLEType2(void) {
    HxSpec spec = { 2, 0, 1, 0, 0, 0, 0, 0 };
    RunValidImage(&spec);
}

static void CaseBackupFallback(void) {
    HxSpec spec = { 5, 1, 1, 1, 0, 0, 0, 0 };
    HxFixture fx;
    GdxMfsImage* img = NULL;
    GdxMfsImageEntry entries[8];
    int count;
    int i;
    int rc;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);
    rc = gdx_mfsimg_open(HX_TEMP_PATH, &img);
    checkEqLong("open falls back to backup header", rc, GDX_MFSIMG_OK);
    if (rc == GDX_MFSIMG_OK) {
        uint8_t* buf;
        count = gdx_mfsimg_list(img, entries, 8);
        checkEqLong("list from backup header", count, 3);
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "TESTCRS") == 0) {
                buf = (uint8_t*)malloc((size_t)fx.trackSize);
                if (buf != NULL) {
                    rc = gdx_mfsimg_read_file(img, entries[i].entryId, buf, fx.trackSize);
                    checkEqLong("track read via backup FAT", rc, GDX_MFSIMG_OK);
                    if (rc == GDX_MFSIMG_OK) {
                        checkTrue("payload byte-exact", buf[0] == hx_track_pattern(0) &&
                                                            buf[fx.trackSize - 1] == hx_track_pattern(fx.trackSize - 1));
                    }
                    free(buf);
                }
                break;
            }
        }
        gdx_mfsimg_close(img);
    }
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CaseCorruptBothHeadersRefused(void) {
    HxSpec spec = { 5, 1, 1, 1, 1, 0, 0, 0 };
    HxFixture fx;
    GdxMfsImage* img = NULL;
    int rc;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);
    rc = gdx_mfsimg_open(HX_TEMP_PATH, &img);
    checkEqLong("both headers corrupt -> NOT_MFS", rc, GDX_MFSIMG_ERR_NOT_MFS);
    checkTrue("no handle on refusal", img == NULL);
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CaseBadSizeRefused(void) {
    uint8_t buf[12345];
    GdxMfsImage* img = NULL;

    memset(buf, 0, sizeof(buf));
    checkTrue("fixture written", hx_write_temp(buf, sizeof(buf)) == 0);
    checkEqLong("wrong size -> NOT_MFS", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_ERR_NOT_MFS);
    checkTrue("no handle on refusal", img == NULL);
    remove(HX_TEMP_PATH);
}

static void CaseZeroedPartitionRefused(void) {
    /* Exact .ram size but no "64dd-Multi" id: the size match alone must never be trusted. */
    int64_t size = hx_range_bytes(5, hxRamStartLba[5] - 0x18, HX_LBA_COUNT - (hxRamStartLba[5] - 0x18));
    uint8_t* buf = (uint8_t*)calloc(1, (size_t)size);
    GdxMfsImage* img = NULL;

    checkTrue("fixture alloc", buf != NULL);
    if (buf != NULL) {
        checkTrue("fixture written", hx_write_temp(buf, size) == 0);
        checkEqLong("zeroed .ram -> NOT_MFS", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_ERR_NOT_MFS);
        checkTrue("no handle on refusal", img == NULL);
        free(buf);
    }
    remove(HX_TEMP_PATH);
}

static void CaseFatCycleRefused(void) {
    HxSpec spec = { 5, 1, 1, 0, 0, 1, 0, 0 };
    HxFixture fx;
    GdxMfsImage* img = NULL;
    GdxMfsImageEntry entries[8];
    int count;
    int i;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);
    checkEqLong("open (header is valid)", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_OK);
    if (img != NULL) {
        count = gdx_mfsimg_list(img, entries, 8);
        checkEqLong("listed", count, 3);
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "TESTCRS") == 0) {
                int32_t cycleSize = (HX_FAT_COUNT + 2) * hx_block_bytes(5, (hxRamStartLba[5] - 0x18) + 6);
                uint8_t* buf = (uint8_t*)malloc((size_t)cycleSize);
                checkTrue("cycle buf alloc", buf != NULL);
                if (buf != NULL) {
                    checkEqLong("FAT cycle -> CORRUPT", gdx_mfsimg_read_file(img, entries[i].entryId, buf, cycleSize),
                                GDX_MFSIMG_ERR_CORRUPT);
                    free(buf);
                }
                break;
            }
        }
        gdx_mfsimg_close(img);
    }
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CaseChainEndsEarlyRefused(void) {
    HxSpec spec = { 5, 1, 1, 0, 0, 0, 1, 0 };
    HxFixture fx;
    GdxMfsImage* img = NULL;
    GdxMfsImageEntry entries[8];
    int count;
    int i;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);
    checkEqLong("open (header is valid)", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_OK);
    if (img != NULL) {
        count = gdx_mfsimg_list(img, entries, 8);
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "TESTCRS") == 0) {
                uint8_t* buf = (uint8_t*)malloc((size_t)fx.trackSize);
                if (buf != NULL) {
                    checkEqLong("LAST_BLOCK with bytes left -> CORRUPT",
                                gdx_mfsimg_read_file(img, entries[i].entryId, buf, fx.trackSize),
                                GDX_MFSIMG_ERR_CORRUPT);
                    free(buf);
                }
                break;
            }
        }
        gdx_mfsimg_close(img);
    }
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CaseFatIdOutOfRangeRefused(void) {
    HxSpec spec = { 5, 1, 1, 0, 0, 0, 0, 1 };
    HxFixture fx;
    GdxMfsImage* img = NULL;
    GdxMfsImageEntry entries[8];
    int count;
    int i;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    checkTrue("fixture written", hx_write_temp(fx.data, fx.size) == 0);
    checkEqLong("open (header is valid)", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_OK);
    if (img != NULL) {
        count = gdx_mfsimg_list(img, entries, 8);
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "TESTCRS") == 0) {
                uint8_t* buf = (uint8_t*)malloc((size_t)fx.trackSize);
                if (buf != NULL) {
                    checkEqLong("fatId past the disk -> CORRUPT",
                                gdx_mfsimg_read_file(img, entries[i].entryId, buf, fx.trackSize),
                                GDX_MFSIMG_ERR_CORRUPT);
                    free(buf);
                }
                break;
            }
        }
        gdx_mfsimg_close(img);
    }
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

/* ---------------------------------------------------------------------------------
 * Foreign-layout cases
 * ------------------------------------------------------------------------------- */

static int hx_cmp_int64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

/* Anchors the physical track/zone-start tables. Every physical LBA on every disk type must land
 * at a distinct, in-bounds offset; a single mistyped table entry collapses two blocks onto the
 * same address or runs off the end, and both show up here. */
static void CasePhysicalGeometryInjective(void) {
    uint8_t sysData[HX_SYS_DATA_BYTES];
    int64_t* offsets = (int64_t*)malloc(sizeof(int64_t) * HX_PHYS_LBA_COUNT);
    int t;

    memset(sysData, 0, sizeof(sysData));
    checkTrue("offset scratch alloc", offsets != NULL);
    if (offsets == NULL) {
        return;
    }
    for (t = 0; t < 6; t++) {
        int32_t lba;
        int collisions = 0;
        int outOfBounds = 0;
        for (lba = 0; lba < HX_PHYS_LBA_COUNT; lba++) {
            int32_t bytes = hx_phys_block_bytes(t, lba);
            int64_t off = hx_physical_offset(t, sysData, lba);
            offsets[lba] = off;
            if (off < 0 || bytes <= 0 || off + bytes > HX_MAME_TOTAL_BYTES) {
                outOfBounds++;
            }
        }
        qsort(offsets, HX_PHYS_LBA_COUNT, sizeof(int64_t), hx_cmp_int64);
        for (lba = 1; lba < HX_PHYS_LBA_COUNT; lba++) {
            if (offsets[lba] == offsets[lba - 1]) {
                collisions++;
            }
        }
        checkEqLong("physical offsets in bounds", outOfBounds, 0);
        checkEqLong("physical offsets distinct", collisions, 0);
    }
    free(offsets);
}

typedef struct HxSnapshot {
    int count;
    GdxMfsImageEntry entries[8];
    uint8_t* payloads[8];
} HxSnapshot;

static int hx_snapshot(const char* path, HxSnapshot* snap) {
    GdxMfsImage* img = NULL;
    int rc;
    int i;

    memset(snap, 0, sizeof(*snap));
    rc = gdx_mfsimg_open(path, &img);
    if (rc != GDX_MFSIMG_OK) {
        return rc;
    }
    snap->count = gdx_mfsimg_list(img, snap->entries, 8);
    for (i = 0; i < snap->count && i < 8; i++) {
        int32_t sz = snap->entries[i].fileSize;
        if (snap->entries[i].encoded || sz <= 0) {
            continue;
        }
        snap->payloads[i] = (uint8_t*)malloc((size_t)sz);
        if (snap->payloads[i] == NULL) {
            continue;
        }
        if (gdx_mfsimg_read_file(img, snap->entries[i].entryId, snap->payloads[i], sz) != GDX_MFSIMG_OK) {
            free(snap->payloads[i]);
            snap->payloads[i] = NULL;
        }
    }
    gdx_mfsimg_close(img);
    return GDX_MFSIMG_OK;
}

static void hx_free_snapshot(HxSnapshot* snap) {
    int i;
    for (i = 0; i < 8; i++) {
        free(snap->payloads[i]);
        snap->payloads[i] = NULL;
    }
}

/* Builds a synthesized .ndd, reads it, re-encodes the very same bytes into a foreign layout,
 * reads that, and requires the two readings to agree entry for entry and byte for byte. */
static void RunLayoutEquivalence(int diskType, int bigEndian, int asD64, int withDefects) {
    HxSpec spec;
    HxFixture fx;
    HxSnapshot base;
    HxSnapshot conv;
    uint8_t sysData[HX_SYS_DATA_BYTES];
    uint8_t* encoded = NULL;
    int64_t encodedSize = 0;
    int i;

    memset(&spec, 0, sizeof(spec));
    spec.diskType = diskType;
    spec.bigEndian = bigEndian;
    spec.isRawRam = 0;

    checkTrue("fixture built", hx_build_fixture(&spec, &fx) == 0);
    if (fx.data == NULL) {
        return;
    }

    memcpy(sysData, fx.data, HX_SYS_DATA_BYTES);
    if (withDefects) {
        /* One defective track in physical zone 0. The cumulative table must stay monotonic, so
         * every later zone repeats the running total. */
        int z;
        for (z = 0; z < 16; z++) {
            sysData[8 + z] = 1;
        }
        sysData[0x20] = 5; /* track 5 of zone 0 is bad; later tracks shift outward by one */
    }

    checkTrue("baseline .ndd written", hx_write_temp(fx.data, fx.size) == 0);
    checkEqLong("baseline .ndd opened", hx_snapshot(HX_TEMP_PATH, &base), GDX_MFSIMG_OK);
    remove(HX_TEMP_PATH);

    if (asD64) {
        encoded = hx_encode_d64(fx.data, fx.size, diskType, sysData, 100, &encodedSize);
    } else {
        encoded = hx_encode_physical(fx.data, fx.size, diskType, sysData, &encodedSize);
    }
    checkTrue("layout re-encoded", encoded != NULL);
    if (encoded == NULL) {
        hx_free_snapshot(&base);
        hx_free_fixture(&fx);
        return;
    }
    checkTrue("converted image written", hx_write_temp(encoded, encodedSize) == 0);

    {
        int rc = hx_snapshot(HX_TEMP_PATH, &conv);
        checkEqLong("converted image opened", rc, GDX_MFSIMG_OK);
        if (rc != GDX_MFSIMG_OK) {
            printf("      (strerror: %s)\n", gdx_mfsimg_strerror(rc));
            free(encoded);
            hx_free_snapshot(&base);
            hx_free_fixture(&fx);
            remove(HX_TEMP_PATH);
            return;
        }
    }

    checkEqLong("same listed file count", conv.count, base.count);
    checkEqLong("expected file count", base.count, 3);
    if (conv.count == base.count) {
        for (i = 0; i < base.count && i < 8; i++) {
            checkTrue("same name", strcmp(base.entries[i].name, conv.entries[i].name) == 0);
            checkTrue("same extension", strcmp(base.entries[i].extension, conv.entries[i].extension) == 0);
            checkEqLong("same file size", conv.entries[i].fileSize, base.entries[i].fileSize);
            checkEqLong("same content type", conv.entries[i].contentType, base.entries[i].contentType);
            checkEqLong("same encoded flag", conv.entries[i].encoded, base.entries[i].encoded);
            checkEqLong("same entry id", conv.entries[i].entryId, base.entries[i].entryId);
            if (base.payloads[i] != NULL) {
                checkTrue("converted payload present", conv.payloads[i] != NULL);
                if (conv.payloads[i] != NULL) {
                    checkTrue("payload byte-exact across layouts",
                              memcmp(base.payloads[i], conv.payloads[i], (size_t)base.entries[i].fileSize) == 0);
                }
            }
        }
    }

    free(encoded);
    hx_free_snapshot(&base);
    hx_free_snapshot(&conv);
    hx_free_fixture(&fx);
    remove(HX_TEMP_PATH);
}

static void CasePhysicalBE(void) {
    RunLayoutEquivalence(0, 1, 0, 0);
}

static void CasePhysicalLEType3(void) {
    RunLayoutEquivalence(3, 0, 0, 0);
}

static void CasePhysicalWithDefects(void) {
    RunLayoutEquivalence(0, 1, 0, 1);
}

static void CaseD64BE(void) {
    RunLayoutEquivalence(0, 1, 1, 0);
}

static void CaseD64LEType2(void) {
    RunLayoutEquivalence(2, 0, 1, 0);
}

/* A file the right size for the physical layout but with no volume in it must refuse, proving
 * the size match alone never counts as recognition. */
static void CaseZeroedPhysicalRefused(void) {
    uint8_t* buf = (uint8_t*)calloc(1, HX_MAME_TOTAL_BYTES);
    GdxMfsImage* img = NULL;

    checkTrue("zeroed physical buffer alloc", buf != NULL);
    if (buf == NULL) {
        return;
    }
    checkTrue("zeroed physical written", hx_write_temp(buf, HX_MAME_TOTAL_BYTES) == 0);
    checkEqLong("zeroed .disk refused", gdx_mfsimg_open(HX_TEMP_PATH, &img), GDX_MFSIMG_ERR_NOT_MFS);
    checkTrue("no handle leaked", img == NULL);
    free(buf);
    remove(HX_TEMP_PATH);
}

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } cases[] = {
        { "zone-math known answers (LEORAM_BYTE, full .ndd size)", CaseZoneMathKnownAnswers },
        { "valid .ndd, big-endian (console/emulator)", CaseValidNddBE },
        { "valid .ndd, little-endian (port-written)", CaseValidNddLE },
        { "valid .ram, big-endian, disk type 5", CaseValidRamBE },
        { "valid .ram, little-endian, disk type 2", CaseValidRamLEType2 },
        { "corrupt primary header falls back to backup", CaseBackupFallback },
        { "both headers corrupt -> refused", CaseCorruptBothHeadersRefused },
        { "wrong-size file -> refused", CaseBadSizeRefused },
        { "zeroed .ram-size file -> refused", CaseZeroedPartitionRefused },
        { "FAT cycle -> CORRUPT (no hang)", CaseFatCycleRefused },
        { "chain ends early -> CORRUPT", CaseChainEndsEarlyRefused },
        { "fatId past the disk -> CORRUPT", CaseFatIdOutOfRangeRefused },
        { "physical geometry: offsets distinct and in bounds", CasePhysicalGeometryInjective },
        { "MAME/ares .disk == .ndd, big-endian", CasePhysicalBE },
        { "MAME/ares .disk == .ndd, little-endian, disk type 3", CasePhysicalLEType3 },
        { "MAME/ares .disk with a defective track == .ndd", CasePhysicalWithDefects },
        { "D64 == .ndd, big-endian", CaseD64BE },
        { "D64 == .ndd, little-endian, disk type 2", CaseD64LEType2 },
        { "zeroed .disk-size file -> refused", CaseZeroedPhysicalRefused },
    };
    int numCases = (int)(sizeof(cases) / sizeof(cases[0]));
    int i, failedCases = 0;

    printf("=== G-Diffuser MFS-image reader unit-test harness (port/gdx_mfs_image.c) ===\n\n");
    for (i = 0; i < numCases; i++) {
        int failsBefore = gSubFails;
        int checksBefore = gSubChecks;
        printf("-- %s\n", cases[i].name);
        cases[i].fn();
        if (gSubFails > failsBefore) {
            failedCases++;
            printf("[FAIL] %s (%d/%d sub-checks failed)\n\n", cases[i].name, gSubFails - failsBefore,
                   gSubChecks - checksBefore);
        } else {
            printf("[PASS] %s (%d sub-checks)\n\n", cases[i].name, gSubChecks - checksBefore);
        }
    }
    printf("=== Summary: %d/%d cases passed (%d/%d sub-checks passed) ===\n", numCases - failedCases, numCases,
           gSubChecks - gSubFails, gSubChecks);
    return failedCases ? 1 : 0;
}
