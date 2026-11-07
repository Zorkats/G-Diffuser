/* port/gdx_death_race.c -- Death Race rules, course keys, and the best-time records store. See gdx_death_race.h.
 * Pure C. One race is tracked at a time, and one records store per process. Records keep the order they were
 * first submitted in, so a canonical image (zero padding) encodes back to the same bytes after it is decoded.
 */

#define _CRT_SECURE_NO_WARNINGS /* fopen, remove, and rename below; harmless on non-MSVC */

#include "gdx_death_race.h"
#include "gdx_course_registry.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* Wire format, version 1: a 12-byte header, one 40-byte entry per record, and a CRC-32 trailer. The table in
 * gdx_death_race.h is the reference. Inside an entry: key at 0..31, laps at 32, three zero bytes, time at 36. */
#define DR_MAGIC "GDXD"
#define DR_VERSION 1u
#define DR_HEADER_BYTES 12u
#define DR_ENTRY_BYTES 40u
#define DR_LAPS_AT 32u
#define DR_TIME_AT 36u
#define DR_CRC_BYTES 4u

/* A full store is the largest image. Loads read one byte past it, so a longer file is seen as oversized. */
#define DR_IMAGE_MAX (DR_HEADER_BYTES + DR_ENTRY_BYTES * (unsigned int) GDX_DEATH_RACE_RECORDS_MAX + DR_CRC_BYTES)

/* Stock courses 30..41 are the expansion disk cups and fixed tracks, keyed by number like 0..23 and 54. */
#define DR_EXPANSION_FIRST 30
#define DR_EXPANSION_LAST 41

/* Edit courses key as "edit-" plus their file id. */
#define DR_EDIT_PREFIX "edit-"
#define DR_EDIT_PREFIX_LENGTH (sizeof(DR_EDIT_PREFIX) - 1u)

/* ---------------------------------------------------------------------------------
 * Race rules: one race at a time, with its course, laps limit, completed laps, and outcome.
 * ------------------------------------------------------------------------------- */

static int sCourse = -1;
static int sLaps = 0;
static int sLapsDone = 0;
static GdxDeathRaceOutcome sOutcome = GDX_DEATH_RACE_RUNNING;

static int clamp_laps(int laps) {
    if (laps < 0) {
        return 0;
    }
    if (laps > GDX_DEATH_RACE_MAX_LAPS) {
        return GDX_DEATH_RACE_MAX_LAPS;
    }
    return laps;
}

void gdx_death_race_begin(int courseIndex, int laps) {
    sCourse = courseIndex;
    sLaps = clamp_laps(laps);
    sLapsDone = 0;
    sOutcome = GDX_DEATH_RACE_RUNNING;
}

void gdx_death_race_end(void) {
    sCourse = -1;
    sLaps = 0;
    sLapsDone = 0;
    sOutcome = GDX_DEATH_RACE_RUNNING;
}

int gdx_death_race_laps(void) {
    return sLaps;
}

int gdx_death_race_course(void) {
    return sCourse;
}

int gdx_death_race_stock_setup(void) {
    return sCourse == GDX_DEATH_RACE_STOCK_COURSE && sLaps == 0;
}

int gdx_death_race_check_laps(int completedLaps) {
    if (sLaps == 0 || sOutcome != GDX_DEATH_RACE_RUNNING) {
        return 0;
    }
    if (completedLaps < sLaps) {
        return 0;
    }
    sOutcome = GDX_DEATH_RACE_OUT_OF_LAPS;
    return 1;
}

void gdx_death_race_cleared(void) {
    if (sOutcome == GDX_DEATH_RACE_RUNNING) {
        sOutcome = GDX_DEATH_RACE_CLEARED;
    }
}

GdxDeathRaceOutcome gdx_death_race_outcome(void) {
    return sOutcome;
}

int gdx_death_race_show_winner(void) {
    return sOutcome == GDX_DEATH_RACE_CLEARED && sLaps > 0;
}

int gdx_death_race_show_loser(void) {
    return sOutcome == GDX_DEATH_RACE_OUT_OF_LAPS;
}

int gdx_death_race_may_record(void) {
    return sOutcome == GDX_DEATH_RACE_CLEARED;
}

int gdx_death_race_lap_done(void) {
    if (sLapsDone < INT_MAX) {
        sLapsDone++;
    }
    return sLapsDone;
}

int gdx_death_race_laps_done(void) {
    return sLapsDone;
}

/* ---------------------------------------------------------------------------------
 * Course keys.
 * ------------------------------------------------------------------------------- */

/* Builds the key of courseIndex into key, which holds GDX_DEATH_RACE_KEY_MAX bytes. Returns 0 when the course has
 * no key: an X Cup course, a negative index, or an edit course whose file id is missing or too long for the key. */
static int build_key(int courseIndex, const char* fileId, char* key) {
    size_t fileLength;

    if (courseIndex < 0) {
        return 0;
    }
    if (courseIndex >= GDX_DEATH_RACE_X_FIRST && courseIndex <= GDX_DEATH_RACE_X_LAST) {
        return 0;
    }
    if (courseIndex <= GDX_DEATH_RACE_STOCK_LAST || courseIndex == GDX_DEATH_RACE_STOCK_COURSE
        || (courseIndex >= DR_EXPANSION_FIRST && courseIndex <= DR_EXPANSION_LAST)) {
        snprintf(key, GDX_DEATH_RACE_KEY_MAX, "stock-%02d", courseIndex);
        return 1;
    }
    if (fileId == NULL || fileId[0] == '\0') {
        return 0;
    }
    fileLength = strlen(fileId);
    if (fileLength + DR_EDIT_PREFIX_LENGTH > (size_t) (GDX_DEATH_RACE_KEY_MAX - 1)) {
        return 0;
    }
    memcpy(key, DR_EDIT_PREFIX, DR_EDIT_PREFIX_LENGTH);
    memcpy(key + DR_EDIT_PREFIX_LENGTH, fileId, fileLength + 1u);
    return 1;
}

int gdx_death_race_course_key(int courseIndex, const char* fileId, char* out, int outCap) {
    char key[GDX_DEATH_RACE_KEY_MAX];
    size_t keyLength;

    if (out == NULL || outCap <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!build_key(courseIndex, fileId, key)) {
        return 0;
    }
    keyLength = strlen(key);
    if (keyLength + 1u > (size_t) outCap) {
        return 0;
    }
    memcpy(out, key, keyLength + 1u);
    return 1;
}

/* ---------------------------------------------------------------------------------
 * Little-endian access.
 * ------------------------------------------------------------------------------- */

static void put_u32(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char) (value & 0xFFu);
    p[1] = (unsigned char) ((value >> 8) & 0xFFu);
    p[2] = (unsigned char) ((value >> 16) & 0xFFu);
    p[3] = (unsigned char) ((value >> 24) & 0xFFu);
}

static uint32_t get_u32(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* ---------------------------------------------------------------------------------
 * Records store: one best time per key and laps.
 * ------------------------------------------------------------------------------- */

typedef struct DrRecord {
    char key[GDX_DEATH_RACE_KEY_MAX];
    int laps;
    int timeMs;
} DrRecord;

static DrRecord sRecords[GDX_DEATH_RACE_RECORDS_MAX];
static int sRecordCount = 0;

/* Length of the key at key: 1 to GDX_DEATH_RACE_KEY_MAX - 1, when a NUL falls within its 32 bytes. Returns -1 for
 * NULL, an empty key, or a key with no NUL in its 32 bytes. Never reads past the 32nd byte. */
static int key_length(const char* key) {
    int i;

    if (key == NULL) {
        return -1;
    }
    for (i = 0; i < GDX_DEATH_RACE_KEY_MAX; i++) {
        if (key[i] == '\0') {
            return i > 0 ? i : -1;
        }
    }
    return -1;
}

/* Index of the record for key and laps, or -1. key must already pass key_length. */
static int find_record(const char* key, int laps) {
    int i;

    for (i = 0; i < sRecordCount; i++) {
        if (sRecords[i].laps == laps && strcmp(sRecords[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

void gdx_death_race_records_reset(void) {
    sRecordCount = 0;
}

int gdx_death_race_records_count(void) {
    return sRecordCount;
}

int gdx_death_race_record_get(const char* key, int laps) {
    int index;

    if (key_length(key) < 0 || laps < 0 || laps > GDX_DEATH_RACE_MAX_LAPS) {
        return GDX_DEATH_RACE_NO_TIME;
    }
    index = find_record(key, laps);
    if (index < 0) {
        return GDX_DEATH_RACE_NO_TIME;
    }
    return sRecords[index].timeMs;
}

int gdx_death_race_record_submit(const char* key, int laps, int timeMs) {
    int index;
    int keyLen = key_length(key);

    if (keyLen < 0 || laps < 0 || laps > GDX_DEATH_RACE_MAX_LAPS) {
        return 0;
    }
    if (timeMs < 1 || timeMs > GDX_DEATH_RACE_NO_TIME - 1) {
        return 0;
    }
    index = find_record(key, laps);
    if (index >= 0) {
        if (timeMs >= sRecords[index].timeMs) {
            return 0;
        }
        sRecords[index].timeMs = timeMs;
        return 1;
    }
    if (sRecordCount >= GDX_DEATH_RACE_RECORDS_MAX) {
        return 0;
    }
    memcpy(sRecords[sRecordCount].key, key, (size_t) keyLen + 1u);
    sRecords[sRecordCount].laps = laps;
    sRecords[sRecordCount].timeMs = timeMs;
    sRecordCount++;
    return 1;
}

/* ---------------------------------------------------------------------------------
 * Codec.
 * ------------------------------------------------------------------------------- */

int gdx_death_race_records_encoded_size(void) {
    return (int) (DR_HEADER_BYTES + DR_CRC_BYTES + DR_ENTRY_BYTES * (unsigned int) sRecordCount);
}

int gdx_death_race_records_encode(unsigned char* out, unsigned int outCap) {
    unsigned int size;
    unsigned int i;

    if (out == NULL) {
        return 0;
    }
    size = (unsigned int) gdx_death_race_records_encoded_size();
    if (outCap < size) {
        return 0;
    }

    memcpy(out, DR_MAGIC, 4u);
    put_u32(out + 4, DR_VERSION);
    put_u32(out + 8, (uint32_t) sRecordCount);
    for (i = 0; i < (unsigned int) sRecordCount; i++) {
        unsigned char* entry = out + DR_HEADER_BYTES + DR_ENTRY_BYTES * i;

        memset(entry, 0, DR_ENTRY_BYTES);
        memcpy(entry, sRecords[i].key, strlen(sRecords[i].key));
        entry[DR_LAPS_AT] = (unsigned char) sRecords[i].laps;
        put_u32(entry + DR_TIME_AT, (uint32_t) sRecords[i].timeMs);
    }
    put_u32(out + size - DR_CRC_BYTES, gdx_course_registry_crc32(out, size - DR_CRC_BYTES));
    return (int) size;
}

int gdx_death_race_records_decode(const unsigned char* data, unsigned int length) {
    unsigned int count;
    unsigned int expected;
    unsigned int i;
    unsigned int j;

    if (data == NULL || length < DR_HEADER_BYTES) {
        return 0;
    }
    if (memcmp(data, DR_MAGIC, 4u) != 0) {
        return 0;
    }
    if (get_u32(data + 4) != DR_VERSION) {
        return 0;
    }
    count = get_u32(data + 8);
    if (count > (unsigned int) GDX_DEATH_RACE_RECORDS_MAX) {
        return 0;
    }
    expected = DR_HEADER_BYTES + DR_ENTRY_BYTES * count + DR_CRC_BYTES;
    if (length != expected) {
        return 0;
    }
    if (get_u32(data + length - DR_CRC_BYTES) != gdx_course_registry_crc32(data, length - DR_CRC_BYTES)) {
        return 0;
    }

    /* Every entry is checked before the store is touched, so a refused image leaves the store unchanged. */
    for (i = 0; i < count; i++) {
        const unsigned char* entry = data + DR_HEADER_BYTES + DR_ENTRY_BYTES * i;
        uint32_t timeMs = get_u32(entry + DR_TIME_AT);

        if (key_length((const char*) entry) < 0) {
            return 0;
        }
        if (entry[DR_LAPS_AT] > GDX_DEATH_RACE_MAX_LAPS) {
            return 0;
        }
        if (timeMs < 1u || timeMs > (uint32_t) (GDX_DEATH_RACE_NO_TIME - 1)) {
            return 0;
        }
        for (j = 0; j < i; j++) {
            const unsigned char* earlier = data + DR_HEADER_BYTES + DR_ENTRY_BYTES * j;

            if (earlier[DR_LAPS_AT] == entry[DR_LAPS_AT]
                && strcmp((const char*) earlier, (const char*) entry) == 0) {
                return 0;
            }
        }
    }

    for (i = 0; i < count; i++) {
        const unsigned char* entry = data + DR_HEADER_BYTES + DR_ENTRY_BYTES * i;
        int keyLen = key_length((const char*) entry);

        memcpy(sRecords[i].key, entry, (size_t) keyLen + 1u);
        sRecords[i].laps = entry[DR_LAPS_AT];
        sRecords[i].timeMs = (int) get_u32(entry + DR_TIME_AT);
    }
    sRecordCount = (int) count;
    return 1;
}

/* ---------------------------------------------------------------------------------
 * File operations.
 * ------------------------------------------------------------------------------- */

static unsigned char sImage[DR_IMAGE_MAX + 1u];

/* Writes buf to "<path>.tmp" and moves it over path, so a crash leaves the old file or none. Returns 1 on success. */
static int write_file_atomic(const char* path, const unsigned char* buf, size_t length) {
    char tmp[1024];
    FILE* file;
    size_t written;
    int ok;

    if (strlen(path) + 5u >= sizeof(tmp)) {
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    file = fopen(tmp, "wb");
    if (file == NULL) {
        return 0;
    }
    written = fwrite(buf, 1, length, file);
    ok = (written == length) && (fflush(file) == 0);
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(tmp);
        return 0;
    }

#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return 0;
    }
#else
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
#endif
    return 1;
}

int gdx_death_race_records_save_to(const char* path) {
    int size;

    if (path == NULL) {
        return 0;
    }
    size = gdx_death_race_records_encode(sImage, (unsigned int) sizeof(sImage));
    if (size == 0) {
        return 0;
    }
    return write_file_atomic(path, sImage, (size_t) size);
}

int gdx_death_race_records_load_from(const char* path) {
    FILE* file;
    size_t length;

    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        /* A file that does not exist is an empty store. Any other failure leaves the store as it was. */
        if (errno == ENOENT) {
            gdx_death_race_records_reset();
            return 1;
        }
        return 0;
    }
    length = fread(sImage, 1, sizeof(sImage), file);
    if (ferror(file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return gdx_death_race_records_decode(sImage, (unsigned int) length);
}

/* ---------------------------------------------------------------------------------
 * Default directory and file name.
 * ------------------------------------------------------------------------------- */

static char sDirectory[1024];

void gdx_death_race_records_set_directory(const char* directory) {
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

static char sFileName[128] = GDX_DEATH_RACE_FILE_NAME;

void gdx_death_race_records_set_file_name(const char* name) {
    size_t length;

    memcpy(sFileName, GDX_DEATH_RACE_FILE_NAME, sizeof(GDX_DEATH_RACE_FILE_NAME));
    if (name == NULL || strpbrk(name, "/\\") != NULL) {
        return;
    }
    length = strlen(name);
    if (length == 0 || length >= sizeof(sFileName)) {
        return;
    }
    memcpy(sFileName, name, length + 1);
}

/* Writes the default file path into out: the set directory and file name, else the executable's directory on
 * Windows or the bare file name elsewhere. Returns 0 when the path does not fit or the executable is not found. */
static int default_path(char* out, size_t outCap) {
    const size_t nameLength = strlen(sFileName);

    if (out == NULL || outCap == 0) {
        return 0;
    }
    if (sDirectory[0] != '\0') {
        const size_t dirLength = strlen(sDirectory);
        const size_t separator = (sDirectory[dirLength - 1] != '/' && sDirectory[dirLength - 1] != '\\') ? 1u : 0u;

        if (dirLength + separator + nameLength + 1u > outCap) {
            return 0;
        }
        memcpy(out, sDirectory, dirLength);
        if (separator) {
            out[dirLength] = '/';
        }
        memcpy(out + dirLength + separator, sFileName, nameLength + 1u);
        return 1;
    }
#ifdef _WIN32
    {
        char exe[MAX_PATH];
        DWORD n;
        size_t dirLength;

        n = GetModuleFileNameA(NULL, exe, (DWORD) sizeof(exe));
        if (n == 0 || n >= sizeof(exe)) {
            return 0;
        }
        dirLength = (size_t) n;
        while (dirLength > 0 && exe[dirLength - 1] != '\\' && exe[dirLength - 1] != '/') {
            dirLength--;
        }
        if (dirLength + nameLength + 1u > outCap) {
            return 0;
        }
        memcpy(out, exe, dirLength);
        memcpy(out + dirLength, sFileName, nameLength + 1u);
    }
#else
    if (nameLength + 1u > outCap) {
        return 0;
    }
    memcpy(out, sFileName, nameLength + 1u);
#endif
    return 1;
}

int gdx_death_race_records_save(void) {
    char path[1024];

    if (!default_path(path, sizeof(path))) {
        return 0;
    }
    return gdx_death_race_records_save_to(path);
}

int gdx_death_race_records_load(void) {
    char path[1024];

    if (!default_path(path, sizeof(path))) {
        return 0;
    }
    return gdx_death_race_records_load_from(path);
}
