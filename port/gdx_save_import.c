// Converts an emulator SRAM save into G-Diffuser's native fzerox.sav image. Pure C: no file I/O,
// no game state, and no decomp headers.
#include "gdx_save_import.h"

#include <stdint.h>
#include <string.h>

// Offsets restate decomp/include/fzx_save.h so this file builds without decomp headers. Change them
// together with that header. gdx_save_import_layout_is_consistent catches gaps and overlaps, not an
// offset that moved while still tiling the image.
#define GDX_SAVE_IMPORT_PROFILE_COUNT 2
#define GDX_SAVE_IMPORT_PROFILE_SIZE 0x19E0
#define GDX_SAVE_IMPORT_SETTINGS_OFFSET 0x00
#define GDX_SAVE_IMPORT_SETTINGS_SIZE 0x10
#define GDX_SAVE_IMPORT_DEATH_RACE_OFFSET 0x10
#define GDX_SAVE_IMPORT_DEATH_RACE_SIZE 0x10
#define GDX_SAVE_IMPORT_COURSE_OFFSET 0x20
#define GDX_SAVE_IMPORT_COURSE_COUNT 24
#define GDX_SAVE_IMPORT_COURSE_SIZE 0x110
#define GDX_SAVE_IMPORT_UNION_OFFSET 0x19A0
#define GDX_SAVE_IMPORT_UNION_SIZE 0x40
#define GDX_SAVE_IMPORT_GHOST_OFFSET 0x33C0
#define GDX_SAVE_IMPORT_GHOST_RECORD_SIZE 0x40
#define GDX_SAVE_IMPORT_GHOST_DATA_OFFSET 0x3400
#define GDX_SAVE_IMPORT_GHOST_DATA_SIZE 0x3F80
#define GDX_SAVE_IMPORT_GHOST_SIZE 0x3FC0
#define GDX_SAVE_IMPORT_CHARACTER_OFFSET 0x7380
#define GDX_SAVE_IMPORT_CHARACTER_COUNT 24
#define GDX_SAVE_IMPORT_CHARACTER_SIZE 0x80
#define GDX_SAVE_IMPORT_CUP_OFFSET 0x7F80
#define GDX_SAVE_IMPORT_CUP_SIZE 0x60
#define GDX_SAVE_IMPORT_UNK_OFFSET 0x7FE0
#define GDX_SAVE_IMPORT_UNK_SIZE 0x20

#define GDX_SAVE_IMPORT_COUNT(array) ((unsigned long) (sizeof(array) / sizeof((array)[0])))

// Each typedef becomes a negative-length array when its condition fails. A constant that drifts
// away from the sum of its parts therefore stops the build instead of mis-sizing the table.
typedef char gdxSaveImportProfileLayout[
    (GDX_SAVE_IMPORT_SETTINGS_OFFSET + GDX_SAVE_IMPORT_SETTINGS_SIZE == GDX_SAVE_IMPORT_DEATH_RACE_OFFSET &&
     GDX_SAVE_IMPORT_DEATH_RACE_OFFSET + GDX_SAVE_IMPORT_DEATH_RACE_SIZE == GDX_SAVE_IMPORT_COURSE_OFFSET &&
     GDX_SAVE_IMPORT_COURSE_OFFSET + GDX_SAVE_IMPORT_COURSE_COUNT * GDX_SAVE_IMPORT_COURSE_SIZE ==
         GDX_SAVE_IMPORT_UNION_OFFSET &&
     GDX_SAVE_IMPORT_UNION_OFFSET + GDX_SAVE_IMPORT_UNION_SIZE == GDX_SAVE_IMPORT_PROFILE_SIZE) ? 1 : -1];

typedef char gdxSaveImportGhostLayout[
    (GDX_SAVE_IMPORT_GHOST_RECORD_SIZE == GDX_SAVE_IMPORT_GHOST_DATA_OFFSET - GDX_SAVE_IMPORT_GHOST_OFFSET &&
     GDX_SAVE_IMPORT_GHOST_RECORD_SIZE + GDX_SAVE_IMPORT_GHOST_DATA_SIZE == GDX_SAVE_IMPORT_GHOST_SIZE) ? 1 : -1];

typedef char gdxSaveImportContextLayout[
    (GDX_SAVE_IMPORT_PROFILE_COUNT * GDX_SAVE_IMPORT_PROFILE_SIZE == GDX_SAVE_IMPORT_GHOST_OFFSET &&
     GDX_SAVE_IMPORT_GHOST_OFFSET + GDX_SAVE_IMPORT_GHOST_SIZE == GDX_SAVE_IMPORT_CHARACTER_OFFSET &&
     GDX_SAVE_IMPORT_CHARACTER_OFFSET + GDX_SAVE_IMPORT_CHARACTER_COUNT * GDX_SAVE_IMPORT_CHARACTER_SIZE ==
         GDX_SAVE_IMPORT_CUP_OFFSET &&
     GDX_SAVE_IMPORT_CUP_OFFSET + GDX_SAVE_IMPORT_CUP_SIZE == GDX_SAVE_IMPORT_UNK_OFFSET &&
     GDX_SAVE_IMPORT_UNK_OFFSET + GDX_SAVE_IMPORT_UNK_SIZE == GDX_SAVE_IMPORT_SRAM_SIZE) ? 1 : -1];

// Field layout of each struct. Scalars are runs of equally sized elements, and a run is
// byte-swapped element by element when converting from big-endian. Byte arrays are 1-byte runs,
// merged where adjacent, and are never swapped. MachineInfo is all single bytes. The replay data
// stays raw because save.c's GhostData_FromRom notes that swapping it would corrupt playback.
#define GDX_SAVE_IMPORT_RUN(offset, count, elemSize) \
    { GDX_SAVE_IMPORT_OP_RUN, (offset), (count), (elemSize), 0, NULL, 0 }
#define GDX_SAVE_IMPORT_REPEAT(offset, count, stride, sub) \
    { GDX_SAVE_IMPORT_OP_REPEAT, (offset), (count), 0, (stride), (sub), GDX_SAVE_IMPORT_COUNT(sub) }

// SaveSettings (0x10): fileName[8], settings, customUnlocks and cupDifficultiesCleared[4] are bytes.
// The checksum is the final u16.
static const GdxSaveImportOp sSettingsOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 0x0E, 1),
    GDX_SAVE_IMPORT_RUN(0x0E, 1, 2),
};

// SaveDeathRace (0x10): checksum and unk_02 are u16/s16, timeRecord[1] is s32, unk_08 is bytes.
static const GdxSaveImportOp sDeathRaceOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 1, 4),
    GDX_SAVE_IMPORT_RUN(0x08, 8, 1),
};

// SaveCourseRecords (0x110): checksum and unk_02 are u16/s16. timeRecord[5], engines[5] (f32),
// maxSpeed (f32) and bestTime are 4-byte values. name, unk_48, unk_50[5] and unk_F0 are bytes.
static const GdxSaveImportOp sCourseOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 12, 4),
    GDX_SAVE_IMPORT_RUN(0x34, 0xDC, 1),
};

// The profile union is read as ddCups under EXPANSION_KIT, which the port builds with: a u16
// staffGhostCompletion, then cupCompletion bytes. Emulator imports clear it afterwards; see
// clearDDCups.
static const GdxSaveImportOp sDDCupsOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 0x3C, 1),
};

// One ProfileSave copy (0x19E0). Each struct is placed with REPEAT, so its stride (its size) is
// checked against the fields it contains.
static const GdxSaveImportOp sProfileOps[] = {
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_SETTINGS_OFFSET, 1, GDX_SAVE_IMPORT_SETTINGS_SIZE,
                           sSettingsOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_DEATH_RACE_OFFSET, 1, GDX_SAVE_IMPORT_DEATH_RACE_SIZE,
                           sDeathRaceOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_COURSE_OFFSET, GDX_SAVE_IMPORT_COURSE_COUNT,
                           GDX_SAVE_IMPORT_COURSE_SIZE, sCourseOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_UNION_OFFSET, 1, GDX_SAVE_IMPORT_UNION_SIZE,
                           sDDCupsOps),
};

// GhostRecord (0x40): checksum, ghostType, replayChecksum, encodedCourseIndex, raceTime and unk_10
// are scalars. unk_12[5], trackName[9] and unk_20 are bytes.
static const GdxSaveImportOp sGhostRecordOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 3, 4),
    GDX_SAVE_IMPORT_RUN(0x10, 1, 2),
    GDX_SAVE_IMPORT_RUN(0x12, 0x2E, 1),
};

// GhostReplayInfo (0x20): checksum and unk_02 are u16/s16. lapTimes[3], end, size, unk_18 and
// unk_1C are 4-byte values.
static const GdxSaveImportOp sReplayInfoOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 7, 4),
};

// GhostData (0x3F80): the replay header, then replayData[16200] and unk_3F68[24] as raw bytes.
static const GdxSaveImportOp sGhostDataOps[] = {
    GDX_SAVE_IMPORT_REPEAT(0x00, 1, 0x20, sReplayInfoOps),
    GDX_SAVE_IMPORT_RUN(0x20, 0x3F60, 1),
};

// GhostSave (0x3FC0): one GhostRecord, then one GhostData.
static const GdxSaveImportOp sGhostSaveOps[] = {
    GDX_SAVE_IMPORT_REPEAT(0x00, 1, GDX_SAVE_IMPORT_GHOST_RECORD_SIZE, sGhostRecordOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_GHOST_RECORD_SIZE, 1, GDX_SAVE_IMPORT_GHOST_DATA_SIZE,
                           sGhostDataOps),
};

// CharacterSave (0x80): checksum and unk_02 are u16. unk_04 and characterEngine[30] are 4-byte.
static const GdxSaveImportOp sCharacterOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 2, 2),
    GDX_SAVE_IMPORT_RUN(0x04, 31, 4),
};

// CupSave (0x60): checksum is u16, unk_02[14] is bytes, cupCompletion[4][10] is u16.
static const GdxSaveImportOp sCupOps[] = {
    GDX_SAVE_IMPORT_RUN(0x00, 1, 2),
    GDX_SAVE_IMPORT_RUN(0x02, 0x0E, 1),
    GDX_SAVE_IMPORT_RUN(0x10, 40, 2),
};

// The whole SaveContext image, in order. The table must tile all 0x8000 bytes exactly.
static const GdxSaveImportOp sLayout[] = {
    GDX_SAVE_IMPORT_REPEAT(0x0000, GDX_SAVE_IMPORT_PROFILE_COUNT, GDX_SAVE_IMPORT_PROFILE_SIZE,
                           sProfileOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_GHOST_OFFSET, 1, GDX_SAVE_IMPORT_GHOST_SIZE,
                           sGhostSaveOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_CHARACTER_OFFSET, GDX_SAVE_IMPORT_CHARACTER_COUNT,
                           GDX_SAVE_IMPORT_CHARACTER_SIZE, sCharacterOps),
    GDX_SAVE_IMPORT_REPEAT(GDX_SAVE_IMPORT_CUP_OFFSET, 1, GDX_SAVE_IMPORT_CUP_SIZE, sCupOps),
    GDX_SAVE_IMPORT_RUN(GDX_SAVE_IMPORT_UNK_OFFSET, GDX_SAVE_IMPORT_UNK_SIZE, 1),
};

// A checksum unit is one stored u16 and the bytes its sum covers. The ranges follow the game's
// Save_Calculate*Checksum functions in save.c, which sum from just after the checksum field to the
// end of the struct. SETTINGS is the exception: its checksum is the last u16 of the struct, so its
// sum covers the bytes before it.
typedef struct GdxSaveImportUnit {
    unsigned long offset;
    unsigned long count;
    unsigned long stride;
    unsigned long checksumAt;
    unsigned long sumFrom;
    unsigned long sumTo;
    unsigned int section;
} GdxSaveImportUnit;

static const GdxSaveImportUnit sUnits[] = {
    { GDX_SAVE_IMPORT_SETTINGS_OFFSET, GDX_SAVE_IMPORT_PROFILE_COUNT, GDX_SAVE_IMPORT_PROFILE_SIZE,
      0x0E, 0x00, 0x0E, GDX_SAVE_IMPORT_SECTION_SETTINGS },
    { GDX_SAVE_IMPORT_DEATH_RACE_OFFSET, GDX_SAVE_IMPORT_PROFILE_COUNT, GDX_SAVE_IMPORT_PROFILE_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_DEATH_RACE_SIZE, GDX_SAVE_IMPORT_SECTION_DEATH_RACE },
    { GDX_SAVE_IMPORT_COURSE_OFFSET, GDX_SAVE_IMPORT_COURSE_COUNT, GDX_SAVE_IMPORT_COURSE_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_COURSE_SIZE, GDX_SAVE_IMPORT_SECTION_COURSES },
    { GDX_SAVE_IMPORT_COURSE_OFFSET + GDX_SAVE_IMPORT_PROFILE_SIZE, GDX_SAVE_IMPORT_COURSE_COUNT,
      GDX_SAVE_IMPORT_COURSE_SIZE, 0x00, 0x02, GDX_SAVE_IMPORT_COURSE_SIZE,
      GDX_SAVE_IMPORT_SECTION_COURSES },
    { GDX_SAVE_IMPORT_UNION_OFFSET, GDX_SAVE_IMPORT_PROFILE_COUNT, GDX_SAVE_IMPORT_PROFILE_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_UNION_SIZE, GDX_SAVE_IMPORT_SECTION_DD_CUPS },
    { GDX_SAVE_IMPORT_GHOST_OFFSET, 1, GDX_SAVE_IMPORT_GHOST_RECORD_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_GHOST_RECORD_SIZE, GDX_SAVE_IMPORT_SECTION_GHOST_RECORD },
    { GDX_SAVE_IMPORT_GHOST_DATA_OFFSET, 1, GDX_SAVE_IMPORT_GHOST_DATA_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_GHOST_DATA_SIZE, GDX_SAVE_IMPORT_SECTION_GHOST_DATA },
    { GDX_SAVE_IMPORT_CHARACTER_OFFSET, GDX_SAVE_IMPORT_CHARACTER_COUNT,
      GDX_SAVE_IMPORT_CHARACTER_SIZE, 0x00, 0x02, GDX_SAVE_IMPORT_CHARACTER_SIZE,
      GDX_SAVE_IMPORT_SECTION_CHARACTERS },
    { GDX_SAVE_IMPORT_CUP_OFFSET, 1, GDX_SAVE_IMPORT_CUP_SIZE,
      0x00, 0x02, GDX_SAVE_IMPORT_CUP_SIZE, GDX_SAVE_IMPORT_SECTION_CUP },
};

// The game writes "F-ZERO X" into both profile copies when it creates a save (save.c,
// Save_InitSaveSettings and Save_SaveSettings). An all-zero image passes every checksum by accident
// (0 == 0), so this name is what rejects blank SRAM. The game's own load path checks only the
// checksums, so this rule is stricter than the load path, on purpose.
static const unsigned char kSettingsName[8] = { 'F', '-', 'Z', 'E', 'R', 'O', ' ', 'X' };

// Replaces a big-endian u16 in place with the host-order value. The value is built with shifts and
// written with memcpy, so the result is correct on little- and big-endian hosts alike.
static void swapBigEndian16(unsigned char* p) {
    uint16_t value = (uint16_t) (((unsigned int) p[0] << 8) | p[1]);

    memcpy(p, &value, sizeof(value));
}

static void swapBigEndian32(unsigned char* p) {
    uint32_t value = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                     ((uint32_t) p[2] << 8) | (uint32_t) p[3];

    memcpy(p, &value, sizeof(value));
}

static void convertBigEndian(unsigned char* image, const GdxSaveImportOp* ops, unsigned long opCount,
                             unsigned long base) {
    unsigned long i;
    unsigned long k;

    for (i = 0; i < opCount; i++) {
        const GdxSaveImportOp* op = &ops[i];

        if (op->kind == GDX_SAVE_IMPORT_OP_REPEAT) {
            for (k = 0; k < op->count; k++) {
                convertBigEndian(image, op->sub, op->subCount, base + op->offset + k * op->stride);
            }
        } else if (op->elemSize == 2) {
            for (k = 0; k < op->count; k++) {
                swapBigEndian16(image + base + op->offset + k * op->elemSize);
            }
        } else if (op->elemSize == 4) {
            for (k = 0; k < op->count; k++) {
                swapBigEndian32(image + base + op->offset + k * op->elemSize);
            }
        }
    }
}

// Undoes the word-swapped layout, where every 32-bit word is stored little-endian. Reversing the
// bytes of each word restores the big-endian image, including the byte arrays, which the later
// scalar pass then leaves alone.
static void reverseWords(unsigned char* image, unsigned long size) {
    unsigned long i;

    for (i = 0; i + 4 <= size; i += 4) {
        unsigned char b0 = image[i];
        unsigned char b1 = image[i + 1];

        image[i] = image[i + 3];
        image[i + 1] = image[i + 2];
        image[i + 2] = b1;
        image[i + 3] = b0;
    }
}

static int unitChecksumMatches(const unsigned char* instance, const GdxSaveImportUnit* unit) {
    unsigned long sum = 0;
    unsigned long i;
    uint16_t stored;

    // The game sums unsigned bytes into a u16. Truncating to 16 bits below matches that exactly.
    for (i = unit->sumFrom; i < unit->sumTo; i++) {
        sum += instance[i];
    }

    // The stored field is read in host order on purpose. A native image holds host-order fields,
    // so a field in the wrong byte order fails here. That failure is how byte order is detected.
    memcpy(&stored, instance + unit->checksumAt, sizeof(stored));
    return stored == (uint16_t) (sum & 0xFFFFu);
}

static int settingsNamesMatch(const unsigned char* image) {
    unsigned long copy;

    for (copy = 0; copy < GDX_SAVE_IMPORT_PROFILE_COUNT; copy++) {
        const unsigned char* name =
            image + copy * GDX_SAVE_IMPORT_PROFILE_SIZE + GDX_SAVE_IMPORT_SETTINGS_OFFSET;

        if (memcmp(name, kSettingsName, sizeof(kSettingsName)) != 0) {
            return 0;
        }
    }
    return 1;
}

// Returns the mask of sections in which every instance passes. Strict on purpose: the game repairs
// a single bad settings copy in place and writes the repair back, so an import should not guess
// which copy is right.
static unsigned int validateSections(const unsigned char* image) {
    unsigned int valid = GDX_SAVE_IMPORT_SECTION_ALL;
    unsigned long u;
    unsigned long k;

    for (u = 0; u < GDX_SAVE_IMPORT_COUNT(sUnits); u++) {
        const GdxSaveImportUnit* unit = &sUnits[u];

        for (k = 0; k < unit->count; k++) {
            if (!unitChecksumMatches(image + unit->offset + k * unit->stride, unit)) {
                valid &= ~unit->section;
            }
        }
    }
    if (!settingsNamesMatch(image)) {
        valid &= ~GDX_SAVE_IMPORT_SECTION_SETTINGS;
    }
    return valid;
}

// Emulator saves come from the cartridge game, where the profile union is SaveEditCup. Its Edit Cup
// track names sit where the Expansion Kit reads ddCups.cupCompletion, so importing them raw would
// grant random DD cup clears. The import scope is cartridge saves only, with no 64DD saves, so an
// emulator import starts with empty DD cups: the state Save_InitDDCups(..., true) leaves. Zero bytes
// sum to zero, so the zeroed checksum stays valid. Nothing of the cartridge's Edit Cup survives,
// because the Expansion Kit has no Edit Cup to show it in.
static void clearDDCups(unsigned char* image) {
    unsigned long copy;

    for (copy = 0; copy < GDX_SAVE_IMPORT_PROFILE_COUNT; copy++) {
        memset(image + copy * GDX_SAVE_IMPORT_PROFILE_SIZE + GDX_SAVE_IMPORT_UNION_OFFSET, 0,
               GDX_SAVE_IMPORT_UNION_SIZE);
    }
}

static unsigned int countSections(unsigned int mask) {
    unsigned int count = 0;

    while (mask != 0) {
        count += mask & 1u;
        mask >>= 1;
    }
    return count;
}

typedef enum GdxSaveImportCandidate {
    CANDIDATE_NATIVE,
    CANDIDATE_BIG_ENDIAN,
    CANDIDATE_WORD_SWAPPED
} GdxSaveImportCandidate;

typedef struct GdxSaveImportAttempt {
    unsigned char* scratch;
    unsigned char* out;
    GdxSaveImportInfo* info;
    unsigned int bestMask;
} GdxSaveImportAttempt;

// Works on a scratch copy so that in and out may alias, and so that a failed candidate never
// touches out.
static void prepareScratch(unsigned char* scratch, const unsigned char* src, GdxSaveImportCandidate candidate) {
    memcpy(scratch, src, GDX_SAVE_IMPORT_SRAM_SIZE);
    if (candidate == CANDIDATE_WORD_SWAPPED) {
        reverseWords(scratch, GDX_SAVE_IMPORT_SRAM_SIZE);
    }
    if (candidate != CANDIDATE_NATIVE) {
        convertBigEndian(scratch, sLayout, GDX_SAVE_IMPORT_COUNT(sLayout), 0);
    }
}

// Returns 1 when this candidate validates in full, after copying the result to out. Otherwise it
// keeps the candidate with the most valid sections, which is for diagnostics only.
static int tryCandidate(GdxSaveImportAttempt* attempt, const unsigned char* src,
                        GdxSaveImportCandidate candidate, GdxSaveImportFormat format) {
    unsigned int mask;

    prepareScratch(attempt->scratch, src, candidate);
    mask = validateSections(attempt->scratch);
    if (mask == GDX_SAVE_IMPORT_SECTION_ALL) {
        if (candidate != CANDIDATE_NATIVE) {
            clearDDCups(attempt->scratch);
        }
        memcpy(attempt->out, attempt->scratch, GDX_SAVE_IMPORT_SRAM_SIZE);
        attempt->info->format = format;
        attempt->info->validSections = mask;
        return 1;
    }
    if (countSections(mask) > countSections(attempt->bestMask)) {
        attempt->bestMask = mask;
    }
    return 0;
}

int gdx_save_import_convert(const unsigned char* in, unsigned long inSize, unsigned char* out,
                            GdxSaveImportInfo* info) {
    unsigned char scratch[GDX_SAVE_IMPORT_SRAM_SIZE];
    GdxSaveImportInfo localInfo;
    GdxSaveImportAttempt attempt;

    if (info == NULL) {
        info = &localInfo;
    }
    info->format = GDX_SAVE_IMPORT_FORMAT_NONE;
    info->validSections = 0;
    if (in == NULL || out == NULL) {
        return 0;
    }

    attempt.scratch = scratch;
    attempt.out = out;
    attempt.info = info;
    attempt.bestMask = 0;

    // Native is tried first because a host-order dump is the common case. A wrong byte order fails
    // the stored checksums, so the first candidate that validates is the right one.
    if (inSize == GDX_SAVE_IMPORT_SRAM_SIZE) {
        if (tryCandidate(&attempt, in, CANDIDATE_NATIVE, GDX_SAVE_IMPORT_FORMAT_NATIVE) ||
            tryCandidate(&attempt, in, CANDIDATE_BIG_ENDIAN, GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN) ||
            tryCandidate(&attempt, in, CANDIDATE_WORD_SWAPPED,
                         GDX_SAVE_IMPORT_FORMAT_SRA_WORD_SWAPPED)) {
            return 1;
        }
    } else if (inSize == GDX_SAVE_IMPORT_SRM_SIZE) {
        const unsigned char* block = in + GDX_SAVE_IMPORT_SRM_BLOCK_OFFSET;

        if (tryCandidate(&attempt, block, CANDIDATE_BIG_ENDIAN, GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL) ||
            tryCandidate(&attempt, block, CANDIDATE_WORD_SWAPPED,
                         GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL)) {
            return 1;
        }
    } else {
        return 0;
    }

    info->validSections = attempt.bestMask;
    return 0;
}

// Walks ops with a running cursor. Each op must start where the previous one ended, which catches
// gaps and overlaps. A REPEAT's sub-table must fill exactly one stride, which catches a stride
// that is too large or too small.
static int coverOps(const GdxSaveImportOp* ops, unsigned long opCount, unsigned long* cursor) {
    unsigned long i;

    for (i = 0; i < opCount; i++) {
        const GdxSaveImportOp* op = &ops[i];

        if (op->count == 0 || op->offset != *cursor) {
            return 0;
        }
        if (op->kind == GDX_SAVE_IMPORT_OP_RUN) {
            if (op->elemSize != 1 && op->elemSize != 2 && op->elemSize != 4) {
                return 0;
            }
            *cursor += op->count * op->elemSize;
        } else if (op->kind == GDX_SAVE_IMPORT_OP_REPEAT) {
            unsigned long subCursor = 0;

            if (op->stride == 0 || op->sub == NULL || op->subCount == 0) {
                return 0;
            }
            if (!coverOps(op->sub, op->subCount, &subCursor) || subCursor != op->stride) {
                return 0;
            }
            *cursor += op->count * op->stride;
        } else {
            return 0;
        }
    }
    return 1;
}

int gdx_save_import_check_layout(const GdxSaveImportOp* ops, unsigned long opCount,
                                 unsigned long totalSize) {
    unsigned long cursor = 0;

    if (ops == NULL || opCount == 0) {
        return 0;
    }
    if (!coverOps(ops, opCount, &cursor)) {
        return 0;
    }
    return cursor == totalSize;
}

int gdx_save_import_layout_is_consistent(void) {
    return gdx_save_import_check_layout(sLayout, GDX_SAVE_IMPORT_COUNT(sLayout),
                                        GDX_SAVE_IMPORT_SRAM_SIZE);
}
