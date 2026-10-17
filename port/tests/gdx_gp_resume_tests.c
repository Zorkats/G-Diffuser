/* port/tests/gdx_gp_resume_tests.c -- unit tests for the Save GP in progress codec and file layer.
 * Standalone: links gdx_gp_resume.c and gdx_course_registry.c (for the CRC-32 it reuses).
 */

#include "../gdx_gp_resume.h"
#include "../gdx_course_registry.h"

#include <stdio.h>
#include <string.h>

#define TEST_FILE "gdx_gp_resume_test.bin"
#define TEST_TMP "gdx_gp_resume_test.bin.tmp"

static int sFailures;

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

/* Fixtures. A stock Queen Cup state and an X Cup state, both inside every range. */
static void fill_stock_state(GdxGpResumeState* s) {
    int i;
    memset(s, 0, sizeof(*s));
    s->cup = 1;
    s->difficulty = 2;
    s->numPlayers = 1;
    s->courseIndex = 8; /* Queen Cup track 2 */
    s->lastRandomCourseIndex = 7;
    s->raceCounter = 2;
    s->randSeed1 = -123456;
    s->randMask1 = 0xDEADBEEFu;
    s->randSeed2 = 987654;
    s->randMask2 = 0x80000001u;
    s->lives[0] = 3;
    s->lives[1] = 2;
    s->lives[2] = 1;
    s->lives[3] = 0;
    s->playerCharacter[0] = 5;
    s->playerSkin[0] = 3;
    for (i = 0; i < GDX_GP_RESUME_RACERS; i++) {
        s->racers[i].points = i * 3;
        s->racers[i].character = i % 36;
        s->racers[i].skin = i % 4;
        s->engine[i] = (float) i / 40.0f;
    }
}

static void fill_x_state(GdxGpResumeState* s) {
    fill_stock_state(s);
    s->cup = 4;
    s->difficulty = 0;
    s->courseIndex = 50; /* X Cup track 3 */
    s->lastRandomCourseIndex = 49;
    s->raceCounter = 1;
}

static int states_equal(const GdxGpResumeState* a, const GdxGpResumeState* b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Little-endian write into an encoded record, for crafting damaged records. */
static void poke_u32(unsigned char* buf, size_t offset, uint32_t value) {
    buf[offset + 0] = (unsigned char) (value & 0xFFu);
    buf[offset + 1] = (unsigned char) ((value >> 8) & 0xFFu);
    buf[offset + 2] = (unsigned char) ((value >> 16) & 0xFFu);
    buf[offset + 3] = (unsigned char) ((value >> 24) & 0xFFu);
}

/* Writes a field and recomputes the CRC, so only the range check can reject the record. */
static void poke_and_reseal(unsigned char* buf, size_t offset, uint32_t value) {
    poke_u32(buf, offset, value);
    poke_u32(buf, 580, gdx_course_registry_crc32(buf, 580));
}

static int encode_state(const GdxGpResumeState* s, unsigned char* buf) {
    return gdx_gp_resume_encode(s, buf, GDX_GP_RESUME_FILE_SIZE) == GDX_GP_RESUME_OK;
}

/* ---------------------------------------------------------------------------------
 * Round trip and layout.
 * ------------------------------------------------------------------------------- */

static void test_round_trip(void) {
    GdxGpResumeState in;
    GdxGpResumeState out;
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE];

    fill_stock_state(&in);
    check(gdx_gp_resume_encode(&in, buf, sizeof(buf)) == GDX_GP_RESUME_OK, "stock state encodes");
    check(gdx_gp_resume_decode(buf, sizeof(buf), &out) == GDX_GP_RESUME_OK, "stock state decodes");
    check(states_equal(&in, &out), "stock state round trips field for field");

    fill_x_state(&in);
    check(encode_state(&in, buf), "X Cup state encodes");
    check(gdx_gp_resume_decode(buf, sizeof(buf), &out) == GDX_GP_RESUME_OK && states_equal(&in, &out),
          "X Cup state round trips field for field");

    check(GDX_GP_RESUME_FILE_SIZE == 584u, "record is 584 bytes");
    check(memcmp(buf, "GDXG", 4) == 0, "record starts with the GDXG magic");
    check(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0, "record carries version 1");
    check(buf[8] == 0x48 && buf[9] == 0x02 && buf[10] == 0 && buf[11] == 0, "record declares size 584");
}

static void test_encode_refuses_bad_input(void) {
    GdxGpResumeState s;
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE];

    fill_stock_state(&s);
    check(gdx_gp_resume_encode(NULL, buf, sizeof(buf)) == GDX_GP_RESUME_ERR_ARG, "encode rejects a NULL state");
    check(gdx_gp_resume_encode(&s, NULL, sizeof(buf)) == GDX_GP_RESUME_ERR_ARG, "encode rejects a NULL buffer");
    check(gdx_gp_resume_encode(&s, buf, sizeof(buf) - 1) == GDX_GP_RESUME_ERR_ARG,
          "encode rejects a buffer one byte short");
    s.lives[0] = 9;
    check(gdx_gp_resume_encode(&s, buf, sizeof(buf)) == GDX_GP_RESUME_ERR_RANGE,
          "encode refuses a state with lives out of range");
}

/* ---------------------------------------------------------------------------------
 * Damage: magic, version, size, truncation, CRC.
 * ------------------------------------------------------------------------------- */

static void test_damage(void) {
    GdxGpResumeState in;
    GdxGpResumeState out;
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE];
    unsigned char copy[GDX_GP_RESUME_FILE_SIZE + 1];

    fill_stock_state(&in);
    encode_state(&in, buf);

    memcpy(copy, buf, sizeof(buf));
    copy[0] = 'X';
    check(gdx_gp_resume_decode(copy, sizeof(buf), &out) == GDX_GP_RESUME_ERR_MAGIC, "bad magic is refused");

    memcpy(copy, buf, sizeof(buf));
    copy[4] = 2;
    check(gdx_gp_resume_decode(copy, sizeof(buf), &out) == GDX_GP_RESUME_ERR_VERSION, "bad version is refused");

    memcpy(copy, buf, sizeof(buf));
    poke_u32(copy, 8, 600u);
    check(gdx_gp_resume_decode(copy, sizeof(buf), &out) == GDX_GP_RESUME_ERR_SIZE,
          "a declared size other than 584 is refused");

    check(gdx_gp_resume_decode(buf, 5, &out) == GDX_GP_RESUME_ERR_TRUNCATED, "a 5-byte file is truncated");
    check(gdx_gp_resume_decode(buf, 100, &out) == GDX_GP_RESUME_ERR_TRUNCATED, "a 100-byte file is truncated");
    check(gdx_gp_resume_decode(buf, GDX_GP_RESUME_FILE_SIZE - 1, &out) == GDX_GP_RESUME_ERR_TRUNCATED,
          "a file one byte short is truncated");

    memcpy(copy, buf, sizeof(buf));
    copy[GDX_GP_RESUME_FILE_SIZE] = 0;
    check(gdx_gp_resume_decode(copy, GDX_GP_RESUME_FILE_SIZE + 1, &out) == GDX_GP_RESUME_ERR_SIZE,
          "a file with trailing bytes is refused");

    memcpy(copy, buf, sizeof(buf));
    copy[104] ^= 0x01; /* racer 0 points */
    check(gdx_gp_resume_decode(copy, sizeof(buf), &out) == GDX_GP_RESUME_ERR_CRC,
          "a flipped payload byte fails the CRC");

    memcpy(copy, buf, sizeof(buf));
    copy[580] ^= 0xFF; /* the stored CRC itself */
    check(gdx_gp_resume_decode(copy, sizeof(buf), &out) == GDX_GP_RESUME_ERR_CRC,
          "a damaged CRC field is refused");

    check(gdx_gp_resume_decode(NULL, sizeof(buf), &out) == GDX_GP_RESUME_ERR_ARG, "decode rejects a NULL buffer");
    check(gdx_gp_resume_decode(buf, sizeof(buf), NULL) == GDX_GP_RESUME_ERR_ARG, "decode rejects a NULL output");
}

/* ---------------------------------------------------------------------------------
 * Out-of-range fields. Each record is resealed, so the CRC passes and only ranges reject it.
 * ------------------------------------------------------------------------------- */

static void test_out_of_range(void) {
    GdxGpResumeState in;
    GdxGpResumeState out;
    unsigned char buf[GDX_GP_RESUME_FILE_SIZE];
    unsigned char bad[GDX_GP_RESUME_FILE_SIZE];
    float engineOver = 1.5f;
    uint32_t engineBits;
    uint32_t nanBits = 0x7FC00000u;

    fill_stock_state(&in);
    encode_state(&in, buf);

    /* Retries also count as race starts, so a counter past 5 is still a valid record. */
    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 32, 7u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_OK && out.raceCounter == 7,
          "race counter 7 after retries is accepted");

    /* Offsets follow the table in gdx_gp_resume.h. */
    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 12, 5u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "cup 5 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 16, 4u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "difficulty 4 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 20, 0u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "zero players is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 24, 14u); /* cup 1 at course 14 is track 2 of cup 2 */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE,
          "a course that belongs to another cup is refused");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 28, 54u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE,
          "last random course 54 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 32, 0u);
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "race counter 0 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 52, 6u); /* lives[0] */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "lives 6 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 68, 36u); /* playerCharacter[0] */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE,
          "player character 36 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 84, 4u); /* playerSkin[0] */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "player skin 4 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 100, 0xFFFFFFFFu); /* racer 0 points = -1 */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "negative points are refused");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 104, 36u); /* racer 0 character */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE,
          "racer character 36 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 108, 4u); /* racer 0 skin */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "racer skin 4 is out of range");

    memcpy(&engineBits, &engineOver, sizeof(engineBits));
    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 460, engineBits); /* engine[0] = 1.5 */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "engine 1.5 is out of range");

    memcpy(bad, buf, sizeof(buf));
    poke_and_reseal(bad, 460, nanBits); /* engine[0] = NaN */
    check(gdx_gp_resume_decode(bad, sizeof(buf), &out) == GDX_GP_RESUME_ERR_RANGE, "engine NaN is refused");

    /* A bad state cannot slip through the encoder either. */
    fill_stock_state(&in);
    in.courseIndex = 24;
    check(gdx_gp_resume_encode(&in, buf, sizeof(buf)) == GDX_GP_RESUME_ERR_RANGE,
          "encode refuses course 24 (an editor slot) for a stock cup");
}

/* ---------------------------------------------------------------------------------
 * File layer: atomic save, load, clear, and the default path.
 * ------------------------------------------------------------------------------- */

static void test_file_layer(void) {
    GdxGpResumeState in;
    GdxGpResumeState out;
    FILE* file;
    char path[1024];
    size_t nameLength;

    fill_x_state(&in);
    remove(TEST_FILE);
    remove(TEST_TMP);

    check(gdx_gp_resume_load_from(TEST_FILE, &out) == GDX_GP_RESUME_ERR_IO, "loading a missing file is an IO error");
    check(gdx_gp_resume_save_to(TEST_FILE, &in) == GDX_GP_RESUME_OK, "save writes the record");

    file = fopen(TEST_TMP, "rb");
    check(file == NULL, "save leaves no temp file behind");
    if (file != NULL) {
        fclose(file);
    }

    check(gdx_gp_resume_load_from(TEST_FILE, &out) == GDX_GP_RESUME_OK && states_equal(&in, &out),
          "loaded record equals the saved state");

    fill_stock_state(&in);
    check(gdx_gp_resume_save_to(TEST_FILE, &in) == GDX_GP_RESUME_OK, "a second save replaces the first");
    check(gdx_gp_resume_load_from(TEST_FILE, &out) == GDX_GP_RESUME_OK && states_equal(&in, &out),
          "the replacement is what loads");

    /* A valid 12-byte header and nothing after it: magic, version 1, declared size 584. */
    {
        static const unsigned char headerOnly[12] = { 'G', 'D', 'X', 'G', 1, 0, 0, 0, 0x48, 0x02, 0, 0 };
        file = fopen(TEST_FILE, "wb");
        if (file != NULL) {
            fwrite(headerOnly, 1, sizeof(headerOnly), file);
            fclose(file);
        }
    }
    check(gdx_gp_resume_load_from(TEST_FILE, &out) == GDX_GP_RESUME_ERR_TRUNCATED,
          "a header-only file is refused as truncated");

    check(gdx_gp_resume_clear_from(TEST_FILE) == GDX_GP_RESUME_OK, "clear removes the file");
    check(gdx_gp_resume_load_from(TEST_FILE, &out) == GDX_GP_RESUME_ERR_IO, "a cleared file no longer loads");
    check(gdx_gp_resume_clear_from(TEST_FILE) == GDX_GP_RESUME_OK, "clearing twice is harmless");

    check(gdx_gp_resume_default_path(path, sizeof(path)) == GDX_GP_RESUME_OK, "default path resolves");
    nameLength = strlen(GDX_GP_RESUME_FILE_NAME);
    check(strlen(path) > nameLength && strcmp(path + strlen(path) - nameLength, GDX_GP_RESUME_FILE_NAME) == 0,
          "default path ends in gp_resume.bin");
    check(gdx_gp_resume_default_path(path, 4) == GDX_GP_RESUME_ERR_ARG, "default path refuses a small buffer");
}

static void test_error_text(void) {
    check(strcmp(gdx_gp_resume_error_text(GDX_GP_RESUME_OK), "ok") == 0, "error text: ok");
    check(strlen(gdx_gp_resume_error_text(GDX_GP_RESUME_ERR_CRC)) > 0, "error text: CRC has a message");
    check(strcmp(gdx_gp_resume_error_text((GdxGpResumeError) 99), "unknown error") == 0,
          "error text: unknown code falls back");
}

int main(void) {
    test_round_trip();
    test_encode_refuses_bad_input();
    test_damage();
    test_out_of_range();
    test_file_layer();
    test_error_text();

    if (sFailures != 0) {
        fprintf(stderr, "%d GP resume test(s) failed\n", sFailures);
        return 1;
    }
    printf("All GP resume tests passed\n");
    return 0;
}
