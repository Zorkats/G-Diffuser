/* port/tests/gdx_input_profiles_tests.c -- standalone tests for the 4A controller profiles.
 *
 * Builds gdx_input_profiles.c and gdx_input_actions.c against an in-memory CVar stub, so no
 * libultraship is linked. The stub is the same one the 3A tests use.
 */

#include "PR/os_pfs.h"
#include "controller.h"

#include "gdx_input_actions.h"
#include "gdx_input_profiles.h"

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
    int slot = stub_find(name);

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

/* Clears every stored CVar, so the tables read their identity defaults again, and releases every
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

/* Profile indices, in UI order (gdx_input_profiles.c). */
#define PROFILE_N64 0
#define PROFILE_WII_CLASSIC 1
#define PROFILE_GAMECUBE 2
#define PROFILE_WII_U 3
#define PROFILE_NINTENDO_CLASSICS 4

/* ---------------------------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------------------------- */

static void test_profile_list(void) {
    int index;

    CHECK(gdx_input_profiles_count() == 5);
    for (index = 0; index < gdx_input_profiles_count(); index++) {
        CHECK(gdx_input_profiles_name(index) != NULL);
        CHECK(gdx_input_profiles_description(index) != NULL);
        CHECK(gdx_input_profiles_limitations(index) != NULL);
    }
    CHECK(gdx_input_profiles_name(-1) == NULL);
    CHECK(gdx_input_profiles_name(gdx_input_profiles_count()) == NULL);
    CHECK(gdx_input_profiles_description(-1) == NULL);
    CHECK(gdx_input_profiles_limitations(gdx_input_profiles_count()) == NULL);
}

static void test_n64_is_identity(void) {
    int output;

    reset_all();
    gdx_input_profiles_apply(PROFILE_N64);
    for (output = 0; output < GDX_INPUT_OUT_COUNT; output++) {
        CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, output) == gdx_input_actions_output_bit(output));
    }
    CHECK(stub_value("gEnhancements.Input.Profile", -1) == PROFILE_N64);
    CHECK(gdx_input_profiles_match() == PROFILE_N64);

    /* Applying N64 over a changed table brings it back to identity. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_L);
    CHECK(gdx_input_profiles_match() == -1);
    gdx_input_profiles_apply(PROFILE_N64);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z) == BTN_Z);
    CHECK(gdx_input_profiles_match() == PROFILE_N64);
}

static void test_each_profile_matches_itself(void) {
    int index;

    reset_all();
    for (index = 0; index < gdx_input_profiles_count(); index++) {
        gdx_input_profiles_apply(index);
        CHECK(gdx_input_profiles_match() == index);
    }
}

static void test_restart_resolves_each_table(void) {
    int index;

    /* With X and Y in the Wii U VC table, no two profiles share a Race table, so a restart that
     * forgets the applied profile still finds the one whose table is loaded. */
    for (index = 0; index < gdx_input_profiles_count(); index++) {
        reset_all();
        gdx_input_profiles_apply(index);
        CVarSetInteger("gEnhancements.Input.Profile", -1);
        CHECK(gdx_input_profiles_match() == index);
    }
}

static void test_spare_buttons(void) {
    reset_all();

    /* N64: X and Y trigger nothing, and the spare bits never reach the game. */
    gdx_input_profiles_apply(PROFILE_N64);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X | GDX_INPUT_BTN_Y) == 0);
    CHECK(gdx_input_actions_apply(0, BTN_A | GDX_INPUT_BTN_X) == BTN_A);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Wii Classic and GameCube: Y is the lap counter (C-left), X is the camera (C-right). */
    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_Y) == BTN_CLEFT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X) == BTN_CRIGHT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    gdx_input_profiles_apply(PROFILE_GAMECUBE);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_Y) == BTN_CLEFT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Wii U VC and Nintendo Classics: Y is rear view (C-up), X is the camera (C-right). */
    gdx_input_profiles_apply(PROFILE_WII_U);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_Y) == BTN_CUP);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    gdx_input_profiles_apply(PROFILE_NINTENDO_CLASSICS);
    CHECK(gdx_input_actions_apply(0, GDX_INPUT_BTN_X) == BTN_CRIGHT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_manual_edit_breaks_match(void) {
    reset_all();
    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);
    CHECK(gdx_input_profiles_match() == PROFILE_WII_CLASSIC);

    /* A Race edit makes the table custom. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, GDX_INPUT_OUT_Z, BTN_Z);
    CHECK(gdx_input_profiles_match() == -1);

    /* Re-applying the profile restores it. */
    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);
    CHECK(gdx_input_profiles_match() == PROFILE_WII_CLASSIC);

    /* Menu and Course Edit are not part of the profile's table, so editing them keeps the match. */
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_MENU, GDX_INPUT_OUT_A, BTN_B);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_COURSE_EDIT, GDX_INPUT_OUT_Z, BTN_L);
    CHECK(gdx_input_profiles_match() == PROFILE_WII_CLASSIC);
}

static void test_apply_leaves_menu_and_course_edit(void) {
    reset_all();
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_MENU, GDX_INPUT_OUT_A, BTN_B);
    gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_COURSE_EDIT, GDX_INPUT_OUT_Z, BTN_L);

    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_MENU, GDX_INPUT_OUT_A) == BTN_B);
    CHECK(gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_COURSE_EDIT, GDX_INPUT_OUT_Z) == BTN_L);
}

static void test_wii_classic_drift_left_from_l(void) {
    reset_all();
    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);

    /* Drift left is fed by the N64 L bit (the Wii Classic L bumper). It is not Z. */
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_Z);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* ZL and ZR send N64 Z and brake. They do not drift. */
    CHECK(gdx_input_actions_apply(0, BTN_Z) == BTN_CDOWN);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* The D-pad toggles the gap display (L output) and stays unused in the race (D-pad output). */
    CHECK(gdx_input_actions_apply(0, BTN_UP) == (BTN_L | BTN_UP));
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* R is drift right, unchanged from the N64 layout. */
    CHECK(gdx_input_actions_apply(0, BTN_R) == BTN_R);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_gamecube_and_nintendo_classics(void) {
    reset_all();

    /* GameCube: the Z button sends N64 R, which is drift right. The C-stick down brakes. */
    gdx_input_profiles_apply(PROFILE_GAMECUBE);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_R) == BTN_R);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, BTN_CDOWN) == BTN_CDOWN);
    CHECK(gdx_input_actions_apply(0, 0) == 0);

    /* Nintendo Classics: L toggles the lap counter with C-left, and the D-pad toggles the gap. */
    gdx_input_profiles_apply(PROFILE_NINTENDO_CLASSICS);
    gdx_input_actions_begin_frame(GDX_INPUT_CONTEXT_RACE);
    CHECK(gdx_input_actions_apply(0, BTN_L) == BTN_CLEFT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
    CHECK(gdx_input_actions_apply(0, BTN_CLEFT) == BTN_CLEFT);
    CHECK(gdx_input_actions_apply(0, 0) == 0);
}

static void test_invalid_index_is_ignored(void) {
    reset_all();
    gdx_input_profiles_apply(PROFILE_WII_CLASSIC);

    gdx_input_profiles_apply(-1);
    gdx_input_profiles_apply(gdx_input_profiles_count());
    CHECK(gdx_input_profiles_match() == PROFILE_WII_CLASSIC);
    CHECK(stub_value("gEnhancements.Input.Profile", -1) == PROFILE_WII_CLASSIC);
}

int main(void) {
    test_profile_list();
    test_n64_is_identity();
    test_each_profile_matches_itself();
    test_restart_resolves_each_table();
    test_spare_buttons();
    test_manual_edit_breaks_match();
    test_apply_leaves_menu_and_course_edit();
    test_wii_classic_drift_left_from_l();
    test_gamecube_and_nintendo_classics();
    test_invalid_index_is_ignored();
    printf("gdx_input_profiles_tests: all tests passed\n");
    return 0;
}
