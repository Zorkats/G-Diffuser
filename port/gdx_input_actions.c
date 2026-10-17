// G-Diffuser: input action layer (3A). See gdx_input_actions.h for the model.
//
// Pure C. The only decomp header is controller.h, for the BTN_* names and CONT_MASK, so the
// standalone test (tests/gdx_input_actions_tests.c) builds this file against a stubbed CVar bridge.

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
#include "gdx_input_actions.h"

#include <stdio.h>

extern int CVarGetInteger(const char* name, int defaultValue); // libultraship consolevariablebridge.h
extern void CVarSetInteger(const char* name, int value);       // libultraship consolevariablebridge.h

// N64 bit of each output, in GdxInputOutput order. Identity for output i is sOutputBits[i].
static const unsigned short sOutputBits[GDX_INPUT_OUT_COUNT] = {
    BTN_A,    BTN_B,     BTN_Z,      BTN_START,  BTN_UP,    BTN_DOWN, BTN_LEFT,
    BTN_RIGHT, BTN_L,    BTN_R,      BTN_CUP,    BTN_CDOWN, BTN_CLEFT, BTN_CRIGHT,
};

// Every bit an input mask may hold: the N64 buttons plus the spare X and Y bits.
#define GDX_INPUT_IN_MASK (CONT_MASK | GDX_INPUT_BTN_X | GDX_INPUT_BTN_Y)

// CVar name segments. Changing one of these renames saved settings, so keep them stable.
static const char* const sContextKeys[GDX_INPUT_CONTEXT_COUNT] = { "Menu", "Race", "CourseEdit" };
static const char* const sOutputKeys[GDX_INPUT_OUT_COUNT] = {
    "A",     "B",     "Z",      "Start",  "DUp",    "DDown", "DLeft",
    "DRight", "L",    "R",      "CUp",    "CDown",  "CLeft", "CRight",
};

static unsigned short sMask[GDX_INPUT_CONTEXT_COUNT][GDX_INPUT_OUT_COUNT];
static int sDirty = 1;
static int sFrameContext = GDX_INPUT_CONTEXT_MENU;

// Per-port edge state. sPrevRaw is last frame's raw pad. sLatched[port][c] holds the input bits
// that were pressed while context c was current and are still held.
static unsigned short sPrevRaw[MAXCONTROLLERS];
static unsigned short sLatched[MAXCONTROLLERS][GDX_INPUT_CONTEXT_COUNT];

static int gdx_input_actions_context_valid(int context) {
    return context >= 0 && context < GDX_INPUT_CONTEXT_COUNT;
}

static int gdx_input_actions_output_valid(int output) {
    return output >= 0 && output < GDX_INPUT_OUT_COUNT;
}

static void gdx_input_actions_build_name(char* buffer, size_t size, int context, int output) {
    snprintf(buffer, size, "gEnhancements.Input.Actions.%s.%s", sContextKeys[context], sOutputKeys[output]);
}

static void gdx_input_actions_reload(void) {
    int c;
    int o;

    for (c = 0; c < GDX_INPUT_CONTEXT_COUNT; c++) {
        for (o = 0; o < GDX_INPUT_OUT_COUNT; o++) {
            char name[96];
            int value;

            gdx_input_actions_build_name(name, sizeof(name), c, o);
            value = CVarGetInteger(name, sOutputBits[o]);
            sMask[c][o] = (unsigned short) (value & GDX_INPUT_IN_MASK);
        }
    }
    sDirty = 0;
}

static void gdx_input_actions_refresh_if_dirty(void) {
    if (sDirty) {
        gdx_input_actions_reload();
    }
}

void gdx_input_actions_begin_frame(int context) {
    gdx_input_actions_refresh_if_dirty();
    sFrameContext = gdx_input_actions_context_valid(context) ? context : GDX_INPUT_CONTEXT_MENU;
}

unsigned short gdx_input_actions_apply(int port, unsigned short rawButtons) {
    unsigned short raw = (unsigned short) (rawButtons & GDX_INPUT_IN_MASK);
    unsigned short pressed;
    unsigned short released;
    unsigned short out = 0;
    int c;
    int o;

    if (port < 0 || port >= MAXCONTROLLERS) {
        return (unsigned short) (raw & CONT_MASK);
    }

    pressed = (unsigned short) (raw & ~sPrevRaw[port]);
    released = (unsigned short) (sPrevRaw[port] & ~raw);
    sPrevRaw[port] = raw;

    // A release forgets the latch on every context. A press latches the current frame context.
    // Bits that stay held keep their latch, so a context switch cannot change their mapping.
    for (c = 0; c < GDX_INPUT_CONTEXT_COUNT; c++) {
        sLatched[port][c] = (unsigned short) (sLatched[port][c] & ~released);
    }
    sLatched[port][sFrameContext] = (unsigned short) (sLatched[port][sFrameContext] | pressed);

    for (c = 0; c < GDX_INPUT_CONTEXT_COUNT; c++) {
        unsigned short held = (unsigned short) (raw & sLatched[port][c]);

        if (held == 0) {
            continue;
        }
        for (o = 0; o < GDX_INPUT_OUT_COUNT; o++) {
            if ((sMask[c][o] & held) != 0) {
                out = (unsigned short) (out | sOutputBits[o]);
            }
        }
    }
    return out;
}

unsigned short gdx_input_actions_get_mask(int context, int output) {
    if (!gdx_input_actions_context_valid(context) || !gdx_input_actions_output_valid(output)) {
        return 0;
    }
    gdx_input_actions_refresh_if_dirty();
    return sMask[context][output];
}

void gdx_input_actions_set_mask(int context, int output, unsigned short mask) {
    char name[96];
    unsigned short clean;

    if (!gdx_input_actions_context_valid(context) || !gdx_input_actions_output_valid(output)) {
        return;
    }
    gdx_input_actions_refresh_if_dirty();
    clean = (unsigned short) (mask & GDX_INPUT_IN_MASK);
    gdx_input_actions_build_name(name, sizeof(name), context, output);
    CVarSetInteger(name, clean);
    sMask[context][output] = clean;
}

void gdx_input_actions_reset_context(int context) {
    int o;

    if (!gdx_input_actions_context_valid(context)) {
        return;
    }
    for (o = 0; o < GDX_INPUT_OUT_COUNT; o++) {
        gdx_input_actions_set_mask(context, o, sOutputBits[o]);
    }
}

unsigned short gdx_input_actions_output_bit(int output) {
    return gdx_input_actions_output_valid(output) ? sOutputBits[output] : 0;
}

unsigned short gdx_input_actions_input_bit(int input) {
    if (input >= 0 && input < GDX_INPUT_OUT_COUNT) {
        return sOutputBits[input];
    }
    if (input == GDX_INPUT_OUT_COUNT) {
        return GDX_INPUT_BTN_X;
    }
    return input == GDX_INPUT_OUT_COUNT + 1 ? GDX_INPUT_BTN_Y : 0;
}

void gdx_input_actions_mark_dirty(void) {
    sDirty = 1;
}
