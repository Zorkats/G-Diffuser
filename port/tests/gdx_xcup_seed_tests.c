// port/tests/gdx_xcup_seed_tests.c -- unit tests for port/gdx_xcup_seed.c: X Cup seed codes, the
// queue of codes to use, and the cup record.
//
// Standalone: links gdx_xcup_seed.c and nothing else. In the game, course.c owns the four
// sRandomCourseInit* globals; this file defines them so the test links without the decomp.
// Prints one summary line. Exit status 0 when every check passes, 1 otherwise.

#include "../gdx_xcup_seed.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int32_t sRandomCourseInitSeed1 = 0;
int32_t sRandomCourseInitMask1 = 0;
int32_t sRandomCourseInitSeed2 = 0;
int32_t sRandomCourseInitMask2 = 0;

// Expected codes, derived by hand from the format: four big-endian u32 (16 bytes), 130 bits with
// two zero pad bits at the end, 26 base32 digits, and a dash after every fifth digit.
static const char kCodeZero[] = "00000-00000-00000-00000-00000-0";
// seed1 = 1: data bit 31 lands in digit 6 with weight 8.
static const char kCodeSeed1[] = "00000-08000-00000-00000-00000-0";
// seed1 = 0x08000000: data bit 4 lands in digit 0 with weight 1.
static const char kCodeSeed1Bit27[] = "10000-00000-00000-00000-00000-0";
// All four values -1: digits 0 to 24 are all ones (Z); digit 25 keeps its three data bits (W).
static const char kCodeAllOnes[] = "ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-W";
static const char kTwoCodes[] = "ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-W\n00000-00000-00000-00000-00000-0";
static const char kCupThree[] =
    "00000-08000-00000-00000-00000-0\n"
    "ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-W\n"
    "00000-00000-00000-00000-00000-0";

// Capacity test input: GDX_XCUP_QUEUE_CAPACITY + 2 codes, each followed by a space, then NUL.
#define CAPACITY_TEST_CODES (GDX_XCUP_QUEUE_CAPACITY + 2)
static char sCapacityText[CAPACITY_TEST_CODES * (GDX_XCUP_CODE_LENGTH + 1) + 1];

static int sChecks = 0;
static int sFailures = 0;

#define CHECK(cond)                                        \
    do {                                                   \
        sChecks++;                                         \
        if (!(cond)) {                                     \
            sFailures++;                                   \
            printf("FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                  \
    } while (0)

static void SetSeeds(int32_t seed1, int32_t mask1, int32_t seed2, int32_t mask2) {
    sRandomCourseInitSeed1 = seed1;
    sRandomCourseInitMask1 = mask1;
    sRandomCourseInitSeed2 = seed2;
    sRandomCourseInitMask2 = mask2;
}

static int SameSeeds(const int32_t got[4], int32_t seed1, int32_t mask1, int32_t seed2, int32_t mask2) {
    return got[0] == seed1 && got[1] == mask1 && got[2] == seed2 && got[3] == mask2;
}

// Takes the front queue entry into got. Returns 1 when an entry was taken, 0 when the queue was empty.
static int ConsumeFront(int32_t got[4]) {
    return gdx_xcup_consume_pending_seed(&got[0], &got[1], &got[2], &got[3]);
}

// Copies src to dst with the dashes dropped and the letters lowercased. dst needs strlen(src) + 1.
static void MangleCode(const char* src, char* dst) {
    size_t i;
    size_t o = 0;

    for (i = 0; src[i] != '\0'; i++) {
        char c = src[i];

        if (c == '-') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

// Runs first: nothing has been captured yet in this process, so no code can be formatted.
static void TestNoCodeBeforeCapture(void) {
    char code[GDX_XCUP_CODE_BUF_SIZE];

    memset(code, 'x', sizeof(code));
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 0);
    CHECK(code[0] == '\0');
    CHECK(gdx_xcup_current_seed_code(NULL, 0) == 0);
    CHECK(gdx_xcup_queue_count() == 0);
    CHECK(gdx_xcup_has_pending_seed() == 0);
}

static void TestKnownVectors(void) {
    char code[GDX_XCUP_CODE_BUF_SIZE];

    SetSeeds(0, 0, 0, 0);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeZero) == 0);

    SetSeeds(1, 0, 0, 0);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeSeed1) == 0);

    SetSeeds(134217728, 0, 0, 0);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeSeed1Bit27) == 0);

    SetSeeds(-1, -1, -1, -1);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeAllOnes) == 0);
    CHECK(strlen(code) == (size_t) GDX_XCUP_CODE_LENGTH);
}

static void TestRoundTrip(void) {
    static const int32_t kValues[4] = {0x12345678, -1698898192, 0x0F0F0F0F, 0x7FFFFFFF};
    char code[GDX_XCUP_CODE_BUF_SIZE];
    char mangled[GDX_XCUP_CODE_BUF_SIZE];
    int32_t got[4];

    SetSeeds(kValues[0], kValues[1], kValues[2], kValues[3]);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_current_seed_code(code, sizeof(code)) == 1);

    gdx_xcup_queue_clear();
    CHECK(gdx_xcup_set_pending_code(code) == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, kValues[0], kValues[1], kValues[2], kValues[3]));

    // The same code, lowercased and without dashes, decodes to the same values.
    MangleCode(code, mangled);
    CHECK(gdx_xcup_set_pending_code(mangled) == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, kValues[0], kValues[1], kValues[2], kValues[3]));
}

static void TestDecodeTolerance(void) {
    char mangled[GDX_XCUP_CODE_BUF_SIZE];
    int32_t got[4];

    gdx_xcup_queue_clear();

    MangleCode(kCodeAllOnes, mangled);
    CHECK(gdx_xcup_set_pending_code(mangled) == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, -1, -1, -1, -1));

    // O reads as 0; I and L read as 1 (Crockford aliases).
    CHECK(gdx_xcup_set_pending_code("OOOOO-O8OOO-OOOOO-OOOOO-OOOOO-O") == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 1, 0, 0, 0));
    CHECK(gdx_xcup_set_pending_code("I0000-00000-00000-00000-00000-0") == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 134217728, 0, 0, 0));
    CHECK(gdx_xcup_set_pending_code("l0000-00000-00000-00000-00000-0") == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 134217728, 0, 0, 0));

    // Spaces stand in for dashes.
    CHECK(gdx_xcup_set_pending_code("00000 08000 00000 00000 00000 0") == 1);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 1, 0, 0, 0));

    gdx_xcup_queue_clear();
}

static void TestRejects(void) {
    gdx_xcup_queue_clear();

    CHECK(gdx_xcup_set_pending_code(NULL) == 0);
    CHECK(gdx_xcup_set_pending_code("") == 0);
    CHECK(gdx_xcup_set_pending_code("00000-08000") == 0);
    // 27 digits.
    CHECK(gdx_xcup_set_pending_code("00000-08000-00000-00000-00000-00") == 0);
    // U is not in the alphabet.
    CHECK(gdx_xcup_set_pending_code("00000-08000-00000-00000-0000U-0") == 0);
    CHECK(gdx_xcup_set_pending_code("00000-08000-00000-00000-00000-!") == 0);
    CHECK(gdx_xcup_queue_count() == 0);
}

static void TestQueueFifo(void) {
    char code[GDX_XCUP_CODE_BUF_SIZE];
    int32_t got[4];

    gdx_xcup_queue_clear();
    CHECK(gdx_xcup_has_pending_seed() == 0);
    CHECK(gdx_xcup_set_pending_code(kCodeAllOnes) == 1);
    CHECK(gdx_xcup_set_pending_code(kCodeZero) == 1);
    CHECK(gdx_xcup_set_pending_code(kCodeSeed1) == 1);
    CHECK(gdx_xcup_queue_count() == 3);
    CHECK(gdx_xcup_has_pending_seed() == 1);

    // Index 0 is the next entry used; out-of-range indices and small buffers fail.
    CHECK(gdx_xcup_queue_code(0, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeAllOnes) == 0);
    CHECK(gdx_xcup_queue_code(2, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeSeed1) == 0);
    CHECK(gdx_xcup_queue_code(3, code, sizeof(code)) == 0);
    CHECK(gdx_xcup_queue_code(-1, code, sizeof(code)) == 0);
    CHECK(gdx_xcup_queue_code(0, code, GDX_XCUP_CODE_BUF_SIZE - 1) == 0);

    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, -1, -1, -1, -1));
    CHECK(gdx_xcup_queue_count() == 2);
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 0, 0, 0, 0));
    CHECK(ConsumeFront(got) == 1);
    CHECK(SameSeeds(got, 1, 0, 0, 0));
    CHECK(gdx_xcup_queue_count() == 0);
    CHECK(gdx_xcup_has_pending_seed() == 0);

    // An empty queue takes nothing and leaves the outputs alone.
    got[0] = 7;
    got[1] = 7;
    got[2] = 7;
    got[3] = 7;
    CHECK(ConsumeFront(got) == 0);
    CHECK(SameSeeds(got, 7, 7, 7, 7));
}

static void TestAddText(void) {
    static const char kMixedInput[] =
        "ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-W, 00000-00000-00000-00000-00000-0;hello\n"
        "00000-08000-00000-00000-00000-0\t";
    char code[GDX_XCUP_CODE_BUF_SIZE];
    int added = -1;
    int rejected = -1;

    gdx_xcup_queue_clear();
    gdx_xcup_queue_add_text(kMixedInput, &added, &rejected);
    CHECK(added == 3);
    CHECK(rejected == 1);
    CHECK(gdx_xcup_queue_count() == 3);
    CHECK(gdx_xcup_queue_code(0, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeAllOnes) == 0);
    CHECK(gdx_xcup_queue_code(1, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeZero) == 0);
    CHECK(gdx_xcup_queue_code(2, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeSeed1) == 0);

    // NULL text adds nothing; NULL count pointers are allowed.
    gdx_xcup_queue_add_text(NULL, &added, &rejected);
    CHECK(added == 0);
    CHECK(rejected == 0);
    gdx_xcup_queue_add_text(kCodeZero, NULL, NULL);
    CHECK(gdx_xcup_queue_count() == 4);

    gdx_xcup_queue_clear();
}

static void TestCapacity(void) {
    int i;
    size_t o = 0;
    int added = -1;
    int rejected = -1;

    for (i = 0; i < CAPACITY_TEST_CODES; i++) {
        memcpy(sCapacityText + o, kCodeZero, GDX_XCUP_CODE_LENGTH);
        o += GDX_XCUP_CODE_LENGTH;
        sCapacityText[o++] = ' ';
    }
    sCapacityText[o] = '\0';

    gdx_xcup_queue_clear();
    gdx_xcup_queue_add_text(sCapacityText, &added, &rejected);
    CHECK(added == GDX_XCUP_QUEUE_CAPACITY);
    CHECK(rejected == 2);
    CHECK(gdx_xcup_queue_count() == GDX_XCUP_QUEUE_CAPACITY);
    CHECK(gdx_xcup_set_pending_code(kCodeSeed1) == 0);

    gdx_xcup_queue_clear();
}

static void TestRemoveAndClear(void) {
    char code[GDX_XCUP_CODE_BUF_SIZE];

    gdx_xcup_queue_clear();
    CHECK(gdx_xcup_set_pending_code(kCodeAllOnes) == 1);
    CHECK(gdx_xcup_set_pending_code(kCodeZero) == 1);
    CHECK(gdx_xcup_set_pending_code(kCodeSeed1) == 1);

    gdx_xcup_queue_remove(1);
    CHECK(gdx_xcup_queue_count() == 2);
    CHECK(gdx_xcup_queue_code(0, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeAllOnes) == 0);
    CHECK(gdx_xcup_queue_code(1, code, sizeof(code)) == 1);
    CHECK(strcmp(code, kCodeSeed1) == 0);

    // Indices out of range are ignored.
    gdx_xcup_queue_remove(5);
    gdx_xcup_queue_remove(-1);
    CHECK(gdx_xcup_queue_count() == 2);

    gdx_xcup_queue_clear();
    CHECK(gdx_xcup_queue_count() == 0);
    CHECK(gdx_xcup_has_pending_seed() == 0);
}

static void TestCupRecord(void) {
    char out[128];

    // Slots are written in order. Slot 0 starts the cup; later captures keep it.
    SetSeeds(1, 0, 0, 0);
    gdx_xcup_note_seed_captured(0, 1);
    SetSeeds(-1, -1, -1, -1);
    gdx_xcup_note_seed_captured(1, 0);
    SetSeeds(0, 0, 0, 0);
    gdx_xcup_note_seed_captured(2, 0);
    CHECK(gdx_xcup_cup_codes(out, sizeof(out)) == 3);
    CHECK(strcmp(out, kCupThree) == 0);

    // Slots outside 0..5 change nothing, even with newCup set.
    gdx_xcup_note_seed_captured(6, 0);
    gdx_xcup_note_seed_captured(-1, 1);
    CHECK(gdx_xcup_cup_codes(out, sizeof(out)) == 3);
    CHECK(strcmp(out, kCupThree) == 0);

    // A new cup clears the record before it stores its first course.
    SetSeeds(-1, -1, -1, -1);
    gdx_xcup_note_seed_captured(0, 1);
    CHECK(gdx_xcup_cup_codes(out, sizeof(out)) == 1);
    CHECK(strcmp(out, kCodeAllOnes) == 0);

    // Slot 5 joins slot 0 without a clear, and the list stays in slot order.
    SetSeeds(0, 0, 0, 0);
    gdx_xcup_note_seed_captured(5, 0);
    CHECK(gdx_xcup_cup_codes(out, sizeof(out)) == 2);
    CHECK(strcmp(out, kTwoCodes) == 0);

    // Whole codes only: a buffer too small for the next code stops the list.
    CHECK(gdx_xcup_cup_codes(out, 40) == 1);
    CHECK(strcmp(out, kCodeAllOnes) == 0);
    CHECK(gdx_xcup_cup_codes(out, 1) == 0);
    CHECK(out[0] == '\0');
    CHECK(gdx_xcup_cup_codes(out, 0) == 0);
    CHECK(gdx_xcup_cup_codes(NULL, sizeof(out)) == 0);
}

int main(void) {
    // This runs first: any capture sets the flag for the whole process.
    TestNoCodeBeforeCapture();
    TestKnownVectors();
    TestRoundTrip();
    TestDecodeTolerance();
    TestRejects();
    TestQueueFifo();
    TestAddText();
    TestCapacity();
    TestRemoveAndClear();
    TestCupRecord();

    if (sFailures == 0) {
        printf("gdx_xcup_seed_tests: PASS, %d checks\n", sChecks);
        return 0;
    }
    printf("gdx_xcup_seed_tests: FAIL, %d of %d checks failed\n", sFailures, sChecks);
    return 1;
}
