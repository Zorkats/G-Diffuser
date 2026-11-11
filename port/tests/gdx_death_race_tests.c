/* port/tests/gdx_death_race_tests.c -- unit tests for the Death Race rules, lap count, course keys, and records store.
 * Standalone: links gdx_death_race.c and gdx_course_registry.c (for the CRC-32 that seals each test image).
 *
 * Every image the decode tests refuse is built here from the wire format in gdx_death_race.h, so the module's own
 * encoder is never the oracle for its decoder. The file tests write only inside a test directory under the working
 * directory, so a real death_race_records.bin beside the tests is never touched.
 */

#define _CRT_SECURE_NO_WARNINGS /* fopen, remove, and rename below; harmless on non-MSVC */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L /* mkdir and rmdir below */
#endif

#include "../gdx_death_race.h"
#include "../gdx_course_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0777)
#define TEST_RMDIR(path) rmdir(path)
#endif

#define TEST_DIR "gdx_death_race_test_dir"
#define TEST_FILE "gdx_death_race_test.bin"
#define TEST_CORRUPT_FILE "gdx_death_race_corrupt.bin"
#define TEST_MISSING_FILE "gdx_death_race_missing.bin"
#define TEST_CUSTOM_NAME "custom_records.bin"
#define TEST_PATH_FILE TEST_DIR "/" TEST_FILE
#define TEST_PATH_CORRUPT TEST_DIR "/" TEST_CORRUPT_FILE
#define TEST_PATH_MISSING TEST_DIR "/" TEST_MISSING_FILE
#define TEST_PATH_CUSTOM TEST_DIR "/" TEST_CUSTOM_NAME
#define TEST_PATH_DEFAULT TEST_DIR "/" GDX_DEATH_RACE_FILE_NAME

/* File ids of 26, 27, and 39 characters, and whole keys of 31 and 32 characters ("edit-" plus the id). */
#define TEST_FILE_ID_26 "abcdefghijklmnopqrstuvwxyz"
#define TEST_FILE_ID_27 "abcdefghijklmnopqrstuvwxyz0"
#define TEST_FILE_ID_39 "abcdefghijklmnopqrstuvwxyz0123456789abc"
#define TEST_KEY_31 "edit-abcdefghijklmnopqrstuvwxyz"
#define TEST_KEY_32 "edit-abcdefghijklmnopqrstuvwxyz0"

/* Wire sizes, written out here so the tests do not read them back from the module. */
#define TEST_HEADER_BYTES 12u
#define TEST_ENTRY_BYTES 40u
#define TEST_CRC_BYTES 4u
#define TEST_GOOD_COUNT 2u
#define TEST_GOOD_LENGTH (16u + 40u * 2u)
#define TEST_ONE_LENGTH (16u + 40u * 1u)
#define TEST_THREE_LENGTH (16u + 40u * 3u)
#define TEST_OVER_COUNT 513u
#define TEST_OVER_LENGTH (16u + 40u * 513u)

static int sFailures;

static unsigned char sGood[TEST_GOOD_LENGTH + 8u];
static unsigned char sBad[TEST_GOOD_LENGTH + 8u];
static unsigned char sEncoded[TEST_THREE_LENGTH + 8u];
static unsigned char sEncodedAgain[TEST_THREE_LENGTH + 8u];
static unsigned char sOversize[TEST_OVER_LENGTH + 8u];
static unsigned char sReadBack[TEST_OVER_LENGTH + 8u];

/* ---------------------------------------------------------------------------------
 * Checks, little-endian access, and hand-built images.
 * ------------------------------------------------------------------------------- */

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

static void put_u32(unsigned char* p, unsigned int value) {
    p[0] = (unsigned char) (value & 0xFFu);
    p[1] = (unsigned char) ((value >> 8) & 0xFFu);
    p[2] = (unsigned char) ((value >> 16) & 0xFFu);
    p[3] = (unsigned char) ((value >> 24) & 0xFFu);
}

static unsigned int get_u32(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) | ((unsigned int) p[2] << 16)
        | ((unsigned int) p[3] << 24);
}

/* Writes the 12-byte header: magic "GDXD", version 1, and the entry count. */
static void image_header(unsigned char* image, unsigned int count) {
    memcpy(image, "GDXD", 4);
    put_u32(image + 4, 1u);
    put_u32(image + 8, count);
}

/* Writes entry slot index: the key NUL-padded to 32 bytes, laps, three zero bytes, and the time as a
 * little-endian s32. The key must be shorter than 32 characters; the refusal tests build bad keys by hand. */
static void image_entry(unsigned char* image, unsigned int index, const char* key, unsigned int laps,
                        unsigned int timeMs) {
    unsigned char* entry = image + TEST_HEADER_BYTES + TEST_ENTRY_BYTES * index;

    memset(entry, 0, TEST_ENTRY_BYTES);
    memcpy(entry, key, strlen(key));
    entry[32] = (unsigned char) laps;
    put_u32(entry + 36, timeMs);
}

/* Seals an image of length bytes: its last four bytes become the CRC-32 of every byte before them. */
static void image_seal(unsigned char* image, unsigned int length) {
    uint32_t crc = gdx_course_registry_crc32(image, (size_t) (length - TEST_CRC_BYTES));

    put_u32(image + length - TEST_CRC_BYTES, (unsigned int) crc);
}

/* The reference image: stock-00 at laps 0 in 1000 ms, then edit-abc at laps 3 in 2000 ms. */
static void build_good_image(unsigned char* image) {
    image_header(image, TEST_GOOD_COUNT);
    image_entry(image, 0u, "stock-00", 0u, 1000u);
    image_entry(image, 1u, "edit-abc", 3u, 2000u);
    image_seal(image, TEST_GOOD_LENGTH);
}

/* A sealed image of one entry, TEST_ONE_LENGTH bytes long. */
static void build_one_entry(unsigned char* image, const char* key, unsigned int laps, unsigned int timeMs) {
    image_header(image, 1u);
    image_entry(image, 0u, key, laps, timeMs);
    image_seal(image, TEST_ONE_LENGTH);
}

/* A sealed image that declares 513 entries, one more than the store holds. Every entry is valid and its key
 * is distinct, so only the count can refuse it. */
static void build_over_image(unsigned char* image) {
    char key[16];
    unsigned int i;

    image_header(image, TEST_OVER_COUNT);
    for (i = 0; i < TEST_OVER_COUNT; i++) {
        snprintf(key, sizeof(key), "k%03u", i);
        image_entry(image, i, key, 0u, 1000u + i);
    }
    image_seal(image, TEST_OVER_LENGTH);
}

/* ---------------------------------------------------------------------------------
 * Store and file helpers.
 * ------------------------------------------------------------------------------- */

/* The reference store, in the order the good image lists it. */
static void set_reference_store(void) {
    gdx_death_race_records_reset();
    gdx_death_race_record_submit("stock-00", 0, 1000);
    gdx_death_race_record_submit("edit-abc", 3, 2000);
}

static int reference_intact(void) {
    return gdx_death_race_records_count() == 2 && gdx_death_race_record_get("stock-00", 0) == 1000
        && gdx_death_race_record_get("edit-abc", 3) == 2000;
}

/* A store holding one sentinel record. A refused load or decode must leave exactly this store behind. */
static void set_sentinel(void) {
    gdx_death_race_records_reset();
    gdx_death_race_record_submit("keep-me", 1, 777);
}

static int sentinel_intact(void) {
    return gdx_death_race_records_count() == 1 && gdx_death_race_record_get("keep-me", 1) == 777;
}

/* 1 when decode refuses the image and the sentinel store is left unchanged. */
static int refused(const unsigned char* image, unsigned int length) {
    set_sentinel();
    return gdx_death_race_records_decode(image, length) == 0 && sentinel_intact();
}

static int file_exists(const char* path) {
    FILE* file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static long file_length(const char* path) {
    FILE* file = fopen(path, "rb");
    long length;

    if (file == NULL) {
        return -1L;
    }
    fseek(file, 0L, SEEK_END);
    length = ftell(file);
    fclose(file);
    return length;
}

/* 1 when the file holds exactly the length bytes of expected. */
static int file_matches(const char* path, const unsigned char* expected, unsigned int length) {
    FILE* file = fopen(path, "rb");
    size_t got;

    if (file == NULL) {
        return 0;
    }
    got = fread(sReadBack, 1, sizeof(sReadBack), file);
    fclose(file);
    return got == (size_t) length && memcmp(sReadBack, expected, length) == 0;
}

static int write_bytes(const char* path, const unsigned char* bytes, unsigned int length) {
    FILE* file = fopen(path, "wb");
    size_t written;

    if (file == NULL) {
        return 0;
    }
    written = fwrite(bytes, 1, length, file);
    fclose(file);
    return written == (size_t) length;
}

static void remove_test_files(void) {
    remove(TEST_PATH_FILE);
    remove(TEST_PATH_FILE ".tmp");
    remove(TEST_PATH_CORRUPT);
    remove(TEST_PATH_CORRUPT ".tmp");
    remove(TEST_PATH_MISSING);
    remove(TEST_PATH_MISSING ".tmp");
    remove(TEST_PATH_CUSTOM);
    remove(TEST_PATH_CUSTOM ".tmp");
    remove(TEST_PATH_DEFAULT);
    remove(TEST_PATH_DEFAULT ".tmp");
    remove(TEST_DIR "/evil.bin");
    remove(TEST_DIR "/evil.bin.tmp");
    remove(TEST_DIR ".tmp");
    TEST_RMDIR(TEST_DIR);
}

static void make_test_directory(void) {
    remove_test_files();
    check(TEST_MKDIR(TEST_DIR) == 0, "setup: the test directory can be created");
}

/* ---------------------------------------------------------------------------------
 * Oracle: the CRC-32 that seals every image below.
 * ------------------------------------------------------------------------------- */

static void test_oracle(void) {
    check(gdx_course_registry_crc32((const unsigned char*) "123456789", 9u) == 0xCBF43926u,
          "oracle: the registry CRC-32 matches the published check value");
}

/* ---------------------------------------------------------------------------------
 * Race rules: course, laps, stock setup, outcome, and what may be shown or recorded.
 * ------------------------------------------------------------------------------- */

static void test_race_rules(void) {
    check(gdx_death_race_course() == -1, "rules: no race means course -1");
    check(gdx_death_race_laps() == 0, "rules: no race means zero laps");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_RUNNING, "rules: no race means RUNNING");
    check(gdx_death_race_stock_setup() == 0, "rules: no race is not stock setup");
    check(gdx_death_race_check_laps(5, 1) == 0, "rules: no race never finishes on laps");

    /* Laps are clamped to 0..99. */
    gdx_death_race_begin(0, 0);
    check(gdx_death_race_laps() == 0, "rules: laps 0 is endless");
    gdx_death_race_begin(0, 1);
    check(gdx_death_race_laps() == 1, "rules: laps 1 is the smallest limit");
    gdx_death_race_begin(0, 99);
    check(gdx_death_race_laps() == 99, "rules: laps 99 is the largest limit");
    gdx_death_race_begin(0, 100);
    check(gdx_death_race_laps() == GDX_DEATH_RACE_MAX_LAPS, "rules: laps 100 is clamped to 99");
    gdx_death_race_begin(0, -5);
    check(gdx_death_race_laps() == 0, "rules: negative laps are clamped to 0");
    gdx_death_race_begin(0, 1000);
    check(gdx_death_race_laps() == 99, "rules: laps 1000 is clamped to 99");

    gdx_death_race_begin(54, 0);
    check(gdx_death_race_course() == 54, "rules: begin records the course");
    gdx_death_race_end();
    check(gdx_death_race_course() == -1 && gdx_death_race_laps() == 0, "rules: end clears the course and laps");

    /* Stock setup is Death Race (54) with endless laps and nothing else. */
    gdx_death_race_begin(54, 0);
    check(gdx_death_race_stock_setup() == 1, "rules: course 54 with endless laps is stock setup");
    gdx_death_race_begin(54, 1);
    check(gdx_death_race_stock_setup() == 0, "rules: course 54 with a laps limit is not stock setup");
    gdx_death_race_begin(54, 99);
    check(gdx_death_race_stock_setup() == 0, "rules: course 54 with laps 99 is not stock setup");
    gdx_death_race_begin(53, 0);
    check(gdx_death_race_stock_setup() == 0, "rules: X Cup course 53 is not stock setup");
    gdx_death_race_begin(23, 0);
    check(gdx_death_race_stock_setup() == 0, "rules: stock course 23 is not stock setup");
    gdx_death_race_begin(30, 0);
    check(gdx_death_race_stock_setup() == 0, "rules: expansion course 30 is not stock setup");
    gdx_death_race_begin(55, 0);
    check(gdx_death_race_stock_setup() == 0, "rules: edit course 55 is not stock setup");
    gdx_death_race_end();
    check(gdx_death_race_stock_setup() == 0, "rules: after end there is no stock setup");

    /* A limit of 3 finishes on the third completed lap, and not before. */
    gdx_death_race_begin(0, 3);
    check(gdx_death_race_check_laps(2, 1) == 0, "rules: two laps of three do not finish the race");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_RUNNING, "rules: a race short of the limit stays RUNNING");
    check(gdx_death_race_check_laps(3, 1) == 1, "rules: the third lap of three finishes the race");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_OUT_OF_LAPS, "rules: finishing on the limit is OUT_OF_LAPS");
    check(gdx_death_race_check_laps(4, 1) == 0, "rules: a finished race is not reported again");
    check(gdx_death_race_show_loser() == 1, "rules: an out-of-laps race shows the loser");
    check(gdx_death_race_show_winner() == 0, "rules: an out-of-laps race shows no winner");
    check(gdx_death_race_may_record() == 0, "rules: an out-of-laps race may not be recorded");
    gdx_death_race_cleared();
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_OUT_OF_LAPS,
          "rules: a clear after OUT_OF_LAPS does not change the outcome");
    gdx_death_race_end();

    /* The last rival is destroyed but its explosion is still playing when the player finishes the laps:
       that is a full clear, not a loss. */
    gdx_death_race_begin(0, 3);
    check(gdx_death_race_check_laps(2, 0) == 0, "rules: no rival standing short of the limit does not finish");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_RUNNING, "rules: short of the limit stays RUNNING with no rival");
    check(gdx_death_race_check_laps(3, 0) == 1, "rules: the last lap with no rival standing finishes the race");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_CLEARED, "rules: the last lap with no rival standing is CLEARED");
    check(gdx_death_race_show_winner() == 1, "rules: that clear shows the winner");
    check(gdx_death_race_show_loser() == 0, "rules: that clear shows no loser");
    check(gdx_death_race_may_record() == 1, "rules: that clear may be recorded");
    gdx_death_race_end();

    /* One rival still racing on the last lap is a loss, however many were destroyed. */
    gdx_death_race_begin(0, 3);
    check(gdx_death_race_check_laps(3, 1) == 1, "rules: the last lap with one rival standing finishes the race");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_OUT_OF_LAPS, "rules: one rival standing is OUT_OF_LAPS");
    gdx_death_race_end();

    /* Endless laps never finish on laps. */
    gdx_death_race_begin(0, 0);
    check(gdx_death_race_check_laps(1000, 1) == 0, "rules: endless laps never finish on laps");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_RUNNING, "rules: an endless race stays RUNNING on laps");
    gdx_death_race_end();

    /* A clear is the win under a limit. An endless clear may be recorded but shows no winner. */
    gdx_death_race_begin(0, 3);
    check(gdx_death_race_may_record() == 0, "rules: a running race may not be recorded");
    check(gdx_death_race_show_winner() == 0, "rules: a running race shows no winner");
    gdx_death_race_cleared();
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_CLEARED, "rules: a clear sets CLEARED");
    check(gdx_death_race_show_winner() == 1, "rules: a clear under a limit shows the winner");
    check(gdx_death_race_show_loser() == 0, "rules: a clear shows no loser");
    check(gdx_death_race_may_record() == 1, "rules: a clear may be recorded");
    check(gdx_death_race_check_laps(3, 1) == 0, "rules: a cleared race does not finish on laps");
    gdx_death_race_cleared();
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_CLEARED, "rules: a second clear keeps CLEARED");
    gdx_death_race_end();

    gdx_death_race_begin(0, 0);
    gdx_death_race_cleared();
    check(gdx_death_race_show_winner() == 0, "rules: an endless clear shows no winner");
    check(gdx_death_race_may_record() == 1, "rules: an endless clear may be recorded");
    gdx_death_race_end();

    /* end() clears the outcome too, so nothing from a finished race carries over. */
    gdx_death_race_begin(54, 5);
    gdx_death_race_cleared();
    gdx_death_race_end();
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_RUNNING, "rules: end sets RUNNING");
    check(gdx_death_race_may_record() == 0, "rules: after end nothing may be recorded");
}

/* ---------------------------------------------------------------------------------
 * Lap count: lap_done adds one completed lap, laps_done reads it, and begin and end reset it.
 * ------------------------------------------------------------------------------- */

static void test_lap_count(void) {
    int i;
    int finished = 0;

    gdx_death_race_end();
    check(gdx_death_race_laps_done() == 0, "laps: no race means zero completed laps");

    gdx_death_race_begin(0, 2);
    check(gdx_death_race_laps_done() == 0, "laps: a new race starts at zero completed laps");
    check(gdx_death_race_lap_done() == 1, "laps: the first completed lap returns 1");
    check(gdx_death_race_lap_done() == 2, "laps: the second completed lap returns 2");
    check(gdx_death_race_laps_done() == 2, "laps: laps_done reports the count");
    check(gdx_death_race_laps_done() == 2, "laps: laps_done does not change the count");

    gdx_death_race_begin(0, 2);
    check(gdx_death_race_laps_done() == 0, "laps: begin resets the count");
    gdx_death_race_lap_done();
    gdx_death_race_end();
    check(gdx_death_race_laps_done() == 0, "laps: end resets the count");

    /* The decomp flow: one check per completed lap, fed by lap_done. */
    gdx_death_race_begin(0, 2);
    check(gdx_death_race_check_laps(gdx_death_race_lap_done(), 1) == 0, "laps: the first of two laps does not finish");
    check(gdx_death_race_check_laps(gdx_death_race_lap_done(), 1) == 1, "laps: the second of two laps finishes");
    check(gdx_death_race_outcome() == GDX_DEATH_RACE_OUT_OF_LAPS, "laps: the limit finishes the race out of laps");
    gdx_death_race_end();

    /* Endless laps: every lap is counted, and none finishes the race. */
    gdx_death_race_begin(0, 0);
    for (i = 0; i < 5; i++) {
        finished += gdx_death_race_check_laps(gdx_death_race_lap_done(), 1);
    }
    check(finished == 0, "laps: endless laps never finish the race");
    check(gdx_death_race_laps_done() == 5, "laps: endless laps are still counted");
    gdx_death_race_end();
}

/* ---------------------------------------------------------------------------------
 * Course keys. Stock courses 0..23, 30..41, and 54 key by number; edit courses key by file id;
 * X Cup courses 48..53 have no key.
 * ------------------------------------------------------------------------------- */

/* 1 when the key for courseIndex is expected and fits outCap. outCap must not exceed the 32-byte buffer. */
static int key_is_in(int courseIndex, const char* fileId, const char* expected, int outCap) {
    char out[GDX_DEATH_RACE_KEY_MAX];

    memset(out, 'S', sizeof(out));
    if (gdx_death_race_course_key(courseIndex, fileId, out, outCap) != 1) {
        return 0;
    }
    return strncmp(out, expected, sizeof(out)) == 0;
}

static int key_is(int courseIndex, const char* fileId, const char* expected) {
    return key_is_in(courseIndex, fileId, expected, GDX_DEATH_RACE_KEY_MAX);
}

/* 1 when the key is refused and nothing is written: out[0] is "" when outCap > 0, and bytes at or past
 * outCap keep their fill. */
static int key_refused_in(int courseIndex, const char* fileId, int outCap) {
    char out[64];

    memset(out, 'S', sizeof(out));
    if (gdx_death_race_course_key(courseIndex, fileId, out, outCap) != 0) {
        return 0;
    }
    if (outCap > 0) {
        if (out[0] != '\0') {
            return 0;
        }
        return outCap >= (int) sizeof(out) || out[outCap] == 'S';
    }
    return out[0] == 'S';
}

static void test_course_keys(void) {
    /* Stock courses key by number, and the file id is ignored for them. */
    check(key_is(0, NULL, "stock-00"), "keys: course 0 is stock-00");
    check(key_is(9, NULL, "stock-09"), "keys: course 9 is stock-09, two digits");
    check(key_is(23, NULL, "stock-23"), "keys: course 23 is stock-23");
    check(key_is(30, NULL, "stock-30"), "keys: expansion disk cup course 30 is stock-30");
    check(key_is(41, "ignored", "stock-41"), "keys: fixed track 41 is stock-41 and ignores its file id");
    check(key_is(54, NULL, "stock-54"), "keys: Death Race course 54 is stock-54");

    /* Edit courses 24..29, 42..47, and 55 and up key by their file id. */
    check(key_is(24, "abc", "edit-abc"), "keys: edit slot 24 is edit-abc");
    check(key_is(29, "abc", "edit-abc"), "keys: edit slot 29 is edit-abc");
    check(key_is(42, "abc", "edit-abc"), "keys: edit course 42 is edit-abc");
    check(key_is(47, "abc", "edit-abc"), "keys: edit course 47 is edit-abc");
    check(key_is(55, "abc", "edit-abc"), "keys: edit course 55 is edit-abc");
    check(key_is(100, "abc", "edit-abc"), "keys: edit course 100 is edit-abc");
    check(key_is(24, TEST_FILE_ID_26, TEST_KEY_31), "keys: a 26-character file id gives a 31-character key");

    /* X Cup courses 48..53 have no key. */
    check(key_refused_in(48, "abc", 64), "keys: X Cup course 48 has no key");
    check(key_refused_in(50, "abc", 64), "keys: X Cup course 50 has no key");
    check(key_refused_in(53, "abc", 64), "keys: X Cup course 53 has no key");

    /* Negative indexes have no key. */
    check(key_refused_in(-1, "abc", 64), "keys: course -1 has no key");
    check(key_refused_in(-5, NULL, 64), "keys: course -5 has no key");

    /* An edit course needs a non-empty file id. */
    check(key_refused_in(24, NULL, 64), "keys: an edit course with no file id has no key");
    check(key_refused_in(24, "", 64), "keys: an edit course with an empty file id has no key");

    /* The whole key holds 31 characters, so the file id holds 26 at most. */
    check(key_refused_in(24, TEST_FILE_ID_27, 64), "keys: a 27-character file id has no key, even with room");
    check(key_refused_in(24, TEST_FILE_ID_39, 64), "keys: a 39-character file id has no key");

    /* The buffer must hold the key and its NUL: stock-00 and edit-abc need 9 bytes. */
    check(key_refused_in(0, NULL, 8), "keys: a buffer of 8 bytes is too small for stock-00");
    check(key_is_in(0, NULL, "stock-00", 9), "keys: a buffer of 9 bytes holds stock-00");
    check(key_refused_in(24, "abc", 8), "keys: a buffer of 8 bytes is too small for edit-abc");
    check(key_is_in(24, "abc", "edit-abc", 9), "keys: a buffer of 9 bytes holds edit-abc");

    /* Nothing is written without an output buffer. */
    check(gdx_death_race_course_key(0, NULL, NULL, 32) == 0, "keys: a NULL out has no key");
    check(key_refused_in(0, NULL, 0), "keys: outCap 0 writes nothing");
    check(key_refused_in(0, NULL, -1), "keys: a negative outCap writes nothing");
}

/* ---------------------------------------------------------------------------------
 * Records store: best time per key and laps, and the full-store limit.
 * ------------------------------------------------------------------------------- */

static void test_records(void) {
    char key[16];
    int i;

    gdx_death_race_records_reset();
    check(gdx_death_race_records_count() == 0, "records: reset leaves an empty store");
    check(gdx_death_race_record_get("stock-00", 0) == GDX_DEATH_RACE_NO_TIME, "records: an empty store has no time");

    /* One best time per key and laps: a faster time replaces, and an equal or slower time is refused. */
    check(gdx_death_race_record_submit("stock-00", 0, 5000) == 1, "records: the first time is stored");
    check(gdx_death_race_record_get("stock-00", 0) == 5000, "records: get returns the stored time");
    check(gdx_death_race_record_submit("stock-00", 0, 6000) == 0, "records: a slower time is refused");
    check(gdx_death_race_record_submit("stock-00", 0, 5000) == 0, "records: an equal time is refused");
    check(gdx_death_race_record_get("stock-00", 0) == 5000, "records: a refused time keeps the best time");
    check(gdx_death_race_record_submit("stock-00", 0, 4000) == 1, "records: a faster time replaces the best");
    check(gdx_death_race_record_get("stock-00", 0) == 4000, "records: get returns the faster time");
    check(gdx_death_race_records_count() == 1, "records: replacing a time does not add a record");

    /* Laps are part of the identity: laps 3 is its own record. */
    check(gdx_death_race_record_submit("stock-00", 3, 7000) == 1, "records: a new laps value is a new record");
    check(gdx_death_race_record_get("stock-00", 3) == 7000, "records: laps 3 keeps its own time");
    check(gdx_death_race_record_get("stock-00", 0) == 4000, "records: laps 0 keeps its own time");

    /* The key is part of the identity too. */
    check(gdx_death_race_record_submit("edit-abc", 0, 9000) == 1, "records: a new key is a new record");
    check(gdx_death_race_record_get("edit-abc", 0) == 9000, "records: the new key keeps its own time");
    check(gdx_death_race_records_count() == 3, "records: three records are stored");
    check(gdx_death_race_record_get("stock-54", 0) == GDX_DEATH_RACE_NO_TIME, "records: an unknown key has no time");

    /* Keys: NULL, empty, and 32 characters or more are refused. 31 characters is the longest key. */
    check(gdx_death_race_record_submit(NULL, 0, 1000) == 0, "records: a NULL key is refused");
    check(gdx_death_race_record_get(NULL, 0) == GDX_DEATH_RACE_NO_TIME, "records: a NULL key has no time");
    check(gdx_death_race_record_submit("", 0, 1000) == 0, "records: an empty key is refused");
    check(gdx_death_race_record_submit(TEST_KEY_32, 0, 1000) == 0, "records: a 32-character key is refused");
    check(gdx_death_race_record_get(TEST_KEY_32, 0) == GDX_DEATH_RACE_NO_TIME,
          "records: a 32-character key has no time");
    check(gdx_death_race_record_submit(TEST_KEY_31, 0, 1000) == 1, "records: a 31-character key is stored");
    check(gdx_death_race_record_get(TEST_KEY_31, 0) == 1000, "records: a 31-character key reads back");

    /* Laps must be 0..99. */
    check(gdx_death_race_record_submit("stock-00", -1, 1000) == 0, "records: laps -1 is refused");
    check(gdx_death_race_record_submit("stock-00", 100, 1000) == 0, "records: laps 100 is refused");
    check(gdx_death_race_record_get("stock-00", -1) == GDX_DEATH_RACE_NO_TIME, "records: laps -1 has no time");
    check(gdx_death_race_record_get("stock-00", 100) == GDX_DEATH_RACE_NO_TIME, "records: laps 100 has no time");
    check(gdx_death_race_record_submit("laps-low", 0, 1000) == 1, "records: laps 0 is accepted");
    check(gdx_death_race_record_submit("laps-high", 99, 1000) == 1, "records: laps 99 is accepted");

    /* Times must be 1..3599998 milliseconds. */
    check(gdx_death_race_record_submit("time-check", 5, 0) == 0, "records: time 0 is refused");
    check(gdx_death_race_record_submit("time-check", 5, -1) == 0, "records: a negative time is refused");
    check(gdx_death_race_record_submit("time-check", 5, 3599999) == 0, "records: the no-time value is refused");
    check(gdx_death_race_record_get("time-check", 5) == GDX_DEATH_RACE_NO_TIME, "records: refused times store nothing");
    check(gdx_death_race_record_submit("time-check", 5, 3599998) == 1, "records: the largest time is accepted");
    check(gdx_death_race_record_get("time-check", 5) == 3599998, "records: the largest time reads back");
    check(gdx_death_race_record_submit("time-min", 5, 1) == 1, "records: the smallest time is accepted");

    /* A full store refuses a new key, a new laps value, and a slower time. It still takes a faster time for a
     * pair it already holds. */
    gdx_death_race_records_reset();
    for (i = 0; i < GDX_DEATH_RACE_RECORDS_MAX; i++) {
        snprintf(key, sizeof(key), "k%03d", i);
        if (gdx_death_race_record_submit(key, 0, 1000 + i) != 1) {
            break;
        }
    }
    check(gdx_death_race_records_count() == GDX_DEATH_RACE_RECORDS_MAX, "records: 512 distinct keys fill the store");
    check(gdx_death_race_record_submit("k000", 1, 500) == 0, "records: a new laps value is refused when full");
    check(gdx_death_race_record_submit("overflow", 0, 500) == 0, "records: a new key is refused when full");
    check(gdx_death_race_record_submit("k001", 0, 99999) == 0, "records: a slower time is refused when full");
    check(gdx_death_race_record_submit("k000", 0, 900) == 1, "records: a faster time for a stored pair is taken");
    check(gdx_death_race_record_get("k000", 0) == 900, "records: the faster time reads back when full");
    check(gdx_death_race_records_count() == GDX_DEATH_RACE_RECORDS_MAX, "records: a full store stays at 512");
    gdx_death_race_records_reset();
    check(gdx_death_race_records_count() == 0, "records: reset empties a full store");
}

/* ---------------------------------------------------------------------------------
 * Codec layout: the bytes of a known store, checked against the wire format by hand.
 * ------------------------------------------------------------------------------- */

static void test_codec_layout(void) {
    unsigned char buf[64];
    int paddingZero = 1;
    unsigned int i;

    gdx_death_race_records_reset();
    check(gdx_death_race_records_encoded_size() == 16, "layout: an empty store is 16 bytes");
    check(gdx_death_race_records_encode(buf, (unsigned int) sizeof(buf)) == 16,
          "layout: an empty store encodes to 16 bytes");
    check(gdx_death_race_records_encode(NULL, 1000u) == 0, "layout: a NULL out is refused");
    check(gdx_death_race_records_encode(buf, 15u) == 0, "layout: a buffer smaller than the store is refused");

    gdx_death_race_record_submit("stock-00", 7, 5000);
    check(gdx_death_race_records_encoded_size() == 56, "layout: one record is 56 bytes");
    memset(buf, 0xAA, sizeof(buf));
    check(gdx_death_race_records_encode(buf, 55u) == 0, "layout: a buffer one byte short is refused");
    check(gdx_death_race_records_encode(buf, (unsigned int) sizeof(buf)) == 56,
          "layout: one record encodes to 56 bytes");
    check(buf[56] == 0xAA && buf[63] == 0xAA, "layout: nothing is written past byte 56");

    check(memcmp(buf, "GDXD", 4) == 0, "layout: the magic GDXD is at offset 0");
    check(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0, "layout: version 1 is little-endian at offset 4");
    check(buf[8] == 1 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0, "layout: count 1 is little-endian at offset 8");
    check(memcmp(buf + 12, "stock-00", 9) == 0, "layout: the key starts at offset 12 and ends with its NUL");
    for (i = 21; i < 44; i++) {
        if (buf[i] != 0) {
            paddingZero = 0;
        }
    }
    check(paddingZero, "layout: the key padding is zero through offset 43");
    check(buf[44] == 7, "layout: laps is the byte at offset 44");
    check(buf[45] == 0 && buf[46] == 0 && buf[47] == 0, "layout: three zero bytes follow the laps");
    check(buf[48] == 0x88 && buf[49] == 0x13 && buf[50] == 0 && buf[51] == 0,
          "layout: time 5000 is little-endian at offset 48");
    check(get_u32(buf + 52) == gdx_course_registry_crc32(buf, 52u),
          "layout: the CRC-32 at offset 52 covers bytes 0..51");
}

/* ---------------------------------------------------------------------------------
 * Codec round trip.
 * ------------------------------------------------------------------------------- */

static void test_codec_round_trip(void) {
    gdx_death_race_records_reset();
    gdx_death_race_record_submit("stock-00", 0, 1000);
    gdx_death_race_record_submit("edit-abc", 3, 2000);
    gdx_death_race_record_submit("stock-54", 99, 3599998);
    check(gdx_death_race_records_encoded_size() == 136, "round trip: three records are 136 bytes");
    check(gdx_death_race_records_encode(sEncoded, (unsigned int) sizeof(sEncoded)) == 136,
          "round trip: three records encode to 136 bytes");

    gdx_death_race_records_reset();
    check(gdx_death_race_records_decode(sEncoded, 136u) == 1, "round trip: the encoded store decodes");
    check(gdx_death_race_records_count() == 3, "round trip: the decoded store has three records");
    check(gdx_death_race_record_get("stock-00", 0) == 1000, "round trip: stock-00 keeps its time");
    check(gdx_death_race_record_get("edit-abc", 3) == 2000, "round trip: edit-abc keeps its time");
    check(gdx_death_race_record_get("stock-54", 99) == 3599998, "round trip: stock-54 keeps its largest time");

    check(gdx_death_race_records_encode(sEncodedAgain, (unsigned int) sizeof(sEncodedAgain)) == 136
              && memcmp(sEncoded, sEncodedAgain, 136u) == 0,
          "round trip: encoding the decoded store gives the same bytes");

    /* An image of an empty store decodes to an empty store. The image is sealed here, not by the module. */
    image_header(sBad, 0u);
    image_seal(sBad, TEST_HEADER_BYTES + TEST_CRC_BYTES);
    set_sentinel();
    check(gdx_death_race_records_decode(sBad, TEST_HEADER_BYTES + TEST_CRC_BYTES) == 1
              && gdx_death_race_records_count() == 0,
          "round trip: an empty image decodes to an empty store");
}

/* ---------------------------------------------------------------------------------
 * Decode refusals. Each refused image leaves the sentinel store unchanged. Images are sealed here,
 * so only the check under test can refuse them.
 * ------------------------------------------------------------------------------- */

static void test_decode_refusals(void) {
    build_good_image(sGood);
    set_sentinel();
    check(gdx_death_race_records_decode(sGood, TEST_GOOD_LENGTH) == 1 && gdx_death_race_records_count() == 2,
          "refusals: the good image decodes to two records");
    check(gdx_death_race_record_get("edit-abc", 3) == 2000, "refusals: the good image keeps its times");

    /* Magic, version, CRC, and length each refuse on their own. */
    build_good_image(sBad);
    sBad[0] = 'X';
    image_seal(sBad, TEST_GOOD_LENGTH);
    check(refused(sBad, TEST_GOOD_LENGTH), "refusals: a bad magic is refused");

    build_good_image(sBad);
    put_u32(sBad + 4, 2u);
    image_seal(sBad, TEST_GOOD_LENGTH);
    check(refused(sBad, TEST_GOOD_LENGTH), "refusals: version 2 is refused");

    build_good_image(sBad);
    sBad[TEST_GOOD_LENGTH - 1] ^= 0xFFu;
    check(refused(sBad, TEST_GOOD_LENGTH), "refusals: a damaged CRC is refused");

    build_good_image(sBad);
    sBad[48] ^= 0x01u;
    check(refused(sBad, TEST_GOOD_LENGTH), "refusals: a payload byte changed under the old CRC is refused");

    check(refused(sGood, TEST_GOOD_LENGTH - 1u), "refusals: an image one byte short is refused");
    check(refused(sGood, 8u), "refusals: an image of 8 bytes is refused");
    check(refused(sGood, 12u), "refusals: an image of 12 bytes is refused");

    build_good_image(sBad);
    sBad[TEST_GOOD_LENGTH] = 0u;
    image_seal(sBad, TEST_GOOD_LENGTH + 1u);
    check(refused(sBad, TEST_GOOD_LENGTH + 1u), "refusals: an image one byte too long is refused");

    set_sentinel();
    check(gdx_death_race_records_decode(NULL, TEST_GOOD_LENGTH) == 0 && sentinel_intact(),
          "refusals: a NULL image is refused");

    /* Count above 512. */
    build_over_image(sOversize);
    check(refused(sOversize, TEST_OVER_LENGTH), "refusals: a count of 513 is refused");

    /* A key and laps pair may appear once. */
    image_header(sBad, 2u);
    image_entry(sBad, 0u, "stock-00", 0u, 1000u);
    image_entry(sBad, 1u, "stock-00", 0u, 2000u);
    image_seal(sBad, TEST_GOOD_LENGTH);
    check(refused(sBad, TEST_GOOD_LENGTH), "refusals: a repeated key and laps is refused");

    image_header(sBad, 2u);
    image_entry(sBad, 0u, "stock-00", 0u, 1000u);
    image_entry(sBad, 1u, "stock-00", 1u, 2000u);
    image_seal(sBad, TEST_GOOD_LENGTH);
    check(gdx_death_race_records_decode(sBad, TEST_GOOD_LENGTH) == 1 && gdx_death_race_records_count() == 2,
          "refusals: a repeated key at another laps value is accepted");

    /* A key needs a NUL within its 32 bytes, and it may not be empty. */
    image_header(sBad, 1u);
    memset(sBad + TEST_HEADER_BYTES, 'x', 32u);
    sBad[TEST_HEADER_BYTES + 32u] = 0u;
    put_u32(sBad + TEST_HEADER_BYTES + 36u, 1000u);
    image_seal(sBad, TEST_ONE_LENGTH);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: a key with no NUL in its 32 bytes is refused");

    image_header(sBad, 1u);
    image_entry(sBad, 0u, "", 0u, 1000u);
    image_seal(sBad, TEST_ONE_LENGTH);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: an empty key is refused");

    /* Laps 0..99 and times 1..3599998. */
    build_one_entry(sBad, "stock-00", 100u, 1000u);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: laps 100 is refused");
    build_one_entry(sBad, "stock-00", 0u, 0u);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: time 0 is refused");
    build_one_entry(sBad, "stock-00", 0u, 0xFFFFFFFFu);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: time -1 is refused");
    build_one_entry(sBad, "stock-00", 0u, 3599999u);
    check(refused(sBad, TEST_ONE_LENGTH), "refusals: time 3599999, the no-time value, is refused");

    build_one_entry(sBad, "stock-54", 99u, 3599998u);
    check(gdx_death_race_records_decode(sBad, TEST_ONE_LENGTH) == 1
              && gdx_death_race_record_get("stock-54", 99) == 3599998,
          "refusals: laps 99 and time 3599998 are accepted");
    build_one_entry(sBad, "stock-00", 0u, 1u);
    check(gdx_death_race_records_decode(sBad, TEST_ONE_LENGTH) == 1 && gdx_death_race_record_get("stock-00", 0) == 1,
          "refusals: laps 0 and time 1 are accepted");

    /* Padding is not checked: the bytes after the key's NUL and the three bytes after laps may hold anything. */
    build_one_entry(sBad, "stock-00", 0u, 1000u);
    sBad[40] = 0x55u;
    sBad[45] = 0x55u;
    image_seal(sBad, TEST_ONE_LENGTH);
    check(gdx_death_race_records_decode(sBad, TEST_ONE_LENGTH) == 1, "refusals: non-zero padding is accepted");

    /* Control: a valid image must not look refused, or the refusal checks above prove nothing. */
    image_header(sBad, 0u);
    image_seal(sBad, TEST_HEADER_BYTES + TEST_CRC_BYTES);
    check(refused(sBad, TEST_HEADER_BYTES + TEST_CRC_BYTES) == 0,
          "refusals: a valid empty image is not refused, so refused() tells a refusal from an acceptance");
}

/* ---------------------------------------------------------------------------------
 * File layer: atomic save, load, missing, corrupt, and directory cases.
 * ------------------------------------------------------------------------------- */

static void test_files(void) {
    make_test_directory();

    set_reference_store();
    check(gdx_death_race_records_save_to(TEST_PATH_FILE) == 1, "files: save_to writes the store");
    check(!file_exists(TEST_PATH_FILE ".tmp"), "files: a save leaves no tmp file behind");
    build_good_image(sGood);
    check(file_matches(TEST_PATH_FILE, sGood, TEST_GOOD_LENGTH), "files: the saved bytes are the encoded store");

    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_PATH_FILE) == 1 && reference_intact(),
          "files: load_from restores the saved store");

    /* A second save replaces the first. Three records are 136 bytes. */
    gdx_death_race_record_submit("stock-54", 99, 3599998);
    check(gdx_death_race_records_save_to(TEST_PATH_FILE) == 1, "files: a second save replaces the first");
    check(file_length(TEST_PATH_FILE) == 136L, "files: the replacement holds three records");
    check(!file_exists(TEST_PATH_FILE ".tmp"), "files: the replacing save leaves no tmp file behind");

    /* A missing file is an empty store, not an error. */
    set_reference_store();
    check(gdx_death_race_records_load_from(TEST_PATH_MISSING) == 1, "files: a missing file loads as success");
    check(gdx_death_race_records_count() == 0, "files: a missing file gives an empty store");

    /* Corrupt, truncated, oversized, and empty files are refused, and the store is left alone. */
    build_good_image(sBad);
    sBad[60] ^= 0x40u;
    check(write_bytes(TEST_PATH_CORRUPT, sBad, TEST_GOOD_LENGTH), "files: setup writes a corrupt file");
    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_PATH_CORRUPT) == 0 && sentinel_intact(),
          "files: a corrupt file is refused and the store is unchanged");

    build_good_image(sBad);
    check(write_bytes(TEST_PATH_CORRUPT, sBad, TEST_GOOD_LENGTH - 1u), "files: setup writes a truncated file");
    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_PATH_CORRUPT) == 0 && sentinel_intact(),
          "files: a truncated file is refused and the store is unchanged");

    build_over_image(sOversize);
    check(write_bytes(TEST_PATH_CORRUPT, sOversize, TEST_OVER_LENGTH), "files: setup writes an oversized file");
    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_PATH_CORRUPT) == 0 && sentinel_intact(),
          "files: an oversized file is refused and the store is unchanged");

    check(write_bytes(TEST_PATH_CORRUPT, sBad, 0u), "files: setup writes an empty file");
    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_PATH_CORRUPT) == 0 && sentinel_intact(),
          "files: an empty file is refused and the store is unchanged");

    /* NULL paths are refused. */
    check(gdx_death_race_records_save_to(NULL) == 0, "files: a NULL save path is refused");
    check(gdx_death_race_records_load_from(NULL) == 0, "files: a NULL load path is refused");

    /* A directory is not a file: saving over one fails and leaves no tmp file, and loading one fails. */
    set_reference_store();
    check(gdx_death_race_records_save_to(TEST_DIR) == 0, "files: saving over a directory fails");
    check(!file_exists(TEST_DIR ".tmp"), "files: the failed save leaves no tmp file behind");
    check(reference_intact(), "files: the failed save leaves the store unchanged");
    set_sentinel();
    check(gdx_death_race_records_load_from(TEST_DIR) == 0 && sentinel_intact(),
          "files: loading a directory fails and leaves the store unchanged");

    remove_test_files();
}

/* ---------------------------------------------------------------------------------
 * Default directory and file name.
 * ------------------------------------------------------------------------------- */

/* Saves the reference store under the current name and reports whether the default file was written. A
 * rejected name must select the default file, not keep the name set before it. */
static int rejected_selects_default(const char* name) {
    int ok;

    remove(TEST_PATH_DEFAULT);
    remove(TEST_PATH_CUSTOM);
    gdx_death_race_records_set_file_name(TEST_CUSTOM_NAME);
    gdx_death_race_records_set_file_name(name);
    set_reference_store();
    ok = gdx_death_race_records_save() == 1 && file_exists(TEST_PATH_DEFAULT) && !file_exists(TEST_PATH_CUSTOM);
    remove(TEST_PATH_DEFAULT);
    return ok;
}

static void test_directory_and_names(void) {
    char name127[128];
    char name128[129];
    char path[256];

    make_test_directory();
    gdx_death_race_records_set_directory(TEST_DIR);

    /* The default name lives in the set directory. */
    gdx_death_race_records_set_file_name(NULL);
    set_reference_store();
    check(gdx_death_race_records_save() == 1, "names: the default save succeeds");
    check(file_exists(TEST_PATH_DEFAULT), "names: the default file is death_race_records.bin in the set directory");
    set_sentinel();
    check(gdx_death_race_records_load() == 1 && reference_intact(), "names: the default load reads the default file");

    /* A custom name selects its own file in the same directory. */
    gdx_death_race_records_set_file_name(TEST_CUSTOM_NAME);
    gdx_death_race_records_reset();
    gdx_death_race_record_submit("stock-54", 99, 3599998);
    check(gdx_death_race_records_save() == 1 && file_exists(TEST_PATH_CUSTOM),
          "names: a custom name is saved in the set directory");
    set_sentinel();
    check(gdx_death_race_records_load() == 1 && gdx_death_race_records_count() == 1
              && gdx_death_race_record_get("stock-54", 99) == 3599998,
          "names: a custom name loads its own file");
    remove(TEST_PATH_CUSTOM);

    /* Rejected names select the default file: a path separator, an empty name, NULL, or a name too long. */
    check(rejected_selects_default("sub/evil.bin"), "names: a forward slash selects the default file");
    check(rejected_selects_default("sub\\evil.bin"), "names: a backslash selects the default file");
    check(rejected_selects_default(""), "names: an empty name selects the default file");
    check(rejected_selects_default(NULL), "names: a NULL name selects the default file");

    /* 127 characters is the longest name that fits; 128 is too long. */
    memset(name127, 'n', 123u);
    memcpy(name127 + 123, ".bin", 5u);
    memset(name128, 'n', 124u);
    memcpy(name128 + 124, ".bin", 5u);

    gdx_death_race_records_set_file_name(name127);
    set_reference_store();
    snprintf(path, sizeof(path), "%s/%s", TEST_DIR, name127);
    /* The name is kept, so the save never falls back to the default file. Whether the OS opens the long path
       depends on the working directory (Windows MAX_PATH), so a failed save only has to leave no file. */
    remove(TEST_PATH_DEFAULT);
    {
        const int saved = gdx_death_race_records_save();

        check(!file_exists(TEST_PATH_DEFAULT) && (saved == file_exists(path)),
              "names: a 127-character name is kept as given");
    }
    remove(path);
    check(rejected_selects_default(name128), "names: a 128-character name is too long and selects the default file");

    check(!file_exists(TEST_DIR "/evil.bin"), "names: no rejected name writes evil.bin");

    gdx_death_race_records_set_file_name(NULL);
    gdx_death_race_records_set_directory(NULL);
    remove_test_files();
}

int main(void) {
    remove_test_files();
    test_oracle();
    test_race_rules();
    test_lap_count();
    test_course_keys();
    test_records();
    test_codec_layout();
    test_codec_round_trip();
    test_decode_refusals();
    test_files();
    test_directory_and_names();
    remove_test_files();

    if (sFailures != 0) {
        fprintf(stderr, "%d death race test(s) failed\n", sFailures);
        return 1;
    }
    printf("All death race tests passed\n");
    return 0;
}
