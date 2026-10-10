/* Standalone unit-test harness for port/gdx_rom_courses.c. Console exe, no game deps, no
 * libultraship, no decomp headers: the module compiles unmodified alongside this file (see
 * port/CMakeLists.txt's gdx_rom_courses_tests) and is driven entirely through its public API.
 *
 * Every fixture is SYNTHESIZED. No ROM, patched or otherwise, is read or committed: the harness
 * builds its own big-endian image containing a course table it filled itself, then re-interleaves
 * that image into the .v64 and .n64 orderings to check the loader normalises all three to the
 * same payloads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gdx_rom_courses.h"

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

#define RX_ROM_BYTES (GDX_ROMCOURSE_TABLE_OFFSET + GDX_ROMCOURSE_TABLE_BYTES + 0x1000)
#define RX_CLEAN_PATH "gdx_romcourse_clean.tmp"
#define RX_PATCHED_PATH "gdx_romcourse_patched.tmp"

static void rx_wr32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void rx_wr16be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void rx_wrf32be(uint8_t* p, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    rx_wr32be(p, bits);
}

/* Deterministic per-slot values, so every assertion can recompute what it expects. */
static float rx_pos(int slot, int cp, int axis) {
    return (float)(slot * 100 + cp * 7 + axis) * 1.5f;
}

static int16_t rx_radius(int slot, int cp, int side) {
    return (int16_t)(50 + ((slot * 13 + cp * 3 + side * 7) % 900));
}

static int32_t rx_tsi(int slot, int cp) {
    return (int32_t)(0x18000000 + slot * 0x10000 + cp);
}

static int16_t rx_bank(int slot, int cp) {
    return (int16_t)((slot * 31 + cp * 5) % 4096 - 2048);
}

static int8_t rx_attr(int slot, int cp, int which) {
    return (int8_t)((slot + cp + which) % 200 - 100);
}

static void rx_build_course(uint8_t* block, int slot, int controlPointCount) {
    int cp;
    int a;
    char name[GDX_ROMCOURSE_NAME_LEN + 1];

    memset(block, 0, GDX_ROMCOURSE_STRIDE);
    block[0] = 4; /* CREATOR_NINTENDO */
    block[1] = (uint8_t)(int8_t)controlPointCount;
    block[2] = (uint8_t)(int8_t)(slot % 8);  /* venue */
    block[3] = (uint8_t)(int8_t)(slot % 6);  /* skybox */
    rx_wr32be(block + 0x004, 0xA5000000u + (uint32_t)slot);
    block[0x008] = 1; /* flag */

    snprintf(name, sizeof(name), "TRACK%02d", slot);
    memcpy(block + 0x009, name, strlen(name));
    /* Retail ROMs leave junk after the terminator; reproduce that so the name trim is exercised. */
    block[0x009 + strlen(name)] = 0x00;
    block[0x009 + strlen(name) + 1] = 0x7F;
    block[0x009 + strlen(name) + 2] = 0xFF;

    block[0x01F] = (uint8_t)(int8_t)(slot % 14); /* bgm */

    for (cp = 0; cp < GDX_ROMCOURSE_MAX_CONTROL_POINTS; cp++) {
        uint8_t* p = block + 0x020 + (size_t)cp * 0x14;
        rx_wrf32be(p + 0, rx_pos(slot, cp, 0));
        rx_wrf32be(p + 4, rx_pos(slot, cp, 1));
        rx_wrf32be(p + 8, rx_pos(slot, cp, 2));
        rx_wr16be(p + 12, (uint16_t)rx_radius(slot, cp, 0));
        rx_wr16be(p + 14, (uint16_t)rx_radius(slot, cp, 1));
        rx_wr32be(p + 16, (uint32_t)rx_tsi(slot, cp));
        rx_wr16be(block + 0x520 + (size_t)cp * 2, (uint16_t)rx_bank(slot, cp));
        for (a = 0; a < 9; a++) {
            block[0x5A0 + (size_t)a * 64 + cp] = (uint8_t)rx_attr(slot, cp, a);
        }
    }
}

static uint8_t* rx_build_rom(void) {
    uint8_t* rom = (uint8_t*)calloc(1, RX_ROM_BYTES);
    int slot;

    if (rom == NULL) {
        return NULL;
    }
    rom[0] = 0x80;
    rom[1] = 0x37;
    rom[2] = 0x12;
    rom[3] = 0x40;
    for (slot = 0; slot < GDX_ROMCOURSE_COUNT; slot++) {
        rx_build_course(rom + GDX_ROMCOURSE_TABLE_OFFSET + (size_t)slot * GDX_ROMCOURSE_STRIDE, slot,
                        4 + slot % 60);
    }
    return rom;
}

static int rx_write(const char* path, const uint8_t* data, int64_t size) {
    FILE* f = fopen(path, "wb");
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

/* Re-interleave a big-endian image into .v64 (16-bit swapped) or .n64 (32-bit reversed). */
static uint8_t* rx_reinterleave(const uint8_t* be, int64_t size, int wordBytes) {
    uint8_t* out = (uint8_t*)malloc((size_t)size);
    int64_t i;

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, be, (size_t)size);
    if (wordBytes == 2) {
        for (i = 0; i + 1 < size; i += 2) {
            uint8_t t = out[i];
            out[i] = out[i + 1];
            out[i + 1] = t;
        }
    } else {
        for (i = 0; i + 3 < size; i += 4) {
            uint8_t a = out[i];
            uint8_t b = out[i + 1];
            uint8_t c = out[i + 2];
            uint8_t d = out[i + 3];
            out[i] = d;
            out[i + 1] = c;
            out[i + 2] = b;
            out[i + 3] = a;
        }
    }
    return out;
}

/* ---------------------------------------------------------------------------------
 * Cases
 * ------------------------------------------------------------------------------- */

static void CaseListsEveryCourse(void) {
    uint8_t* rom = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    GdxRomCourseEntry entries[GDX_ROMCOURSE_COUNT];
    int rc;
    int count;

    checkTrue("rom built", rom != NULL);
    if (rom == NULL) {
        return;
    }
    checkTrue("rom written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);

    rc = gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set);
    checkEqLong("open without baseline", rc, GDX_ROMCOURSE_OK);
    if (rc != GDX_ROMCOURSE_OK) {
        printf("      (strerror: %s)\n", gdx_romcourse_strerror(rc));
        free(rom);
        remove(RX_PATCHED_PATH);
        return;
    }
    checkEqLong("count query (NULL out)", gdx_romcourse_list(set, NULL, 0, 0), GDX_ROMCOURSE_COUNT);
    count = gdx_romcourse_list(set, entries, GDX_ROMCOURSE_COUNT, 0);
    checkEqLong("listed all slots", count, GDX_ROMCOURSE_COUNT);
    checkEqLong("no baseline reported", gdx_romcourse_has_baseline(set), 0);
    checkEqLong("no changes without baseline", gdx_romcourse_changed_count(set), 0);
    checkEqLong("no non-course claim without baseline", gdx_romcourse_touches_non_course(set), 0);

    if (count == GDX_ROMCOURSE_COUNT) {
        int i;
        int nameOk = 1;
        int metaOk = 1;
        for (i = 0; i < count; i++) {
            char expect[GDX_ROMCOURSE_NAME_LEN + 1];
            snprintf(expect, sizeof(expect), "TRACK%02d", i);
            if (strcmp(entries[i].name, expect) != 0) {
                nameOk = 0;
                printf("      slot %d name '%s' expected '%s'\n", i, entries[i].name, expect);
                break;
            }
            if (entries[i].controlPointCount != 4 + i % 60 || entries[i].venue != i % 8 ||
                entries[i].skybox != i % 6 || entries[i].bgm != i % 14 ||
                entries[i].checksum != 0xA5000000u + (uint32_t)i) {
                metaOk = 0;
                break;
            }
            if (entries[i].symbol == NULL) {
                metaOk = 0;
                break;
            }
        }
        checkTrue("names cut at NUL and trimmed", nameOk);
        checkTrue("metadata byteswapped correctly", metaOk);
    }

    gdx_romcourse_close(set);
    free(rom);
    remove(RX_PATCHED_PATH);
}

static void CasePayloadRoundTrip(void) {
    uint8_t* rom = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    uint8_t* payload = (uint8_t*)malloc(GDX_ROMCOURSE_PAYLOAD_SIZE);
    int rc;

    checkTrue("rom built", rom != NULL);
    checkTrue("payload buffer", payload != NULL);
    if (rom == NULL || payload == NULL) {
        free(rom);
        free(payload);
        return;
    }
    checkTrue("rom written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set);
    checkEqLong("open", rc, GDX_ROMCOURSE_OK);
    if (rc != GDX_ROMCOURSE_OK) {
        free(rom);
        free(payload);
        remove(RX_PATCHED_PATH);
        return;
    }

    checkEqLong("undersized buffer refused",
                gdx_romcourse_payload(set, 0, payload, GDX_ROMCOURSE_PAYLOAD_SIZE - 1),
                GDX_ROMCOURSE_ERR_BAD_ARGS);
    checkEqLong("out-of-range index refused",
                gdx_romcourse_payload(set, GDX_ROMCOURSE_COUNT, payload, GDX_ROMCOURSE_PAYLOAD_SIZE),
                GDX_ROMCOURSE_ERR_BAD_ARGS);

    {
        int slot;
        int fieldsOk = 1;
        int tailOk = 1;
        for (slot = 0; slot < GDX_ROMCOURSE_COUNT && fieldsOk && tailOk; slot++) {
            int cp;
            int a;
            int32_t i32;
            uint32_t u32;
            int16_t i16;
            float f;

            checkEqLong("payload built", gdx_romcourse_payload(set, slot, payload, GDX_ROMCOURSE_PAYLOAD_SIZE),
                        GDX_ROMCOURSE_OK);

            memcpy(&u32, payload + 0x004, 4);
            if (u32 != 0xA5000000u + (uint32_t)slot) {
                fieldsOk = 0;
                printf("      slot %d checksum 0x%08X\n", slot, (unsigned)u32);
                break;
            }
            for (cp = 0; cp < GDX_ROMCOURSE_MAX_CONTROL_POINTS; cp++) {
                const uint8_t* p = payload + 0x020 + (size_t)cp * 0x14;
                memcpy(&f, p + 0, 4);
                if (f != rx_pos(slot, cp, 0)) { fieldsOk = 0; break; }
                memcpy(&f, p + 4, 4);
                if (f != rx_pos(slot, cp, 1)) { fieldsOk = 0; break; }
                memcpy(&f, p + 8, 4);
                if (f != rx_pos(slot, cp, 2)) { fieldsOk = 0; break; }
                memcpy(&i16, p + 12, 2);
                if (i16 != rx_radius(slot, cp, 0)) { fieldsOk = 0; break; }
                memcpy(&i16, p + 14, 2);
                if (i16 != rx_radius(slot, cp, 1)) { fieldsOk = 0; break; }
                memcpy(&i32, p + 16, 4);
                if (i32 != rx_tsi(slot, cp)) { fieldsOk = 0; break; }
                memcpy(&i16, payload + 0x520 + (size_t)cp * 2, 2);
                if (i16 != rx_bank(slot, cp)) { fieldsOk = 0; break; }
                for (a = 0; a < 9; a++) {
                    if ((int8_t)payload[0x5A0 + (size_t)a * 64 + cp] != rx_attr(slot, cp, a)) {
                        fieldsOk = 0;
                        break;
                    }
                }
                if (!fieldsOk) {
                    break;
                }
            }
            {
                int32_t i;
                for (i = GDX_ROMCOURSE_STRIDE; i < GDX_ROMCOURSE_PAYLOAD_SIZE; i++) {
                    if (payload[i] != 0) {
                        tailOk = 0;
                        break;
                    }
                }
            }
        }
        checkTrue("every field byteswapped to host order", fieldsOk);
        checkTrue("ghost/records tail zeroed", tailOk);
    }

    gdx_romcourse_close(set);
    free(rom);
    free(payload);
    remove(RX_PATCHED_PATH);
}

/* .z64, .v64 and .n64 of the same disk must produce byte-identical payloads. */
static void CaseByteOrderVariantsAgree(void) {
    uint8_t* rom = rx_build_rom();
    uint8_t* v64 = NULL;
    uint8_t* n64 = NULL;
    uint8_t* base = (uint8_t*)malloc(GDX_ROMCOURSE_PAYLOAD_SIZE);
    uint8_t* other = (uint8_t*)malloc(GDX_ROMCOURSE_PAYLOAD_SIZE);
    GdxRomCourseSet* set = NULL;
    int pass;

    checkTrue("buffers", rom != NULL && base != NULL && other != NULL);
    if (rom == NULL || base == NULL || other == NULL) {
        free(rom);
        free(base);
        free(other);
        return;
    }
    v64 = rx_reinterleave(rom, RX_ROM_BYTES, 2);
    n64 = rx_reinterleave(rom, RX_ROM_BYTES, 4);
    checkTrue("re-interleaved images", v64 != NULL && n64 != NULL);

    checkTrue("z64 written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    checkEqLong("z64 open", gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set), GDX_ROMCOURSE_OK);
    if (set != NULL) {
        checkEqLong("z64 payload", gdx_romcourse_payload(set, 7, base, GDX_ROMCOURSE_PAYLOAD_SIZE),
                    GDX_ROMCOURSE_OK);
        gdx_romcourse_close(set);
        set = NULL;
    }

    for (pass = 0; pass < 2; pass++) {
        const uint8_t* img = (pass == 0) ? v64 : n64;
        const char* label = (pass == 0) ? "v64" : "n64";
        if (img == NULL) {
            continue;
        }
        checkTrue("variant written", rx_write(RX_PATCHED_PATH, img, RX_ROM_BYTES) == 0);
        checkEqLong("variant open", gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set), GDX_ROMCOURSE_OK);
        if (set == NULL) {
            printf("      (%s failed to open)\n", label);
            continue;
        }
        checkEqLong("variant payload", gdx_romcourse_payload(set, 7, other, GDX_ROMCOURSE_PAYLOAD_SIZE),
                    GDX_ROMCOURSE_OK);
        checkTrue("payload identical across interleavings",
                  memcmp(base, other, GDX_ROMCOURSE_PAYLOAD_SIZE) == 0);
        gdx_romcourse_close(set);
        set = NULL;
    }

    free(rom);
    free(v64);
    free(n64);
    free(base);
    free(other);
    remove(RX_PATCHED_PATH);
}

static void CaseChangedCourseDetected(void) {
    uint8_t* clean = rx_build_rom();
    uint8_t* patched = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    GdxRomCourseEntry entries[GDX_ROMCOURSE_COUNT];
    int rc;

    checkTrue("roms built", clean != NULL && patched != NULL);
    if (clean == NULL || patched == NULL) {
        free(clean);
        free(patched);
        return;
    }
    /* Replace slot 18 the way an FZEP hack would: same slot, different course. */
    rx_build_course(patched + GDX_ROMCOURSE_TABLE_OFFSET + 18 * GDX_ROMCOURSE_STRIDE, 99, 40);

    checkTrue("clean written", rx_write(RX_CLEAN_PATH, clean, RX_ROM_BYTES) == 0);
    checkTrue("patched written", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);

    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        int count;
        checkEqLong("baseline reported", gdx_romcourse_has_baseline(set), 1);
        checkEqLong("exactly one course changed", gdx_romcourse_changed_count(set), 1);
        checkEqLong("course-only hack: nothing outside the table", gdx_romcourse_touches_non_course(set), 0);
        count = gdx_romcourse_list(set, entries, GDX_ROMCOURSE_COUNT, 1);
        checkEqLong("changedOnly lists one", count, 1);
        if (count == 1) {
            checkEqLong("changed slot index", entries[0].index, 18);
            checkTrue("changed slot flagged", entries[0].changed != 0);
            checkTrue("changed slot carries the new name", strcmp(entries[0].name, "TRACK99") == 0);
        }
        gdx_romcourse_close(set);
    }

    free(clean);
    free(patched);
    remove(RX_CLEAN_PATH);
    remove(RX_PATCHED_PATH);
}

static void CaseNonCourseChangeDetected(void) {
    uint8_t* clean = rx_build_rom();
    uint8_t* patched = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    int rc;

    checkTrue("roms built", clean != NULL && patched != NULL);
    if (clean == NULL || patched == NULL) {
        free(clean);
        free(patched);
        return;
    }
    /* A byte well before the course table: a music or texture edit, not a course edit. */
    patched[0x100000] ^= 0xFF;

    checkTrue("clean written", rx_write(RX_CLEAN_PATH, clean, RX_ROM_BYTES) == 0);
    checkTrue("patched written", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);

    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("no course changed", gdx_romcourse_changed_count(set), 0);
        checkEqLong("non-course change detected", gdx_romcourse_touches_non_course(set), 1);
        gdx_romcourse_close(set);
    }

    /* Same again with a byte AFTER the table, to prove both sides of the range are compared. */
    memcpy(patched, clean, RX_ROM_BYTES);
    patched[GDX_ROMCOURSE_TABLE_OFFSET + GDX_ROMCOURSE_TABLE_BYTES + 0x40] ^= 0xFF;
    checkTrue("patched rewritten", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);
    set = NULL;
    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline (after-table edit)", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("no course changed", gdx_romcourse_changed_count(set), 0);
        checkEqLong("change after the table detected", gdx_romcourse_touches_non_course(set), 1);
        gdx_romcourse_close(set);
    }

    free(clean);
    free(patched);
    remove(RX_CLEAN_PATH);
    remove(RX_PATCHED_PATH);
}

/* FZEP's "Recalculate Header Checksum" rewrites the two N64 header CRC words so the patched ROM
 * still boots. That says nothing about content, so it must not raise the non-course warning --
 * while every other header byte still must. */
static void CaseHeaderChecksumNotNonCourse(void) {
    uint8_t* clean = rx_build_rom();
    uint8_t* patched = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    int rc;

    checkTrue("roms built", clean != NULL && patched != NULL);
    if (clean == NULL || patched == NULL) {
        free(clean);
        free(patched);
        return;
    }

    /* Only the CRC words at 0x10..0x17, exactly what a header recalculation touches. */
    patched[0x10] ^= 0xFF;
    patched[0x14] ^= 0xFF;
    patched[0x17] ^= 0xFF;
    checkTrue("clean written", rx_write(RX_CLEAN_PATH, clean, RX_ROM_BYTES) == 0);
    checkTrue("patched written", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);

    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("header CRC alone is not a non-course change", gdx_romcourse_touches_non_course(set), 0);
        checkEqLong("header CRC change reported separately", gdx_romcourse_header_checksum_changed(set), 1);
        checkEqLong("no course changed", gdx_romcourse_changed_count(set), 0);
        gdx_romcourse_close(set);
        set = NULL;
    }

    /* A course edit plus the header recalculation: still course-only, which is the exact shape of
     * a distributed FZEP track hack. */
    rx_build_course(patched + GDX_ROMCOURSE_TABLE_OFFSET + 11 * GDX_ROMCOURSE_STRIDE, 77, 30);
    checkTrue("patched rewritten", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("course hack + header CRC stays course-only", gdx_romcourse_touches_non_course(set), 0);
        checkEqLong("the course change is still seen", gdx_romcourse_changed_count(set), 1);
        gdx_romcourse_close(set);
        set = NULL;
    }

    /* Any OTHER header byte must still count. 0x20 is the internal ROM name. */
    memcpy(patched, clean, RX_ROM_BYTES);
    patched[0x20] ^= 0xFF;
    checkTrue("patched rewritten", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    checkEqLong("open with baseline", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("other header bytes still count", gdx_romcourse_touches_non_course(set), 1);
        checkEqLong("no header CRC change claimed", gdx_romcourse_header_checksum_changed(set), 0);
        gdx_romcourse_close(set);
        set = NULL;
    }

    /* The byte immediately before and after the CRC window must count too, proving the exclusion
     * window is exactly 0x10..0x17 and not wider. */
    memcpy(patched, clean, RX_ROM_BYTES);
    patched[0x0F] ^= 0xFF;
    checkTrue("patched rewritten", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("byte before the CRC window counts", gdx_romcourse_touches_non_course(set), 1);
        gdx_romcourse_close(set);
        set = NULL;
    }
    memcpy(patched, clean, RX_ROM_BYTES);
    patched[0x18] ^= 0xFF;
    checkTrue("patched rewritten", rx_write(RX_PATCHED_PATH, patched, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, RX_CLEAN_PATH, &set);
    if (rc == GDX_ROMCOURSE_OK) {
        checkEqLong("byte after the CRC window counts", gdx_romcourse_touches_non_course(set), 1);
        gdx_romcourse_close(set);
    }

    free(clean);
    free(patched);
    remove(RX_CLEAN_PATH);
    remove(RX_PATCHED_PATH);
}

static void CaseRefusals(void) {
    uint8_t* rom = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    uint8_t small[64];

    checkEqLong("NULL path refused", gdx_romcourse_open(NULL, NULL, &set), GDX_ROMCOURSE_ERR_BAD_ARGS);
    checkEqLong("NULL out refused", gdx_romcourse_open(RX_PATCHED_PATH, NULL, NULL),
                GDX_ROMCOURSE_ERR_BAD_ARGS);

    memset(small, 0, sizeof(small));
    checkTrue("tiny file written", rx_write(RX_PATCHED_PATH, small, sizeof(small)) == 0);
    checkEqLong("too-small file refused", gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set),
                GDX_ROMCOURSE_ERR_NOT_ROM);
    checkTrue("no handle leaked", set == NULL);

    checkTrue("rom built", rom != NULL);
    if (rom == NULL) {
        remove(RX_PATCHED_PATH);
        return;
    }

    /* Right size, wrong magic. */
    rom[0] = 0x11;
    checkTrue("bad-magic rom written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    checkEqLong("unknown interleaving refused", gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set),
                GDX_ROMCOURSE_ERR_NOT_ROM);
    rom[0] = 0x80;

    /* Right magic, no course table. */
    memset(rom + GDX_ROMCOURSE_TABLE_OFFSET, 0xEE, GDX_ROMCOURSE_TABLE_BYTES);
    checkTrue("tableless rom written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    checkEqLong("missing course table refused", gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set),
                GDX_ROMCOURSE_ERR_NO_COURSES);
    checkTrue("no handle leaked", set == NULL);

    /* A valid ROM but an unreadable baseline. */
    free(rom);
    rom = rx_build_rom();
    checkTrue("good rom written", rom != NULL && rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    checkEqLong("missing baseline refused",
                gdx_romcourse_open(RX_PATCHED_PATH, "gdx_romcourse_does_not_exist.tmp", &set),
                GDX_ROMCOURSE_ERR_NO_BASELINE);
    checkTrue("no handle leaked", set == NULL);

    free(rom);
    remove(RX_PATCHED_PATH);
}

/* A slot whose creator id or control-point count is nonsense is skipped rather than offered, but
 * its neighbours still list. */
static void CaseImplausibleSlotSkipped(void) {
    uint8_t* rom = rx_build_rom();
    GdxRomCourseSet* set = NULL;
    int rc;

    checkTrue("rom built", rom != NULL);
    if (rom == NULL) {
        return;
    }
    rom[GDX_ROMCOURSE_TABLE_OFFSET + 3 * GDX_ROMCOURSE_STRIDE + 0] = 9;    /* not CREATOR_NINTENDO */
    rom[GDX_ROMCOURSE_TABLE_OFFSET + 5 * GDX_ROMCOURSE_STRIDE + 1] = 0x7F; /* 127 control points */

    checkTrue("rom written", rx_write(RX_PATCHED_PATH, rom, RX_ROM_BYTES) == 0);
    rc = gdx_romcourse_open(RX_PATCHED_PATH, NULL, &set);
    checkEqLong("open", rc, GDX_ROMCOURSE_OK);
    if (rc == GDX_ROMCOURSE_OK) {
        uint8_t* payload = (uint8_t*)malloc(GDX_ROMCOURSE_PAYLOAD_SIZE);
        checkEqLong("two slots skipped", gdx_romcourse_list(set, NULL, 0, 0), GDX_ROMCOURSE_COUNT - 2);
        if (payload != NULL) {
            checkEqLong("payload for an implausible slot refused",
                        gdx_romcourse_payload(set, 3, payload, GDX_ROMCOURSE_PAYLOAD_SIZE),
                        GDX_ROMCOURSE_ERR_NO_COURSES);
            checkEqLong("payload for a good slot still works",
                        gdx_romcourse_payload(set, 4, payload, GDX_ROMCOURSE_PAYLOAD_SIZE),
                        GDX_ROMCOURSE_OK);
            free(payload);
        }
        gdx_romcourse_close(set);
    }

    free(rom);
    remove(RX_PATCHED_PATH);
}

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } cases[] = {
        { "lists every course slot with correct metadata", CaseListsEveryCourse },
        { "payload byteswap round-trip + zeroed tail", CasePayloadRoundTrip },
        { "z64 / v64 / n64 produce identical payloads", CaseByteOrderVariantsAgree },
        { "changed course detected against the baseline", CaseChangedCourseDetected },
        { "non-course edits detected (both sides of the table)", CaseNonCourseChangeDetected },
        { "header CRC recalculation is not a non-course change", CaseHeaderChecksumNotNonCourse },
        { "malformed inputs refused", CaseRefusals },
        { "implausible slots skipped, neighbours unaffected", CaseImplausibleSlotSkipped },
    };
    int numCases = (int)(sizeof(cases) / sizeof(cases[0]));
    int i, failedCases = 0;

    printf("=== G-Diffuser ROM course reader unit-test harness (port/gdx_rom_courses.c) ===\n\n");
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
