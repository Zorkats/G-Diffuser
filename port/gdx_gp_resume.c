/* port/gdx_gp_resume.c -- Save GP in progress: codec and file operations. See gdx_gp_resume.h.
 * Pure C. Field values are copied in and out by the PORT glue in decomp/src/game/game.c.
 */

#define _CRT_SECURE_NO_WARNINGS /* fopen/remove below; harmless on non-MSVC */

#include "gdx_gp_resume.h"
#include "gdx_course_registry.h"

/* game.c sees the record fields as int (GDX_GP_RESUME_STATE_ONLY); this TU sees int32_t. */
typedef char GdxGpIntIs32Bits[(sizeof(int) == 4 && sizeof(int32_t) == 4) ? 1 : -1];

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* Byte offsets of the version 1 record. The table in gdx_gp_resume.h is the reference. */
#define OFF_CUP 12u
#define OFF_DIFFICULTY 16u
#define OFF_NUM_PLAYERS 20u
#define OFF_COURSE_INDEX 24u
#define OFF_LAST_RANDOM 28u
#define OFF_RACE_COUNTER 32u
#define OFF_RAND_SEED1 36u
#define OFF_RAND_MASK1 40u
#define OFF_RAND_SEED2 44u
#define OFF_RAND_MASK2 48u
#define OFF_LIVES 52u
#define OFF_PLAYER_CHARACTER 68u
#define OFF_PLAYER_SKIN 84u
#define OFF_RACERS 100u
#define OFF_ENGINE 460u
#define OFF_CRC 580u
#define RACER_STRIDE 12u

/* ---------------------------------------------------------------------------------
 * Little-endian primitives.
 * ------------------------------------------------------------------------------- */

static void put_u32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void put_s32(unsigned char* p, int32_t v) {
    put_u32(p, (uint32_t) v);
}

/* Two's-complement read without relying on signed overflow or implementation-defined casts. */
static int32_t get_s32(const unsigned char* p) {
    uint32_t u = get_u32(p);
    if (u <= 0x7FFFFFFFu) {
        return (int32_t) u;
    }
    return -(int32_t) (~u) - 1;
}

static void put_f32(unsigned char* p, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const unsigned char* p) {
    uint32_t bits = get_u32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static int in_range(int32_t value, int32_t low, int32_t high) {
    return value >= low && value <= high;
}

/* ---------------------------------------------------------------------------------
 * Validation.
 * ------------------------------------------------------------------------------- */

GdxGpResumeError gdx_gp_resume_validate(const GdxGpResumeState* s) {
    int i;

    if (s == NULL) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    if (!in_range(s->cup, 0, GDX_GP_RESUME_CUP_X)) {
        return GDX_GP_RESUME_ERR_RANGE;
    }
    if (!in_range(s->difficulty, 0, GDX_GP_RESUME_DIFFICULTY_COUNT - 1)) {
        return GDX_GP_RESUME_ERR_RANGE;
    }
    if (!in_range(s->numPlayers, 1, GDX_GP_RESUME_PLAYERS)) {
        return GDX_GP_RESUME_ERR_RANGE;
    }
    /* X Cup courses are 48..53. Stock cups are 0..23, with course / 6 naming the cup. */
    if (s->cup == GDX_GP_RESUME_CUP_X) {
        if (!in_range(s->courseIndex, GDX_GP_RESUME_COURSE_X_FIRST, GDX_GP_RESUME_COURSE_X_LAST)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
    } else {
        if (!in_range(s->courseIndex, 0, GDX_GP_RESUME_CUP_X * GDX_GP_RESUME_COURSES_PER_CUP - 1)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        if (s->courseIndex / GDX_GP_RESUME_COURSES_PER_CUP != s->cup) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
    }
    if (!in_range(s->lastRandomCourseIndex, -1, GDX_GP_RESUME_COURSE_X_LAST)) {
        return GDX_GP_RESUME_ERR_RANGE;
    }
    if (!in_range(s->raceCounter, 1, GDX_GP_RESUME_MAX_RACE_COUNTER)) {
        return GDX_GP_RESUME_ERR_RANGE;
    }
    for (i = 0; i < GDX_GP_RESUME_PLAYERS; i++) {
        if (!in_range(s->lives[i], 0, GDX_GP_RESUME_MAX_LIVES)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        if (!in_range(s->playerCharacter[i], 0, GDX_GP_RESUME_MACHINE_COUNT - 1)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        if (!in_range(s->playerSkin[i], 0, GDX_GP_RESUME_SKIN_COUNT - 1)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
    }
    for (i = 0; i < GDX_GP_RESUME_RACERS; i++) {
        if (!in_range(s->racers[i].points, 0, GDX_GP_RESUME_MAX_POINTS)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        if (!in_range(s->racers[i].character, 0, GDX_GP_RESUME_MACHINE_COUNT - 1)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        if (!in_range(s->racers[i].skin, 0, GDX_GP_RESUME_SKIN_COUNT - 1)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
        /* Written as a positive test so that NaN is rejected too. */
        if (!(s->engine[i] >= 0.0f && s->engine[i] <= 1.0f)) {
            return GDX_GP_RESUME_ERR_RANGE;
        }
    }
    return GDX_GP_RESUME_OK;
}

/* ---------------------------------------------------------------------------------
 * Codec.
 * ------------------------------------------------------------------------------- */

GdxGpResumeError gdx_gp_resume_encode(const GdxGpResumeState* s, unsigned char* out, size_t outCap) {
    GdxGpResumeError error;
    int i;

    if (s == NULL || out == NULL || outCap < GDX_GP_RESUME_FILE_SIZE) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    error = gdx_gp_resume_validate(s);
    if (error != GDX_GP_RESUME_OK) {
        return error;
    }

    memcpy(out, GDX_GP_RESUME_MAGIC, 4);
    put_u32(out + 4, GDX_GP_RESUME_VERSION);
    put_u32(out + 8, GDX_GP_RESUME_FILE_SIZE);

    put_s32(out + OFF_CUP, s->cup);
    put_s32(out + OFF_DIFFICULTY, s->difficulty);
    put_s32(out + OFF_NUM_PLAYERS, s->numPlayers);
    put_s32(out + OFF_COURSE_INDEX, s->courseIndex);
    put_s32(out + OFF_LAST_RANDOM, s->lastRandomCourseIndex);
    put_s32(out + OFF_RACE_COUNTER, s->raceCounter);
    put_s32(out + OFF_RAND_SEED1, s->randSeed1);
    put_u32(out + OFF_RAND_MASK1, s->randMask1);
    put_s32(out + OFF_RAND_SEED2, s->randSeed2);
    put_u32(out + OFF_RAND_MASK2, s->randMask2);

    for (i = 0; i < GDX_GP_RESUME_PLAYERS; i++) {
        put_s32(out + OFF_LIVES + 4u * (unsigned) i, s->lives[i]);
        put_s32(out + OFF_PLAYER_CHARACTER + 4u * (unsigned) i, s->playerCharacter[i]);
        put_s32(out + OFF_PLAYER_SKIN + 4u * (unsigned) i, s->playerSkin[i]);
    }
    for (i = 0; i < GDX_GP_RESUME_RACERS; i++) {
        unsigned char* racer = out + OFF_RACERS + RACER_STRIDE * (unsigned) i;
        put_s32(racer + 0, s->racers[i].points);
        put_s32(racer + 4, s->racers[i].character);
        put_s32(racer + 8, s->racers[i].skin);
        put_f32(out + OFF_ENGINE + 4u * (unsigned) i, s->engine[i]);
    }

    put_u32(out + OFF_CRC, gdx_course_registry_crc32(out, OFF_CRC));
    return GDX_GP_RESUME_OK;
}

GdxGpResumeError gdx_gp_resume_decode(const unsigned char* data, size_t length, GdxGpResumeState* out) {
    GdxGpResumeState parsed;
    GdxGpResumeError error;
    uint32_t declared;
    uint32_t stored;
    int i;

    if (data == NULL || out == NULL) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    if (length < GDX_GP_RESUME_HEADER_SIZE) {
        return GDX_GP_RESUME_ERR_TRUNCATED;
    }
    if (memcmp(data, GDX_GP_RESUME_MAGIC, 4) != 0) {
        return GDX_GP_RESUME_ERR_MAGIC;
    }
    if (get_u32(data + 4) != GDX_GP_RESUME_VERSION) {
        return GDX_GP_RESUME_ERR_VERSION;
    }
    declared = get_u32(data + 8);
    if (declared != GDX_GP_RESUME_FILE_SIZE) {
        return GDX_GP_RESUME_ERR_SIZE;
    }
    if (length < GDX_GP_RESUME_FILE_SIZE) {
        return GDX_GP_RESUME_ERR_TRUNCATED;
    }
    if (length > GDX_GP_RESUME_FILE_SIZE) {
        return GDX_GP_RESUME_ERR_SIZE;
    }
    stored = get_u32(data + OFF_CRC);
    if (stored != gdx_course_registry_crc32(data, OFF_CRC)) {
        return GDX_GP_RESUME_ERR_CRC;
    }

    parsed.cup = get_s32(data + OFF_CUP);
    parsed.difficulty = get_s32(data + OFF_DIFFICULTY);
    parsed.numPlayers = get_s32(data + OFF_NUM_PLAYERS);
    parsed.courseIndex = get_s32(data + OFF_COURSE_INDEX);
    parsed.lastRandomCourseIndex = get_s32(data + OFF_LAST_RANDOM);
    parsed.raceCounter = get_s32(data + OFF_RACE_COUNTER);
    parsed.randSeed1 = get_s32(data + OFF_RAND_SEED1);
    parsed.randMask1 = get_u32(data + OFF_RAND_MASK1);
    parsed.randSeed2 = get_s32(data + OFF_RAND_SEED2);
    parsed.randMask2 = get_u32(data + OFF_RAND_MASK2);
    for (i = 0; i < GDX_GP_RESUME_PLAYERS; i++) {
        parsed.lives[i] = get_s32(data + OFF_LIVES + 4u * (unsigned) i);
        parsed.playerCharacter[i] = get_s32(data + OFF_PLAYER_CHARACTER + 4u * (unsigned) i);
        parsed.playerSkin[i] = get_s32(data + OFF_PLAYER_SKIN + 4u * (unsigned) i);
    }
    for (i = 0; i < GDX_GP_RESUME_RACERS; i++) {
        const unsigned char* racer = data + OFF_RACERS + RACER_STRIDE * (unsigned) i;
        parsed.racers[i].points = get_s32(racer + 0);
        parsed.racers[i].character = get_s32(racer + 4);
        parsed.racers[i].skin = get_s32(racer + 8);
        parsed.engine[i] = get_f32(data + OFF_ENGINE + 4u * (unsigned) i);
    }

    error = gdx_gp_resume_validate(&parsed);
    if (error != GDX_GP_RESUME_OK) {
        return error;
    }
    *out = parsed;
    return GDX_GP_RESUME_OK;
}

/* ---------------------------------------------------------------------------------
 * File operations.
 * ------------------------------------------------------------------------------- */

static GdxGpResumeError write_file_atomic(const char* path, const unsigned char* buf, size_t length) {
    char tmp[1024];
    FILE* file;
    size_t written;
    int ok;

    if (path == NULL || strlen(path) + 5 >= sizeof(tmp)) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    file = fopen(tmp, "wb");
    if (file == NULL) {
        return GDX_GP_RESUME_ERR_WRITE;
    }
    written = fwrite(buf, 1, length, file);
    ok = (written == length) && (fflush(file) == 0);
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(tmp);
        return GDX_GP_RESUME_ERR_WRITE;
    }

#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return GDX_GP_RESUME_ERR_WRITE;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return GDX_GP_RESUME_ERR_WRITE;
    }
#endif
    return GDX_GP_RESUME_OK;
}

GdxGpResumeError gdx_gp_resume_save_to(const char* path, const GdxGpResumeState* state) {
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE];
    GdxGpResumeError error;

    if (path == NULL || state == NULL) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    error = gdx_gp_resume_encode(state, buf, sizeof(buf));
    if (error != GDX_GP_RESUME_OK) {
        return error;
    }
    return write_file_atomic(path, buf, sizeof(buf));
}

GdxGpResumeError gdx_gp_resume_load_from(const char* path, GdxGpResumeState* out) {
    /* One byte past the fixed size, so a file with trailing bytes is seen as oversized. */
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE + 1];
    FILE* file;
    size_t length;

    if (path == NULL || out == NULL) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return GDX_GP_RESUME_ERR_IO;
    }
    length = fread(buf, 1, sizeof(buf), file);
    fclose(file);
    return gdx_gp_resume_decode(buf, length, out);
}

GdxGpResumeError gdx_gp_resume_clear_from(const char* path) {
    FILE* file;

    if (path == NULL) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    if (remove(path) == 0) {
        return GDX_GP_RESUME_OK;
    }
    /* Nothing to clear when the file is already gone. */
    file = fopen(path, "rb");
    if (file == NULL) {
        return GDX_GP_RESUME_OK;
    }
    fclose(file);
    return GDX_GP_RESUME_ERR_WRITE;
}

static char sDirectory[1024];

void gdx_gp_resume_set_directory(const char* directory) {
    size_t length;

    sDirectory[0] = '\0';
    if (directory == NULL) {
        return;
    }
    length = strlen(directory);
    if (length == 0 || length + 1 >= sizeof(sDirectory)) {
        return;
    }
    memcpy(sDirectory, directory, length + 1);
}

GdxGpResumeError gdx_gp_resume_default_path(char* out, size_t outCap) {
    const size_t nameLength = strlen(GDX_GP_RESUME_FILE_NAME);

    if (out == NULL || outCap == 0) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    if (sDirectory[0] != '\0') {
        const size_t dirLength = strlen(sDirectory);
        const int needsSeparator = sDirectory[dirLength - 1] != '/' && sDirectory[dirLength - 1] != '\\';

        if (dirLength + (size_t) needsSeparator + nameLength + 1 > outCap) {
            return GDX_GP_RESUME_ERR_ARG;
        }
        memcpy(out, sDirectory, dirLength);
        if (needsSeparator) {
            out[dirLength] = '/';
        }
        memcpy(out + dirLength + (size_t) needsSeparator, GDX_GP_RESUME_FILE_NAME, nameLength + 1);
        return GDX_GP_RESUME_OK;
    }
#ifdef _WIN32
    {
        char exe[MAX_PATH];
        DWORD n;
        size_t dirLength;

        n = GetModuleFileNameA(NULL, exe, (DWORD) sizeof(exe));
        if (n == 0 || n >= sizeof(exe)) {
            return GDX_GP_RESUME_ERR_IO;
        }
        dirLength = (size_t) n;
        while (dirLength > 0 && exe[dirLength - 1] != '\\' && exe[dirLength - 1] != '/') {
            dirLength--;
        }
        if (dirLength + nameLength + 1 > outCap) {
            return GDX_GP_RESUME_ERR_ARG;
        }
        memcpy(out, exe, dirLength);
        memcpy(out + dirLength, GDX_GP_RESUME_FILE_NAME, nameLength + 1);
    }
#else
    if (nameLength + 1 > outCap) {
        return GDX_GP_RESUME_ERR_ARG;
    }
    memcpy(out, GDX_GP_RESUME_FILE_NAME, nameLength + 1);
#endif
    return GDX_GP_RESUME_OK;
}

GdxGpResumeError gdx_gp_resume_save(const GdxGpResumeState* state) {
    char path[1024];
    GdxGpResumeError error = gdx_gp_resume_default_path(path, sizeof(path));

    if (error != GDX_GP_RESUME_OK) {
        return error;
    }
    return gdx_gp_resume_save_to(path, state);
}

GdxGpResumeError gdx_gp_resume_load(GdxGpResumeState* out) {
    char path[1024];
    GdxGpResumeError error = gdx_gp_resume_default_path(path, sizeof(path));

    if (error != GDX_GP_RESUME_OK) {
        return error;
    }
    return gdx_gp_resume_load_from(path, out);
}

int gdx_gp_resume_available(int* outDifficulty) {
    GdxGpResumeState state;

    if (gdx_gp_resume_load(&state) != GDX_GP_RESUME_OK) {
        return 0;
    }
    if (outDifficulty != NULL) {
        *outDifficulty = state.difficulty;
    }
    return 1;
}

GdxGpResumeError gdx_gp_resume_clear(void) {
    char path[1024];
    GdxGpResumeError error = gdx_gp_resume_default_path(path, sizeof(path));

    if (error != GDX_GP_RESUME_OK) {
        return error;
    }
    return gdx_gp_resume_clear_from(path);
}

const char* gdx_gp_resume_error_text(GdxGpResumeError error) {
    switch (error) {
        case GDX_GP_RESUME_OK:
            return "ok";
        case GDX_GP_RESUME_ERR_ARG:
            return "bad argument";
        case GDX_GP_RESUME_ERR_IO:
            return "no saved cup";
        case GDX_GP_RESUME_ERR_TRUNCATED:
            return "saved cup is truncated";
        case GDX_GP_RESUME_ERR_MAGIC:
            return "not a saved cup file";
        case GDX_GP_RESUME_ERR_VERSION:
            return "saved cup was written by a different version";
        case GDX_GP_RESUME_ERR_SIZE:
            return "saved cup has the wrong size";
        case GDX_GP_RESUME_ERR_CRC:
            return "saved cup failed its checksum";
        case GDX_GP_RESUME_ERR_RANGE:
            return "saved cup holds an out-of-range value";
        case GDX_GP_RESUME_ERR_WRITE:
            return "could not write the saved cup";
    }
    return "unknown error";
}
