/* Unit tests for port/gdx_host_placement.c, compiled UNMODIFIED alongside this file. */
#include "../gdx_host_placement.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond, msg)                                   \
    do {                                                   \
        sChecks++;                                         \
        if (!(cond)) {                                     \
            sFailures++;                                   \
            printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        }                                                  \
    } while (0)

/* A host address with the given high and low 32 bits. */
static uintptr_t at(uint32_t high, uint32_t low) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    return ((uintptr_t)high << 32) | (uintptr_t)low;
#else
    (void)high;
    return (uintptr_t)low;
#endif
}

static void test_window_edges(void) {
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x00296B00u), 1) != 0, "a bare RDRAM offset collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x0FFFFFFFu), 1) != 0, "the last segmented address collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x10000000u), 1) == 0, "the band's first byte is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x7FFFFFFFu), 1) == 0, "the band's last byte is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x80000000u), 1) != 0, "KSEG0 RDRAM collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x8FFFFFFFu), 1) != 0,
          "a segmented address with bit 31 set collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x90000000u), 1) == 0, "0x90000000 is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x9FFFFFFFu), 1) == 0, "0x9FFFFFFF is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0xA0000000u), 1) != 0, "KSEG1 RDRAM collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0xA0FFFFFFu), 1) != 0, "the last KSEG1 RDRAM byte collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0xA1000000u), 1) == 0, "past KSEG1 RDRAM is clear");
}

static void test_window_spans(void) {
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x00296B00u), 0) == 0, "an empty range never collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x7FF00000u), 0x00100000u) == 0,
          "a span ending exactly at the band's end is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x7FF00000u), 0x00100001u) != 0,
          "a span one byte past the band's end collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x0FF00000u), 0x00200000u) != 0,
          "a span that starts below the band collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x9F000000u), 0x01000000u) == 0,
          "a span ending exactly where KSEG1 RDRAM starts is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x9F000000u), 0x01000001u) != 0,
          "a span one byte into KSEG1 RDRAM collides");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0xFFF00000u), 0x00100000u) == 0,
          "a span ending exactly at 4 GB is clear");
    CHECK(gdx_low32_window_collides(at(0x1AA, 0xFFF00000u), 0x00100001u) != 0,
          "a span that wraps past 4 GB into low32 0 collides");
#if UINTPTR_MAX > 0xFFFFFFFFu
    CHECK(gdx_low32_window_collides(at(0x1AA, 0x20000000u), (size_t)0x100000000ULL) != 0,
          "a span of 4 GB always collides");
    CHECK(gdx_low32_window_collides(at(0x7FF6, 0xFF290000u), 0x02000000u) != 0,
          "the exe image layout that broke segment 9 collides");
#endif
}

static int all_zero(const unsigned char* bytes, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void check_placed(size_t size, const char* what) {
    unsigned char* buffer = (unsigned char*)gdx_host_alloc_placed(size);
    char message[160];
    uint32_t low32;

    snprintf(message, sizeof(message), "%s: allocated", what);
    CHECK(buffer != NULL, message);
    if (buffer == NULL) {
        return;
    }

    low32 = (uint32_t)((uintptr_t)buffer & 0xFFFFFFFFu);
    snprintf(message, sizeof(message), "%s: low32 %08X starts inside the band", what, (unsigned)low32);
    CHECK(low32 >= GDX_PLACEMENT_BAND_BEGIN, message);
    snprintf(message, sizeof(message), "%s: low32 %08X keeps the whole buffer inside the band", what,
             (unsigned)low32);
    CHECK((uint64_t)low32 + (uint64_t)size <= (uint64_t)GDX_PLACEMENT_BAND_END, message);
    snprintf(message, sizeof(message), "%s: meets no N64 address window", what);
    CHECK(gdx_low32_window_collides((uintptr_t)buffer, size) == 0, message);
    snprintf(message, sizeof(message), "%s: zeroed", what);
    CHECK(all_zero(buffer, size), message);

    /* Checked one write at a time: for size 1 both ends are the same byte. */
    buffer[0] = 0x5A;
    snprintf(message, sizeof(message), "%s: writable at the first byte", what);
    CHECK(buffer[0] == 0x5A, message);
    buffer[size - 1] = 0xA5;
    snprintf(message, sizeof(message), "%s: writable at the last byte", what);
    CHECK(buffer[size - 1] == 0xA5, message);

    gdx_host_free_placed(buffer, size);
}

static void test_placement(void) {
#if UINTPTR_MAX > 0xFFFFFFFFu
    int round;
    void* held[24];
    int heldCount = 0;
    int i;

    check_placed(0x1000000u, "the 16 MB RDRAM size");
    check_placed(0x2000000u, "a 32 MB ROM size");
    check_placed(0xA3F1D0u + 0x44u, "the audio table blob size");
    check_placed(1u, "one byte");
    check_placed(4097u, "a page and a byte");

    CHECK(gdx_host_alloc_placed(0) == NULL, "size 0 gives NULL");
    gdx_host_free_placed(NULL, 16u); /* must not crash */

    /* The system places each reservation differently, so repeat; and keep some buffers alive so later
     * reservations land elsewhere instead of reusing the same spot. */
    for (round = 0; round < 40; round++) {
        check_placed(0x1000000u, "16 MB, repeated");
        if (heldCount < (int)(sizeof(held) / sizeof(held[0]))) {
            held[heldCount] = gdx_host_alloc_placed(0x00300000u);
            CHECK(held[heldCount] != NULL, "a held 3 MB buffer");
            if (held[heldCount] != NULL) {
                CHECK(gdx_low32_window_collides((uintptr_t)held[heldCount], 0x00300000u) == 0,
                      "a held 3 MB buffer meets no N64 address window");
                heldCount++;
            }
        }
    }
    for (i = 0; i < heldCount; i++) {
        gdx_host_free_placed(held[i], 0x00300000u);
    }
#endif
}

int main(void) {
    test_window_edges();
    test_window_spans();
    test_placement();

    if (sFailures == 0) {
        printf("gdx_host_placement_tests: %d checks passed\n", sChecks);
        return 0;
    }
    printf("gdx_host_placement_tests: %d of %d checks FAILED\n", sFailures, sChecks);
    return 1;
}
