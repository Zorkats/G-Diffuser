/* port/tests/gdx_input_actions_tests.c -- standalone tests for the 3A action layer.
 *
 * Builds gdx_input_actions.c against an in-memory CVar stub, so no libultraship is linked. The
 * stub stores integers by name and counts reads, which lets the tests check both the persisted
 * names and the cache that avoids re-reading them every frame.
 */

#include "PR/os_pfs.h"
#include "controller.h"

#include "gdx_input_actions.h"

#include <stdio.h>
#include <stdlib.h>

/* A failing check prints its expression and exits, so the test does not depend on NDEBUG. */
#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

/* ---------------------------------------------------------------------------------------------
 * CVar stub (libultraship consolevariablebridge.h). No string.h: names are compared by hand.
 * ------------------------------------------------------------------------------------------- */

#define STUB_SLOTS 64
#define STUB_NAME_MAX 96

static struct {
    char name[STUB_NAME_MAX];
    int value;
} sStore[STUB_SLOTS];
static int sStoreCount;
static int sReadCount;

static int stub_name_equal(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void stub_copy_name(char* dst, const char* src) {
    int k = 0;

    while (src[k] != '\0' && k < STUB_NAME_MAX - 1) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = '\0';
}

static int stub_find(const char* name) {
    int i;

    for (i = 0; i < sStoreCount; i++) {
        if (stub_name_equal(sStore[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int CVarGetInteger(const char* name, int defaultValue) {
    int slot;

    sReadCount++;
    slot = stub_find(name);
    return slot >= 0 ? sStore[slot].value : defaultValue;
}

void CVarSetInteger(const char* name, int value) {
    int slot = stub_find(name);

    if (slot < 0) {
        CHECK(sStoreCount < STUB_SLOTS);
        slot = sStoreCount++;
        stub_copy_name(sStore[slot].name, name);
    }
    sStore[slot].value = value;
}

/* Test helpers. */

static int stub_value(const char* name, int fallback) {
    int slot = stub_find(name);

    return slot >= 0 ? sStore[slot].value : fallback;
}

/* Clears every stored CVar, so every table reads its identity default again, and releases every
 * port so no latch state carries over from the previous test. */
static void reset_all(void) {
    int port;

    sStoreCount = 0;
    gdx_input_actions_mark_dirty();
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    for (port = 0; port < MAXCONTROLLERS; port++) {
        gdx_input_actions_apply(port, 0);
    }
}

/* ---------------------------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------------------------- */

static void test_identity_default(void) {
    int context;
    int output;
    unsigned short all = CONT_MASK;

    reset_all();
    for (context = 0; context < GDX_INPUT_CONTEXT_COUNT; context++) {
        gdx_input_actions_begin_frame(context);
        for (output = 0; output < GDX_INPUT_OUT_COUNT; output++) {
            unsigned short bit = gdx_input_actions_output_bit(output);

            CHECK(bit != 0);
            CHECK(gdx_input_actions_get_mask(context, output) == bit);
            CHECK(gdx_input_actions_apply(0, bit) == bit);
            CHECK(gdx_input_actions_apply(0, 0) == 0);
        }
        CHECK(gdx_input_actions_apply(0, all) == all);
        CHECK(gdx_input_actions_apply(0, 0) == 0);
    }

    /* Bits outside CONT_MASK are never produced by the LUS read and are dropped. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, 0x0080) == 0);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_race_remap_z_fed_by_l(void) {
    reset_all();

    /* Race: Z output is fed by Z or L. L output is unbound in races. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_Z | BTN_L);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_L, 0);
    CHECK(stub_value("gEnhancements.Input.Actions.Race.Z", -1) == (BTN_Z | BTN_L));
    CHECK(stub_value("gEnhancements.Input.Actions.Race.L", -1) == 0);

    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, BTN_Z) == BTN_Z);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Reset restores identity for this context: L is a plain L again in the race. */
    gdx_input_actions_reset_context(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z) == BTN_Z);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_L) == BTN_L);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_L);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_menu_unaffected_by_race_remap(void) {
    reset_all();
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_Z | BTN_L);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_L, 0);

    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_L);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_COURSE_EDIT);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_L);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_latch_across_context_switch(void) {
    unsigned short out;

    reset_all();
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_Z | BTN_L);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_L, 0);
    /* Race B output is fed by A only, so a B press in the race would not fire B. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_B, BTN_A);

    /* L pressed in the race: the race mapping produces Z. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    out = gdx_input_actions_apply(0, 0);
    CHECK(out == 0);
    out = gdx_input_actions_apply(0, BTN_L);
    CHECK(out == BTN_Z);

    /* Switch to a menu while L stays held. The latch keeps the race mapping: no fake release. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);

    /* A menu press while L is still held: A is the menu's A, L keeps its race mapping. */
    CHECK(gdx_input_actions_apply(0, BTN_L | BTN_A) == (BTN_Z | BTN_A));

    /* Back in the race with A released: L keeps its latch, so Z stays on and A drops. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);

    /* Release L: a real release, so Z drops. */
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* B pressed in the menu, then the race starts while B is still held. The race table would
     * not fire B for a fresh press, so the held B must keep the menu mapping. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, BTN_B) == BTN_B);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_B) == BTN_B);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* A fresh B press in the race uses the race table: B is fed by A only, so nothing fires. */
    CHECK(gdx_input_actions_apply(0, BTN_B) == 0);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_ports_latch_independently(void) {
    reset_all();
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_Z | BTN_L);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_L, 0);

    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);
    CHECK(gdx_input_actions_apply(1, 0) == 0);

    /* Port 1 presses L in the menu: plain L. Port 0 still holds its race latch. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);
    CHECK(gdx_input_actions_apply(1, BTN_L) == BTN_L);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(1, 0) == 0);
}

static void test_set_get_and_validation(void) {
    reset_all();

    /* Stored masks are clipped to CONT_MASK plus the spare X and Y bits. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_COURSE_EDIT, GDX_INPUT_OUT_A, 0xFFFF);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_COURSE_EDIT, GDX_INPUT_OUT_A) ==
          (CONT_MASK | GDX_INPUT_BTN_X | GDX_INPUT_BTN_Y));

    /* Out-of-range context or output is ignored. */
    gdx_input_actions_set_mask(99, GDX_INPUT_OUT_A, BTN_B);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_COUNT, BTN_B);
    CHECK(gdx_input_actions_get_mask(99, 0) == 0);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, -1) == 0);
    CHECK(gdx_input_actions_output_bit(GDX_INPUT_OUT_COUNT) == 0);

    /* An unbound output (mask 0) never fires. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_MENU, GDX_INPUT_OUT_START, 0);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, BTN_START) == 0);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Invalid ports pass the raw N64 bits through, and never touch the latch state. */
    CHECK(gdx_input_actions_apply(-1, BTN_A) == BTN_A);
    CHECK(gdx_input_actions_apply(MAXCONTROLLERS, BTN_A) == BTN_A);
    CHECK(gdx_input_actions_apply(-1, BTN_A | GDX_INPUT_BTN_X) == BTN_A);
}

static void test_spare_x_and_y(void) {
    int input;

    reset_all();

    /* Input columns: each output's own bit, then X and Y. */
    for (input = 0; input < GDX_INPUT_OUT_COUNT; input++) {
        CHECK(gdx_input_actions_input_bit(input) == gdx_input_actions_output_bit(input));
    }
    CHECK(gdx_input_actions_input_bit(GDX_INPUT_OUT_COUNT) == GDX_INPUT_BTN_X);
    CHECK(gdx_input_actions_input_bit(GDX_INPUT_OUT_COUNT + 1) == GDX_INPUT_BTN_Y);
    CHECK(gdx_input_actions_input_bit(GDX_INPUT_IN_COUNT) == 0);
    CHECK(gdx_input_actions_input_bit(-1) == 0);

    /* Identity tables: X and Y are dropped, so the game never sees the spare bits. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X | GDX_INPUT_BTN_Y) == 0);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Mapped, X triggers its action in that context only, and keeps its latch while held. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_CUP, BTN_CUP | GDX_INPUT_BTN_X);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X) == BTN_CUP);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_MENU);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X) == BTN_CUP);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X) == 0);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_cache_and_dirty_flag(void) {
    int reads;

    reset_all();

    /* A dirty table re-reads all 3 x 14 CVars once, on the next frame. */
    sReadCount = 0;
    gdx_input_actions_mark_dirty();
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    reads = sReadCount;
    CHECK(reads == GDX_INPUT_CONTEXT_COUNT * GDX_INPUT_OUT_COUNT);

    /* Clean frames and mask reads never touch the CVar bridge. */
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z) == BTN_Z);
    CHECK(sReadCount == reads);

    /* A console-side edit is invisible until the tables are marked dirty. */
    CVarSetInteger("gEnhancements.Input.Actions.Race.A", BTN_B);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_A) == BTN_A);
    gdx_input_actions_mark_dirty();
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_A) == BTN_B);
    /* Race A now fires from B, and B still fires its own identity B output. */
    CHECK(gdx_input_actions_apply(0, BTN_B) == (BTN_A | BTN_B));
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

int main(void) {
    test_identity_default();
    test_race_remap_z_fed_by_l();
    test_menu_unaffected_by_race_remap();
    test_latch_across_context_switch();
    test_ports_latch_independently();
    test_set_get_and_validation();
    test_spare_x_and_y();
    test_cache_and_dirty_flag();
    printf("gdx_input_actions_tests: all tests passed\n");
    return 0;
}
