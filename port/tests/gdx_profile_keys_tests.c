/* Unit tests for port/gdx_profile_keys.c, compiled UNMODIFIED alongside this file and gdx_course_registry.c. */
#include "../gdx_profile_keys.h"
#include "../gdx_course_registry.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(condition, what)                                          \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, what);             \
            sFailures++;                                                \
        }                                                               \
    } while (0)

static unsigned char sFile[GDX_PROFILE_KEYS_FILE_MAX_BYTES + 16];

static void make_id(unsigned char id[GDX_PROFILE_ID_BYTES], int n) {
    memset(id, 0, GDX_PROFILE_ID_BYTES);
    id[0] = (unsigned char) (n + 1);
    id[15] = (unsigned char) (0xA0 + n);
}

static void make_seed(unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES], int n) {
    int i;

    for (i = 0; i < GDX_PROFILE_KEYS_SEED_BYTES; i++) {
        seed[i] = (unsigned char) (n * 31 + i);
    }
}

static int all_zero(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*) p;
    size_t i;

    for (i = 0; i < n; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_set_find_remove(void) {
    GdxProfileKeys keys;
    unsigned char id[GDX_PROFILE_ID_BYTES];
    unsigned char zero[GDX_PROFILE_ID_BYTES];
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    int i;

    memset(&keys, 0, sizeof(keys));
    memset(zero, 0, sizeof(zero));
    make_seed(seed, 0);
    CHECK(gdx_profile_keys_set(&keys, zero, seed) == GDX_PROFILE_ERR_ARGS, "an all-zero id is refused");
    CHECK(gdx_profile_keys_set(NULL, zero, seed) == GDX_PROFILE_ERR_ARGS, "NULL keys are refused");
    make_id(id, 0);
    CHECK(gdx_profile_keys_set(&keys, id, NULL) == GDX_PROFILE_ERR_ARGS, "a NULL seed is refused");
    CHECK(keys.count == 0, "refusals add nothing");

    for (i = 0; i < GDX_PROFILE_KEYS_CAPACITY; i++) {
        make_id(id, i);
        make_seed(seed, i);
        CHECK(gdx_profile_keys_set(&keys, id, seed) == i, "a new id takes the next index");
    }
    make_id(id, GDX_PROFILE_KEYS_CAPACITY);
    CHECK(gdx_profile_keys_set(&keys, id, seed) == GDX_PROFILE_ERR_FULL, "a full store refuses a new id");

    make_id(id, 3);
    make_seed(seed, 99);
    CHECK(gdx_profile_keys_set(&keys, id, seed) == 3, "a known id is replaced in place, even when full");
    CHECK(memcmp(keys.seeds[3], seed, sizeof(seed)) == 0, "the replaced seed is stored");
    CHECK(keys.count == GDX_PROFILE_KEYS_CAPACITY, "a replace does not grow the store");

    CHECK(gdx_profile_keys_find(&keys, id) == 3, "find returns the index");
    CHECK(gdx_profile_keys_find(&keys, zero) == -1, "find misses an unknown id");
    CHECK(gdx_profile_keys_find(NULL, id) == -1, "find with NULL keys misses");

    CHECK(gdx_profile_keys_remove(&keys, id) == GDX_PROFILE_OK, "remove succeeds");
    CHECK(keys.count == GDX_PROFILE_KEYS_CAPACITY - 1, "remove shrinks the store");
    CHECK(gdx_profile_keys_find(&keys, id) == -1, "a removed id is gone");
    make_id(id, GDX_PROFILE_KEYS_CAPACITY - 1);
    CHECK(gdx_profile_keys_find(&keys, id) == 3, "the last entry fills the hole");
    make_seed(seed, GDX_PROFILE_KEYS_CAPACITY - 1);
    CHECK(memcmp(keys.seeds[3], seed, sizeof(seed)) == 0, "the moved entry keeps its seed");
    CHECK(all_zero(keys.seeds[GDX_PROFILE_KEYS_CAPACITY - 1], GDX_PROFILE_KEYS_SEED_BYTES) &&
              all_zero(keys.ids[GDX_PROFILE_KEYS_CAPACITY - 1], GDX_PROFILE_ID_BYTES),
          "the freed slot is wiped");
    make_id(id, 3);
    CHECK(gdx_profile_keys_remove(&keys, id) == GDX_PROFILE_ERR_NOT_FOUND, "a second remove finds nothing");

    make_id(id, GDX_PROFILE_KEYS_CAPACITY - 2);
    CHECK(gdx_profile_keys_find(&keys, id) == GDX_PROFILE_KEYS_CAPACITY - 2, "the tail entry is where expected");
    CHECK(gdx_profile_keys_remove(&keys, id) == GDX_PROFILE_OK, "removing the last entry works");
    CHECK(all_zero(keys.seeds[GDX_PROFILE_KEYS_CAPACITY - 2], GDX_PROFILE_KEYS_SEED_BYTES),
          "removing the last entry wipes it");
}

static void fill(GdxProfileKeys* keys, int count) {
    unsigned char id[GDX_PROFILE_ID_BYTES];
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    int i;

    memset(keys, 0, sizeof(*keys));
    for (i = 0; i < count; i++) {
        make_id(id, i);
        make_seed(seed, i);
        gdx_profile_keys_set(keys, id, seed);
    }
}

static int keys_equal(const GdxProfileKeys* a, const GdxProfileKeys* b) {
    int i;

    if (a->count != b->count) {
        return 0;
    }
    for (i = 0; i < a->count; i++) {
        if ((memcmp(a->ids[i], b->ids[i], GDX_PROFILE_ID_BYTES) != 0) ||
            (memcmp(a->seeds[i], b->seeds[i], GDX_PROFILE_KEYS_SEED_BYTES) != 0)) {
            return 0;
        }
    }
    return 1;
}

static void test_round_trip(void) {
    GdxProfileKeys in;
    GdxProfileKeys out;
    int count;
    int len;

    for (count = 0; count <= GDX_PROFILE_KEYS_CAPACITY; count += 5) {
        fill(&in, count);
        len = gdx_profile_keys_encode(&in, sFile, sizeof(sFile));
        CHECK(len == 8 + count * GDX_PROFILE_KEYS_ENTRY_BYTES + 4, "the file length follows the count");
        CHECK((gdx_profile_keys_decode(sFile, (size_t) len, &out) == GDX_PROFILE_OK) && keys_equal(&in, &out),
              "a file round-trips");
    }
    fill(&in, GDX_PROFILE_KEYS_CAPACITY);
    len = gdx_profile_keys_encode(&in, sFile, sizeof(sFile));
    CHECK(len == GDX_PROFILE_KEYS_FILE_MAX_BYTES, "a full store is the largest file");
    CHECK(memcmp(sFile, "GPK1", 4) == 0, "the file starts with the magic");
    CHECK(gdx_profile_keys_encode(&in, sFile, (size_t) len - 1) == GDX_PROFILE_ERR_FULL, "a short buffer is refused");
    in.count = GDX_PROFILE_KEYS_CAPACITY + 1;
    CHECK(gdx_profile_keys_encode(&in, sFile, sizeof(sFile)) == GDX_PROFILE_ERR_ARGS, "a bad count is not written");
    in.count = -1;
    CHECK(gdx_profile_keys_encode(&in, sFile, sizeof(sFile)) == GDX_PROFILE_ERR_ARGS, "a negative count is not written");
    CHECK(gdx_profile_keys_encode(NULL, sFile, sizeof(sFile)) == GDX_PROFILE_ERR_ARGS, "NULL keys are not written");
}

/* Rewrites the trailing CRC, so a test reaches the check after it. */
static void reseal(unsigned char* data, size_t length) {
    const uint32_t crc = gdx_course_registry_crc32(data, length - 4);

    data[length - 4] = (unsigned char) crc;
    data[length - 3] = (unsigned char) (crc >> 8);
    data[length - 2] = (unsigned char) (crc >> 16);
    data[length - 1] = (unsigned char) (crc >> 24);
}

static int decode_wipes(const unsigned char* data, size_t length, int expected) {
    GdxProfileKeys out;

    memset(&out, 0x77, sizeof(out));
    return (gdx_profile_keys_decode(data, length, &out) == expected) && all_zero(&out, sizeof(out));
}

static void test_bad_files(void) {
    GdxProfileKeys in;
    int len;

    fill(&in, 3);
    len = gdx_profile_keys_encode(&in, sFile, sizeof(sFile));

    CHECK(decode_wipes(sFile, 11, GDX_PROFILE_ERR_TRUNCATED), "a stub file is truncated");
    CHECK(decode_wipes(NULL, 64, GDX_PROFILE_ERR_ARGS), "NULL data is refused");
    CHECK(gdx_profile_keys_decode(sFile, (size_t) len, NULL) == GDX_PROFILE_ERR_ARGS, "a NULL out is refused");

    sFile[20] ^= 1;
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_CRC), "a flipped byte fails the CRC");
    sFile[20] ^= 1;

    sFile[0] = 'X';
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_MAGIC), "a wrong magic is refused");
    sFile[0] = 'G';

    sFile[4] = 2;
    reseal(sFile, (size_t) len);
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_VERSION), "a later version is refused");
    sFile[4] = 1;

    sFile[6] = 4;
    reseal(sFile, (size_t) len);
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_LAYOUT), "a count that disagrees with the length fails");
    sFile[6] = 3;
    reseal(sFile, (size_t) len);

    /* Entry 1 takes the id of entry 0. */
    memcpy(sFile + 8 + GDX_PROFILE_KEYS_ENTRY_BYTES, sFile + 8, GDX_PROFILE_ID_BYTES);
    reseal(sFile, (size_t) len);
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_LAYOUT), "a repeated id fails");

    len = gdx_profile_keys_encode(&in, sFile, sizeof(sFile));
    memset(sFile + 8 + 2 * GDX_PROFILE_KEYS_ENTRY_BYTES, 0, GDX_PROFILE_ID_BYTES);
    reseal(sFile, (size_t) len);
    CHECK(decode_wipes(sFile, (size_t) len, GDX_PROFILE_ERR_LAYOUT), "an all-zero id fails");

    {
        unsigned char big[8 + 17 * GDX_PROFILE_KEYS_ENTRY_BYTES + 4];
        int i;

        memset(big, 0, sizeof(big));
        memcpy(big, "GPK1", 4);
        big[4] = 1;
        big[6] = 17;
        for (i = 0; i < 17; i++) {
            big[8 + i * GDX_PROFILE_KEYS_ENTRY_BYTES] = (unsigned char) (i + 1);
        }
        reseal(big, sizeof(big));
        CHECK(decode_wipes(big, sizeof(big), GDX_PROFILE_ERR_LAYOUT), "more entries than the capacity fail");
    }
}

static void test_wipe(void) {
    unsigned char buffer[40];

    memset(buffer, 0xEE, sizeof(buffer));
    gdx_profile_keys_wipe(buffer + 4, 32);
    CHECK((buffer[3] == 0xEE) && all_zero(buffer + 4, 32) && (buffer[36] == 0xEE), "wipe clears exactly n bytes");
    gdx_profile_keys_wipe(buffer, 0);
    CHECK(buffer[0] == 0xEE, "a zero-length wipe writes nothing");
}

int main(void) {
    test_set_find_remove();
    test_round_trip();
    test_bad_files();
    test_wipe();
    if (sFailures != 0) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
